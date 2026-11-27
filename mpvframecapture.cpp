// SPDX-License-Identifier: LGPL-3.0-only
#include "mpvframecapture.h"
#include "mpvbackendcore_p.h"

#include <QFile>
#include <QImage>
#include <QPointer>
#include <QScopeGuard>
#include <QSize>
#include <QTemporaryFile>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <mpv/client.h>
#include <mutex>
#include <optional>
#include "capture/capture-wire.h"
#include "capture/pool.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <deque>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace MpvQmlPrivate;
using namespace MpvCapture;
constexpr int MaxJobs = 2;

enum class Phase { Working, Cancelled, Publishing };

const char *stageName(Stage stage) {
  switch (stage) {
  case Stage::Input: return "open-input";
  case Stage::Core: return "initialize-core";
  case Stage::Context: return "initialize-render";
  case Stage::Load: return "load-command";
  case Stage::InitialFrame: return "initial-frame";
  case Stage::Seek: return "seek-command";
  case Stage::SeekFrame: return "seek-frame";
  case Stage::Render: return "render";
  case Stage::Encode: return "encode";
  case Stage::Publish: return "publish";
  case Stage::Complete: return "complete";
  case Stage::Cancelled: return "cancelled";
  case Stage::Helper: return "helper";
  }
  Q_UNREACHABLE();
}

struct Descriptor {
  int fd = -1;
  Descriptor() = default;
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  ~Descriptor() { if (fd >= 0) ::close(fd); }
};

struct Budget {
  const std::atomic<Phase> &phase;
  Clock::time_point deadline;

  std::optional<Result> stop(Stage stage) const {
    if (phase.load() != Phase::Working)
      return Result{}; // The facade owns delivery of cancellation.
    if (Clock::now() >= deadline)
      return Result{QStringLiteral("timeout"), QStringLiteral("Capture deadline exceeded"), stage};
    return std::nullopt;
  }
};

bool isFileUrl(const QUrl &url) {
  return url.isLocalFile() && url.host().isEmpty() && !url.hasQuery() && !url.hasFragment()
      && url.toLocalFile().startsWith(QLatin1Char('/')) && !url.toLocalFile().contains(QChar::Null);
}

Result writeFrame(const QString &destination, const QImage &image, const Budget &budget,
                  std::atomic<Phase> &phase) {
  if (auto stopped = budget.stop(Stage::Encode))
    return *stopped;
  const auto slash = destination.lastIndexOf(QLatin1Char('/'));
  const QByteArray name = QFile::encodeName(destination.mid(slash + 1));
  if (name.isEmpty() || name == "." || name == "..")
    return {QStringLiteral("write-failed"), QStringLiteral("Destination must name an unused file"), Stage::Publish};
  Descriptor directory;
  const auto parent = QFile::encodeName(destination.left(slash + 1));
  directory.fd = ::open(parent.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (directory.fd < 0)
    return {QStringLiteral("write-failed"), QStringLiteral("Cannot open staging directory"), Stage::Publish, 0, errno};
  if (auto stopped = budget.stop(Stage::Encode))
    return *stopped;

  // Both names resolve in the same held directory, including descriptor-path
  // parents supplied by a worker's caller. No prior existence check is used
  // to authorize the commit: linkat fails atomically if any name is occupied.
  const auto templatePath = QStringLiteral("/proc/self/fd/%1/.mpvqml-XXXXXX").arg(directory.fd);
  QTemporaryFile file(templatePath);
  if (!file.open())
    return {QStringLiteral("write-failed"), file.errorString(), Stage::Encode};
  if (auto stopped = budget.stop(Stage::Encode))
    return *stopped;
  if (!image.save(&file, "PNG") || !file.flush())
    return {QStringLiteral("write-failed"), file.errorString(), Stage::Encode};
  if (auto stopped = budget.stop(Stage::Encode))
    return *stopped;
  const auto path = file.fileName(); // Materialize a name even with O_TMPFILE.
  const auto temporaryName = QFile::encodeName(path.mid(path.lastIndexOf(QLatin1Char('/')) + 1));
  if (temporaryName.isEmpty())
    return {QStringLiteral("write-failed"), file.errorString(), Stage::Encode};
  if (auto stopped = budget.stop(Stage::Publish))
    return *stopped;
  Phase working = Phase::Working;
  if (!phase.compare_exchange_strong(working, Phase::Publishing))
    return {};
  if (::linkat(directory.fd, temporaryName.constData(), directory.fd, name.constData(), 0) < 0) {
    const int error = errno;
    return {QStringLiteral("write-failed"), error == EEXIST ? QStringLiteral("Staging target already exists")
            : QStringLiteral("Cannot publish staged frame"), Stage::Publish, 0, error};
  }
  return {}; // QTemporaryFile only removes its own name, not the committed link.
}
} // namespace

