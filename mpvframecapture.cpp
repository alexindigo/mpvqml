// SPDX-License-Identifier: LGPL-3.0-only
#include "mpvframecapture.h"
#include "mpvbackendcore_p.h"

#include <QFile>
#include <QImage>
#include <QPointer>
#include <QSize>
#include <QTemporaryFile>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <mpv/render.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace MpvQmlPrivate;
using Clock = std::chrono::steady_clock;
constexpr auto CaptureBudget = std::chrono::seconds(7);
constexpr int MaxEdge = 4096;
constexpr int MaxJobs = 2;

enum class Phase { Working, Cancelled, Publishing };
enum class Stage { Input, Core, Context, Load, InitialFrame, Seek, SeekFrame, Render, Encode, Publish, Complete, Cancelled };

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
  }
  Q_UNREACHABLE();
}

struct Result {
  QString code;
  QString reason;
  Stage stage = Stage::Complete;
  int mpvError = 0;
  int systemError = 0;
};

struct Descriptor {
  int fd = -1;
  Descriptor() = default;
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  ~Descriptor() { if (fd >= 0) ::close(fd); }
};

Result mpvFailure(Stage stage, QString code, int error) {
  return {std::move(code), QString::fromLatin1(mpv_error_string(error)), stage, error};
}

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

// This private core has just one load and at most one explicit seek. Native
// restarts have no token: require the appropriate lifecycle event before a
// restart, independently of the correlated command reply. A reply is not a
// readiness event, and seeing a restart before SEEK cannot complete a seek.
class FrameEvents {
public:
  static constexpr uint64_t Load = 1, Seek = 2;

  void beginSeek() { m_state = {Seek}; }

  Stage stage() const {
    if (m_state.token == Load)
      return m_state.acknowledged ? Stage::InitialFrame : Stage::Load;
    return m_state.acknowledged ? Stage::SeekFrame : Stage::Seek;
  }

  std::optional<Result> accept(const mpv_event &event) {
    switch (event.event_id) {
    case MPV_EVENT_COMMAND_REPLY:
      if (event.reply_userdata != m_state.token || m_state.acknowledged)
        break;
      if (event.error < 0)
        return m_state.token == Load ? mpvFailure(Stage::Load, QStringLiteral("open-failed"), event.error)
                                     : mpvFailure(Stage::Seek, QStringLiteral("seek-failed"), event.error);
      m_state.acknowledged = true;
      break;
    case MPV_EVENT_FILE_LOADED:
    case MPV_EVENT_SEEK:
      if (event.event_id == (m_state.token == Load ? MPV_EVENT_FILE_LOADED : MPV_EVENT_SEEK)) {
        m_state.started = true;
        m_state.restarted = false;
      }
      break;
    case MPV_EVENT_PLAYBACK_RESTART:
      m_state.restarted = m_state.started;
      break;
    case MPV_EVENT_END_FILE: {
      const auto *end = static_cast<const mpv_event_end_file *>(event.data);
      return mpvFailure(frameStage(), QStringLiteral("no-frame"),
                        end && end->error < 0 ? end->error : MPV_ERROR_NOTHING_TO_PLAY);
    }
    case MPV_EVENT_SHUTDOWN:
      return Result{QStringLiteral("no-frame"), QStringLiteral("Native core shut down"), stage()};
    case MPV_EVENT_QUEUE_OVERFLOW:
      return mpvFailure(stage(), QStringLiteral("no-frame"), MPV_ERROR_EVENT_QUEUE_FULL);
    default:
      break;
    }
    return std::nullopt;
  }

  bool ready() const { return m_state.acknowledged && m_state.restarted; }
  bool initial() const { return m_state.token == Load; }

private:
  Stage frameStage() const { return initial() ? Stage::InitialFrame : Stage::SeekFrame; }

  struct Transition {
    uint64_t token;
    bool acknowledged = false;
    bool started = false;
    bool restarted = false;
  } m_state{Load};
};

