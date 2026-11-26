// SPDX-License-Identifier: LGPL-3.0-only
#include "mpvframecapture.h"
#include "mpvbackendcore_p.h"

#include <QFile>
#include <QImage>
#include <QPointer>
#include <QSaveFile>
#include <QSize>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <mutex>
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
struct Result {
  QString code;
  QString reason;
};

struct Input {
  int fd = -1;
  ~Input() { if (fd >= 0) ::close(fd); }
};

Result mpvFailure(QString code, int error) {
  return {std::move(code), QString::fromLatin1(mpv_error_string(error))};
}

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

Result readFrame(mpv_handle *mpv, mpv_render_context *render, const QByteArray &url,
                 double fraction, QSize bounds, const std::atomic<Phase> &phase,
                 Clock::time_point deadline, QImage &image) {
  const char *load[] = {"loadfile", url.constData(), nullptr};
  int error = mpv_command_async(mpv, 1, load);
  if (error < 0)
    return mpvFailure(QStringLiteral("open-failed"), error);

  // A command reply is not a loaded frame. Wait for the initial restart, then
  // (if needed) request an exact seek and wait for its own playback restart.
  bool seekIssued = false;
  while (phase.load() == Phase::Working && Clock::now() < deadline) {
    const mpv_event *event = mpv_wait_event(mpv, 0.01);
    switch (event->event_id) {
    case MPV_EVENT_END_FILE: {
      const auto *end = static_cast<const mpv_event_end_file *>(event->data);
      return mpvFailure(QStringLiteral("no-frame"), end->error < 0 ? end->error : MPV_ERROR_NOTHING_TO_PLAY);
    }
    case MPV_EVENT_COMMAND_REPLY:
      if (event->error < 0)
        return mpvFailure(event->reply_userdata == 1 ? QStringLiteral("open-failed") : QStringLiteral("seek-failed"),
                          event->error);
      break;
    case MPV_EVENT_QUEUE_OVERFLOW:
      return mpvFailure(QStringLiteral("no-frame"), MPV_ERROR_EVENT_QUEUE_FULL);
    case MPV_EVENT_PLAYBACK_RESTART: {
      if (!seekIssued && fraction > 0) {
        double duration = 0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0
            && std::isfinite(duration) && duration >= 1) {
          const QByteArray position = QByteArray::number(duration * fraction, 'g', 17);
          const char *seek[] = {"seek", position.constData(), "absolute+exact", nullptr};
          error = mpv_command_async(mpv, 2, seek);
          if (error < 0)
            return mpvFailure(QStringLiteral("seek-failed"), error);
          seekIssued = true;
          break;
        }
      }
      const QSize size = videoSize(mpv, bounds);
      if (!size.isValid())
        return {QStringLiteral("no-frame"), QStringLiteral("No video dimensions")};
      image = QImage(size, QImage::Format_RGBX8888);
      if (image.isNull())
        return {QStringLiteral("render-failed"), QStringLiteral("Cannot allocate the bounded frame")};
      int dimensions[] = {size.width(), size.height()};
      size_t stride = image.bytesPerLine();
      int block = 0;
      char pixelFormat[] = "rgb0";
      mpv_render_param options[] = {
          {MPV_RENDER_PARAM_SW_SIZE, dimensions}, {MPV_RENDER_PARAM_SW_FORMAT, pixelFormat},
          {MPV_RENDER_PARAM_SW_STRIDE, &stride}, {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
          {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {MPV_RENDER_PARAM_INVALID, nullptr}};
      // No ADVANCED_CONTROL: we may use the core and the render context on
      // this same private thread. Render consumes the pending current frame.
      error = mpv_render_context_render(render, options);
      if (error < 0)
        return mpvFailure(QStringLiteral("render-failed"), error);
      for (int y = 0; y < image.height(); ++y) {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
          row[x * 4 + 3] = 255;
      }
      return {};
    }
    default:
      break;
    }
    if (mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
      int skip = 1;
      int block = 0;
      mpv_render_param options[] = {{MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
          {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {MPV_RENDER_PARAM_INVALID, nullptr}};
      error = mpv_render_context_render(render, options);
      if (error < 0)
        return mpvFailure(QStringLiteral("render-failed"), error);
    }
  }
  return {QStringLiteral("timeout"), QStringLiteral("No video frame before the capture deadline")};
}
} // namespace

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

  Result run(MpvBackendCore &core, Input &input) {
    if (phase.load() != Phase::Working)
      return {};
    input.fd = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat info {};
    if (input.fd < 0 || ::fstat(input.fd, &info) != 0 || !S_ISREG(info.st_mode))
      return {QStringLiteral("unreadable"), QStringLiteral("Cannot open a readable regular video file")};
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
      return {QStringLiteral("initialization-failed"), diagnostic.message};
    auto *mpv = core.handle();
    mpv_render_context *context = nullptr;
    char api[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, api}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    const int error = mpv_render_context_create(&context, mpv, params);
    if (error < 0)
      return mpvFailure(QStringLiteral("initialization-failed"), error);
    std::unique_ptr<mpv_render_context, decltype(&mpv_render_context_free)> render(context, mpv_render_context_free);
    QImage image;
    auto result = readFrame(mpv, render.get(), QByteArray("fd://") + QByteArray::number(input.fd),
                            fraction, bounds, phase, deadline, image);
    if (!result.code.isEmpty() || phase.load() != Phase::Working)
      return result;

    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG"))
      return {QStringLiteral("write-failed"), file.errorString()};
    if (Clock::now() >= deadline)
      return {QStringLiteral("timeout"), QStringLiteral("PNG encoding exceeded the capture deadline")};
    Phase working = Phase::Working;
    if (!phase.compare_exchange_strong(working, Phase::Publishing))
      return {};  // QSaveFile discards its uncommitted temporary file.
    if (!file.commit())
      return {QStringLiteral("write-failed"), file.errorString()};
    return {};
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
        Input input;
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
          receiver->retired(job, result.code, result.reason);
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

void MpvFrameCapture::retired(const std::shared_ptr<Job> &job, const QString &code, const QString &reason) {
  if (!m_activeJobs.remove(job->id))
    return;
  const QPointer<MpvFrameCapture> guard(this);
  if (job->phase.load() != Phase::Cancelled)
    emit finished(job->id, code, reason);
  if (guard)
    emit capacityReleased(job->id);
}

bool MpvFrameCapture::cancel(int id) {
  auto job = m_activeJobs.value(id);
  Phase working = Phase::Working;
  if (!job || !job->phase.compare_exchange_strong(working, Phase::Cancelled))
    return false;
  // Keep the slot occupied until native cleanup; cancellation delivery itself
  // never waits for a decoder or a filesystem operation.
  QMetaObject::invokeMethod(this, [this, id] {
    emit finished(id, QStringLiteral("cancelled"), QStringLiteral("Capture cancelled"));
  }, Qt::QueuedConnection);
  return true;
}