struct MpvFrameCapture::Outcome : Result {
  Outcome(Result result) : Result(std::move(result)) {}

  QVariantMap detail() const {
    return {{QStringLiteral("code"), code}, {QStringLiteral("reason"), reason},
            {QStringLiteral("stage"), QString::fromLatin1(stageName(stage))},
            {QStringLiteral("mpvError"), mpvError}, {QStringLiteral("systemError"), systemError}};
  }
};

struct MpvFrameCapture::Dispatch {
  MpvFrameCapture *receiver = nullptr; // Facade and queue thread only.
};

struct MpvFrameCapture::Admission {
  AdmissionStatus status;
  int result; // positive ID or legacy negative error
  QString reason;
};

struct MpvFrameCapture::Job {
  int id;
  QString source;
  QString destination;
  double fraction;
  QSize bounds;
  Clock::time_point deadline;
  std::atomic<Phase> phase{Phase::Working};
  bool cancellationDelivered = false; // GUI thread only.

  std::shared_ptr<Dispatch> delivery;
  QPointer<Queue> queue;
  std::unique_ptr<QTimer> queuedTimer;
  std::unique_ptr<MpvBackendCore> permit;
  Fd output;
  Result result;
  QSize size;
  int poolTicket = 0;
};

class MpvFrameCapture::Queue final : public QObject {
  using Work = std::shared_ptr<Job>;
  struct WorkerDispatch {
    std::mutex lock;
    Queue *receiver = nullptr;
  };

public:
  static Queue &current() {
    static thread_local QPointer<Queue> queue;
    if (!queue) {
      auto *app = QCoreApplication::instance();
      auto *thread = QThread::currentThread();
      queue = new Queue(app && app->thread() == thread ? app : nullptr);
      if (!queue->parent())
        connect(thread, &QThread::finished, queue, &QObject::deleteLater);
    }
    return *queue;
  }

  void enqueue(const Work &job) {
    // Admission is already bounded by the original shared lease ledger.
    job->queuedTimer = std::make_unique<QTimer>();
    job->queuedTimer->setSingleShot(true);
    job->queuedTimer->setTimerType(Qt::PreciseTimer);
    connect(job->queuedTimer.get(), &QTimer::timeout, this, [this, weak = std::weak_ptr<Job>(job)] {
      if (auto job = weak.lock(); job && removeWaiting(job))
        finish(job, deadline());
    });
    waiting.push_back(job);
    job->queuedTimer->start(std::chrono::ceil<std::chrono::milliseconds>(job->deadline - Clock::now()));
    drain();
  }

  void cancel(const Work &job) {
    if (removeWaiting(job)) {
      finish(job, {});
      return;
    }
    if (job->poolTicket)
      pool.cancel(job->poolTicket);
  }

private:
  explicit Queue(QObject *parent) : QObject(parent), workers(std::make_shared<WorkerDispatch>()) {
    workers->receiver = this;
    connect(&pool, &Pool::capacityReleased, this, [this](int ticket) {
      auto entry = native.extract(ticket);
      if (!entry.empty()) {
        auto job = std::move(entry.mapped());
        job->poolTicket = 0;
        if (job->phase.load() != Phase::Working || !job->result.code.isEmpty()) {
          settle(job, std::move(job->result));
        } else {
          runWorker(job, [](const Work &job) {
            auto result = publish(job);
            return [job, result = std::move(result)](Queue &queue) mutable { queue.settle(job, std::move(result)); };
          });
        }
      }
    });
  }

  ~Queue() override {
    std::lock_guard lock(workers->lock);
    workers->receiver = nullptr;
  }

  static Result deadline() { return {"deadline", "Capture helper deadline exceeded", Stage::Helper}; }

  bool removeWaiting(const Work &job) {
    auto it = std::find(waiting.begin(), waiting.end(), job);
    if (it == waiting.end())
      return false;
    job->queuedTimer->stop();
    waiting.erase(it);
    return true;
  }

  template<class Function>
  void runWorker(const Work &job, Function work) {
    try {
      std::thread([dispatch = workers, job, work = std::move(work)] {
        auto response = work(job);
        std::lock_guard lock(dispatch->lock);
        if (auto *queue = dispatch->receiver)
          QMetaObject::invokeMethod(queue, [queue, response = std::move(response)]() mutable { response(*queue); },
                                    Qt::QueuedConnection);
      }).detach();
    } catch (const std::system_error &) {
      settle(job, {"helper-unavailable", "Cannot start capture filesystem worker", Stage::Helper});
    } catch (const std::bad_alloc &) {
      settle(job, {"helper-unavailable", "Cannot allocate capture filesystem work", Stage::Helper});
    }
  }