bool isFileUrl(const QUrl &url) {
  return url.isLocalFile() && url.host().isEmpty() && !url.hasQuery() && !url.hasFragment()
      && url.toLocalFile().startsWith(QLatin1Char('/')) && !url.toLocalFile().contains(QChar::Null);
}

QSize videoSize(mpv_handle *mpv, QSize bounds) {
  int64_t width = 0;
  int64_t height = 0;
  if (mpv_get_property(mpv, "video-out-params/dw", MPV_FORMAT_INT64, &width) < 0
      || mpv_get_property(mpv, "video-out-params/dh", MPV_FORMAT_INT64, &height) < 0
      || width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX)
    return {};
  if (bounds.width() >= width && bounds.height() >= height)
    return QSize(int(width), int(height));
  return QSize(int(width), int(height)).scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

Result renderFrame(mpv_handle *mpv, mpv_render_context *render, QSize bounds, QImage &image) {
  const QSize size = videoSize(mpv, bounds);
  if (!size.isValid())
    return {QStringLiteral("no-frame"), QStringLiteral("No video dimensions"), Stage::Render};
  image = QImage(size, QImage::Format_RGBX8888);
  if (image.isNull())
    return {QStringLiteral("render-failed"), QStringLiteral("Cannot allocate the bounded frame"), Stage::Render};
  int dimensions[] = {size.width(), size.height()};
  size_t stride = image.bytesPerLine();
  int block = 0;
  char pixelFormat[] = "rgb0";
  mpv_render_param options[] = {
      {MPV_RENDER_PARAM_SW_SIZE, dimensions}, {MPV_RENDER_PARAM_SW_FORMAT, pixelFormat},
      {MPV_RENDER_PARAM_SW_STRIDE, &stride}, {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {MPV_RENDER_PARAM_INVALID, nullptr}};
  // No ADVANCED_CONTROL: this private thread may use both core and renderer.
  const int error = mpv_render_context_render(render, options);
  if (error < 0)
    return mpvFailure(Stage::Render, QStringLiteral("render-failed"), error);
  for (int y = 0; y < image.height(); ++y) {
    auto *row = image.scanLine(y);
    for (int x = 0; x < image.width(); ++x)
      row[x * 4 + 3] = 255;
  }
  return {};
}

Result readFrame(mpv_handle *mpv, mpv_render_context *render, const QByteArray &url,
                 double fraction, QSize bounds, const Budget &budget, QImage &image) {
  if (auto stopped = budget.stop(Stage::Load))
    return *stopped;
  const char *load[] = {"loadfile", url.constData(), nullptr};
  int error = mpv_command_async(mpv, FrameEvents::Load, load);
  if (error < 0)
    return mpvFailure(Stage::Load, QStringLiteral("open-failed"), error);

  FrameEvents events;
  for (;;) {
    if (auto stopped = budget.stop(events.stage()))
      return *stopped;
    if (auto failed = events.accept(*mpv_wait_event(mpv, 0.01)))
      return *failed;
    if (auto stopped = budget.stop(events.stage()))
      return *stopped;
    if (events.ready()) {
      double duration = 0;
      if (!(events.initial() && fraction > 0
            && mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0
            && std::isfinite(duration) && duration >= 1)) {
        if (auto stopped = budget.stop(Stage::Render))
          return *stopped;
        return renderFrame(mpv, render, bounds, image);
      }

      if (auto stopped = budget.stop(Stage::Seek))
        return *stopped;
      const QByteArray position = QByteArray::number(duration * fraction, 'g', 17);
      const char *seek[] = {"seek", position.constData(), "absolute+exact", nullptr};
      error = mpv_command_async(mpv, FrameEvents::Seek, seek);
      if (error < 0)
        return mpvFailure(Stage::Seek, QStringLiteral("seek-failed"), error);
      events.beginSeek();
    }
    // The VO must be allowed to complete a flip before the readiness event.
    // Skip display, not the native frame handoff; libmpv keeps that frame for
    // the eventual memory render (vo_libmpv.c on the supported libmpv).
    if (mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
      int skip = 1;
      int block = 0;
      mpv_render_param options[] = {{MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
          {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {MPV_RENDER_PARAM_INVALID, nullptr}};
      error = mpv_render_context_render(render, options);
      if (error < 0)
        return mpvFailure(Stage::Render, QStringLiteral("render-failed"), error);
    }
  }
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
  std::mutex lock;
  MpvFrameCapture *receiver = nullptr; // Read only under lock off the object's thread.
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

  Result run(MpvBackendCore &core, Descriptor &input) {
    const Budget budget{phase, deadline};
    if (auto stopped = budget.stop(Stage::Input))
      return *stopped;
    input.fd = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat info {};
    const int statError = input.fd < 0 || ::fstat(input.fd, &info) != 0 ? errno : 0;
    if (statError || !S_ISREG(info.st_mode))
      return {QStringLiteral("unreadable"), QStringLiteral("Cannot open a readable regular video file"), Stage::Input, 0, statError};
    if (auto stopped = budget.stop(Stage::Core))
      return *stopped;
    const auto diagnostic = core.initialize({
        {"config", "no"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"ytdl", "no"}, {"access-references", "no"}, {"vo", "libmpv"},
        {"aid", "no"}, {"sid", "no"}, {"audio-file-auto", "no"}, {"sub-auto", "no"},
        {"pause", "yes"}, {"keep-open", "yes"}, {"hwdec", "no"}, {"vd-lavc-threads", "1"},
        // Limit decoded source pixels independently of the caller's output
        // bounds. 64 MiPixels also admits 8K UHD sources.
        {"vd-lavc-o", "max_pixels=67108864"}, {"cache", "no"},
        {"osd-level", "0"}, {"osc", "no"}, {"load-stats-overlay", "no"},
        {"load-osd-console", "no"}, {"load-auto-profiles", "no"}});
    if (diagnostic.error < 0)
      return {QStringLiteral("initialization-failed"), diagnostic.message, Stage::Core, diagnostic.error};
    if (auto stopped = budget.stop(Stage::Context))
      return *stopped;
    auto *mpv = core.handle();
    mpv_render_context *context = nullptr;
    char api[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, api}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    const int error = mpv_render_context_create(&context, mpv, params);
    if (error < 0)
      return mpvFailure(Stage::Context, QStringLiteral("initialization-failed"), error);
    std::unique_ptr<mpv_render_context, decltype(&mpv_render_context_free)> render(context, mpv_render_context_free);
    QImage image;
    auto result = readFrame(mpv, render.get(), QByteArray("fd://") + QByteArray::number(input.fd), fraction, bounds, budget, image);
    if (!result.code.isEmpty())
      return result;
    return writeFrame(destination, image, budget, phase);
  }
};

MpvFrameCapture::MpvFrameCapture(QObject *parent)
    : QObject(parent), m_dispatch(std::make_shared<Dispatch>()) {
  m_dispatch->receiver = this;
}

MpvFrameCapture::~MpvFrameCapture() {
  for (const auto &job : std::as_const(m_activeJobs)) {
    Phase working = Phase::Working;
    job->phase.compare_exchange_strong(working, Phase::Cancelled);
  }
  std::lock_guard lock(m_dispatch->lock);
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
    m_activeJobs.insert(id, job);
    std::thread([job, dispatch = m_dispatch, permit = std::move(permit)]() mutable {
      Result result;
      {
        Descriptor input;
        result = job->run(*permit, input);
        // The render context is gone, but fd:// still needs its input descriptor
        // through final native teardown. Release both before announcing capacity.
        permit->finishExclusive();
      }
      permit.reset();
      std::lock_guard lock(dispatch->lock);
      auto *receiver = dispatch->receiver;
      if (receiver)
        QMetaObject::invokeMethod(receiver, [receiver, job, result = std::move(result)] {
           receiver->retired(job, Outcome(std::move(result)));
        }, Qt::QueuedConnection);
    }).detach();
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
  // Keep the slot occupied until native cleanup; cancellation delivery itself
  // never waits for a decoder or a filesystem operation.
  QMetaObject::invokeMethod(this, [this, job = std::move(job)] {
    deliverCancellation(job);
  }, Qt::QueuedConnection);
  return true;
}