  void drain() {
    while (active < MaxJobs && !waiting.empty()) {
      auto job = std::move(waiting.front());
      waiting.pop_front();
      job->queuedTimer.reset();
      if (Clock::now() >= job->deadline) {
        finish(job, deadline());
        continue;
      }
      ++active;
      runWorker(job, [](const Work &job) {
        Fd input;
        Result result;
        if (job->phase.load() == Phase::Working && Clock::now() >= job->deadline) {
          result = deadline();
        } else if (job->phase.load() == Phase::Working) {
          input = Fd(::open(QFile::encodeName(job->source).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
          struct stat info {};
          const int error = !input || ::fstat(input.get(), &info) != 0 ? errno : 0;
          if (error || !S_ISREG(info.st_mode))
            result = {"unreadable", "Cannot open a readable regular video file", Stage::Input, 0, error};
        }
        return [job, input = std::move(input), result = std::move(result)](Queue &queue) mutable {
          queue.prepared(job, std::move(input), std::move(result));
        };
      });
    }
  }

  void prepared(const Work &job, Fd input, Result result) {
    if (job->phase.load() != Phase::Working || !result.code.isEmpty()) {
      settle(job, std::move(result));
      return;
    }
    const auto now = Clock::now();
    if (now >= job->deadline) {
      settle(job, deadline());
      return;
    }
    job->output = Fd(::memfd_create("mpvqml-frame", MFD_ALLOW_SEALING | MFD_CLOEXEC));
    Fd writable;
    if (!job->output || ::ftruncate(job->output.get(), pixelBytes(job->bounds)) < 0
        || ::fcntl(job->output.get(), F_ADD_SEALS, F_SEAL_GROW) < 0
        || !(writable = Fd(::fcntl(job->output.get(), F_DUPFD_CLOEXEC, 0)))) {
      settle(job, {"render-failed", "Cannot reserve the capture pixel buffer", Stage::Render, 0, errno});
      return;
    }
    Frame request{Kind::Request, 0, encodeSpec({job->bounds, job->fraction, job->deadline}), {}};
    request.fds.push_back(std::move(input));
    request.fds.push_back(std::move(writable));
    const auto ticket = pool.submit(std::move(request), job->deadline,
        [job](JobReply reply) {
          if (reply.error)
            job->result = {std::move(reply.error.code), std::move(reply.error.detail), Stage::Helper};
          else
            decodeResult(reply.frame, job->result, job->size); // validated before consuming ticket
        }, [job](const Frame &frame) {
          Result result;
          QSize size;
          return decodeResult(frame, result, size) && (!result.code.isEmpty()
              || (size.width() <= job->bounds.width() && size.height() <= job->bounds.height()
                  && validOutput(job->output.get(), size, true)));
        });
    if (ticket.status != Pool::Admission::Accepted) {
      settle(job, Clock::now() >= job->deadline ? deadline()
          : Result{"helper-unavailable", "Could not admit the capture helper request", Stage::Helper});
      return;
    }
    job->poolTicket = ticket.id;
    native.emplace(ticket.id, job);
  }

  static Result publish(const Work &job) {
    const Budget budget{job->phase, job->deadline};
    if (auto stopped = budget.stop(Stage::Encode))
      return *stopped;
    const auto bytes = size_t(pixelBytes(job->size));
    const auto *pixels = static_cast<const uchar *>(::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, job->output.get(), 0));
    if (pixels == MAP_FAILED)
      return {"render-failed", "Cannot map captured pixels", Stage::Render, 0, errno};
    const auto unmap = qScopeGuard([=] { ::munmap(const_cast<uchar *>(pixels), bytes); });
    const QImage image(pixels, job->size.width(), job->size.height(), job->size.width() * 4,
                       QImage::Format_RGBX8888);
    return writeFrame(job->destination, image, budget, job->phase);
  }

  void settle(const Work &job, Result result) {
    --active;
    finish(job, std::move(result));
    drain();
  }

  static void finish(const Work &job, Result result) {
    job->output = {};
    job->permit.reset();
    if (auto *receiver = job->delivery->receiver)
      QMetaObject::invokeMethod(receiver, [receiver, job, result = std::move(result)] {
        receiver->retired(job, Outcome(std::move(result)));
      }, Qt::QueuedConnection);
  }

  Pool pool;
  std::shared_ptr<WorkerDispatch> workers;
  std::deque<Work> waiting;
  std::unordered_map<int, Work> native;
  int active = 0;
};

MpvFrameCapture::MpvFrameCapture(QObject *parent)
    : QObject(parent), m_dispatch(std::make_shared<Dispatch>()) {
  m_dispatch->receiver = this;
}

MpvFrameCapture::~MpvFrameCapture() {
  for (const auto &job : std::as_const(m_activeJobs)) {
    Phase working = Phase::Working;
    if (job->phase.compare_exchange_strong(working, Phase::Cancelled) && job->queue)
      job->queue->cancel(job);
  }
  m_dispatch->receiver = nullptr;
}

int MpvFrameCapture::capture(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                            const QUrl &destination) {
  return admit(source, fraction, maxWidth, maxHeight, destination).result;
}

QVariantMap MpvFrameCapture::tryCapture(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                                        const QUrl &destination) {
  const auto admission = admit(source, fraction, maxWidth, maxHeight, destination);
  return {{QStringLiteral("status"), int(admission.status)},
          {QStringLiteral("id"), admission.status == Accepted ? admission.result : 0},
          {QStringLiteral("error"), admission.status == Accepted ? MPV_ERROR_SUCCESS : admission.result},
          {QStringLiteral("reason"), admission.reason}};
}

bool MpvFrameCapture::capacityAvailable() const {
  return m_lastId < std::numeric_limits<int>::max() && m_activeJobs.size() < MaxJobs
      && MpvBackendCore::hasCapacity();
}

MpvFrameCapture::Admission MpvFrameCapture::admit(const QUrl &source, double fraction,
                                                 int maxWidth, int maxHeight, const QUrl &destination) {
  if (!isFileUrl(source) || !isFileUrl(destination) || source == destination
       || !std::isfinite(fraction) || fraction < 0 || fraction >= 1
       || maxWidth < 1 || maxHeight < 1 || maxWidth > MaxEdge || maxHeight > MaxEdge)
    return {InvalidArguments, MPV_ERROR_INVALID_PARAMETER, QStringLiteral("Invalid capture arguments")};
  if (m_lastId == std::numeric_limits<int>::max())
    return {IdExhausted, MPV_ERROR_EVENT_QUEUE_FULL, QStringLiteral("Capture request IDs are exhausted")};
  if (m_activeJobs.size() >= MaxJobs)
    return {LocalBusy, MPV_ERROR_EVENT_QUEUE_FULL, QStringLiteral("All local capture slots are occupied")};

  MpvCoreDiagnostic diagnostic;
  auto permit = MpvBackendCore::reserve(MpvBackendCore::Retirement::Exclusive, diagnostic);
  if (!permit) {
    const auto status = diagnostic.stage == MpvCoreStage::Capacity ? SharedBusy
        : diagnostic.stage == MpvCoreStage::Allocation ? AllocationFailed : BackendUnavailable;
    return {status, diagnostic.error, diagnostic.message};
  }
  const int id = ++m_lastId;
  try {
    auto job = std::shared_ptr<Job>(new Job{id, source.toLocalFile(), destination.toLocalFile(), fraction,
        QSize(maxWidth, maxHeight), Clock::now() + CaptureBudget});
    job->permit = std::move(permit);
    job->delivery = m_dispatch;
    job->queue = &Queue::current();
    m_activeJobs.insert(id, job);
    job->queue->enqueue(job);
  } catch (const std::bad_alloc &) {
    m_activeJobs.remove(id);
    return {AllocationFailed, MPV_ERROR_NOMEM, QStringLiteral("Cannot allocate capture job ownership")};
  } catch (const std::system_error &) {
    m_activeJobs.remove(id);
    return {WorkerUnavailable, MPV_ERROR_GENERIC, QStringLiteral("Cannot start capture worker")};
  }
  return {Accepted, id, {}};
}

void MpvFrameCapture::retired(const std::shared_ptr<Job> &job, const Outcome &outcome) {
  if (!m_activeJobs.remove(job->id))
    return;
  const QPointer<MpvFrameCapture> guard(this);
  if (job->phase.load() == Phase::Cancelled)
    deliverCancellation(job);
  else
    deliver(job->id, outcome);
  if (guard)
    emit capacityReleased(job->id);
}

void MpvFrameCapture::deliver(int id, const Outcome &outcome) {
  const QPointer<MpvFrameCapture> guard(this);
  emit finished(id, outcome.code, outcome.reason);
  if (guard)
    emit finishedDetailed(id, outcome.detail());
}

void MpvFrameCapture::deliverCancellation(const std::shared_ptr<Job> &job) {
  // Retirement may already have been queued when cancel() won the gate.
  // Either callback can deliver the cancellation; both share one ledger.
  if (std::exchange(job->cancellationDelivered, true))
    return;
  deliver(job->id, Result{QStringLiteral("cancelled"), QStringLiteral("Capture cancelled"), Stage::Cancelled});
}

bool MpvFrameCapture::cancel(int id) {
  auto job = m_activeJobs.value(id);
  Phase working = Phase::Working;
  if (!job || !job->phase.compare_exchange_strong(working, Phase::Cancelled))
    return false;
  if (job->queue)
    job->queue->cancel(job);
  // Keep capacity occupied until retirement, without delaying cancellation.
  QMetaObject::invokeMethod(this, [this, job = std::move(job)] {
    deliverCancellation(job);
  }, Qt::QueuedConnection);
  return true;
}
