// SPDX-License-Identifier: LGPL-3.0-only
#include "capture-wire.h"

#include <QImage>
#include <QTransform>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#include <optional>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif

namespace {
using namespace MpvCapture;

// Same bounded, failure-only log ring as imvqml's video transaction.
class Diagnostics {
public:
  void record(const mpv_event &event) {
    if (event.event_id != MPV_EVENT_LOG_MESSAGE || !event.data)
      return;
    const auto &line = *static_cast<const mpv_event_log_message *>(event.data);
    auto &entry = m_logs[m_next];
    std::snprintf(entry.data(), entry.size(), "%.20s/%.8s: %.95s", line.prefix, line.level, line.text);
    m_next = (m_next + 1) % m_logs.size();
    m_count = qMin(m_count + 1, m_logs.size());
  }

  void append(Result &result, mpv_handle *core) {
    if (result.code.isEmpty())
      return;
    for (int n = 0; n < 64; ++n) {
      const auto &event = *mpv_wait_event(core, 0);
      if (event.event_id == MPV_EVENT_NONE)
        break;
      record(event);
    }
    for (size_t n = 0; n < m_count; ++n) {
      auto line = QByteArray(m_logs[(m_next + m_logs.size() - m_count + n) % m_logs.size()].data()).trimmed();
      for (char &c : line)
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
          c = '?';
      result.reason += '\n' + QString::fromLatin1(line);
    }
  }

private:
  std::array<std::array<char, 120>, 5> m_logs{};
  size_t m_next = 0, m_count = 0;
};

Result mpvFailure(Stage stage, QString code, int error) {
  return {std::move(code), QString::fromLatin1(mpv_error_string(error)), stage, error};
}

struct Budget {
  Clock::time_point deadline;

  std::optional<Result> stop(Stage stage) const {
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
  // The filter strips this metadata from the software renderer's input. Read
  // the pre-filter orientation and apply it ourselves (see UPSTREAM.md).
  int64_t rotation = 0;
  if (mpv_get_property(mpv, "video-params/rotate", MPV_FORMAT_INT64, &rotation) < 0 || rotation % 90 != 0)
    rotation = 0;
  rotation %= 360;
  if (rotation % 180 != 0)
    bounds.transpose();
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
  if (rotation != 0) {
    image = image.transformed(QTransform().rotate(rotation));
    if (image.isNull())
      return {QStringLiteral("render-failed"), QStringLiteral("Cannot allocate the oriented frame"), Stage::Render};
  }
  return {};
}

Result readFrame(mpv_handle *mpv, mpv_render_context *render, const QByteArray &url,
                 double fraction, QSize bounds, const Budget &budget, QImage &image, Diagnostics &diagnostics) {
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
    const auto &event = *mpv_wait_event(mpv, 0.01);
    diagnostics.record(event);
    if (auto failed = events.accept(event))
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

Result capture(int input, const Spec &spec, QImage &image) {
  const Budget budget{spec.deadline};
  const auto failure = QStringLiteral("initialization-failed");
  std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> candidate(mpv_create(), mpv_destroy);
  if (!candidate)
    return {failure, QStringLiteral("mpv_create returned null: core creation failed"), Stage::Core, MPV_ERROR_GENERIC};
  struct Option { const char *name, *value; bool allowMissing = false; };
  const Option options[] = {
        {"config", "no"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"access-references", "no"}, {"vo", "libmpv"},
        {"vf", "format=rotate=0"},
        {"aid", "no"}, {"sid", "no"}, {"audio-file-auto", "no"}, {"sub-auto", "no"},
        {"pause", "yes"}, {"keep-open", "yes"}, {"hwdec", "no"}, {"vd-lavc-threads", "1"},
        // Limit decoded source pixels independently of the caller's output
        // bounds. 64 MiPixels also admits 8K UHD sources.
        {"vd-lavc-o", "max_pixels=67108864"}, {"cache", "no"},
        {"osd-level", "0"},
        // load-scripts controls user scripts, not these built-ins. Their
        // options can be absent when Lua or the script is not built in.
        {"osc", "no", true}, {"ytdl", "no", true}, {"load-stats-overlay", "no", true},
        {"load-console", "no", true}, {"load-osd-console", "no", true}, {"load-auto-profiles", "no", true},
        {"load-select", "no", true}, {"load-positioning", "no", true},
        {"load-commands", "no", true}, {"load-context-menu", "no", true}};
  for (const auto &[name, value, allowMissing] : options) {
    const auto rc = mpv_set_option_string(candidate.get(), name, value);
    if (rc < 0 && !(allowMissing && rc == MPV_ERROR_OPTION_NOT_FOUND))
      return {failure, QString::fromLatin1(name) + ": " + mpv_error_string(rc), Stage::Core, rc};
  }
  mpv_request_log_messages(candidate.get(), "warn");
  const int initialized = mpv_initialize(candidate.get());
  Diagnostics diagnostics;
  if (initialized < 0) {
    auto result = mpvFailure(Stage::Core, failure, initialized);
    diagnostics.append(result, candidate.get());
    return result;
  }
  std::unique_ptr<mpv_handle, decltype(&mpv_terminate_destroy)> core(candidate.release(), mpv_terminate_destroy);
  mpv_render_context *context = nullptr;
  char api[] = MPV_RENDER_API_TYPE_SW;
  mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, api}, {MPV_RENDER_PARAM_INVALID, nullptr}};
  const int error = mpv_render_context_create(&context, core.get(), params);
  if (error < 0) {
    auto result = mpvFailure(Stage::Context, failure, error);
    diagnostics.append(result, core.get());
    return result;
  }
  std::unique_ptr<mpv_render_context, decltype(&mpv_render_context_free)> render(context, mpv_render_context_free);
  auto result = readFrame(core.get(), render.get(), QByteArray("fd://") + QByteArray::number(input),
                          spec.fraction, spec.bounds, budget, image, diagnostics);
  diagnostics.append(result, core.get());
  // Context and core retire before this returns, under the watchdog and while
  // the request still owns its input descriptor.
  return result;
}

Result produce(int input, int output, const Spec &spec, QSize &size) {
  QImage image;
  auto result = capture(input, spec, image);
  if (!result.code.isEmpty())
    return result;
  const auto bytes = pixelBytes(image.size());
  if (!bytes || bytes != image.sizeInBytes())
    return {"render-failed", "Unexpected capture frame layout", Stage::Render};
  for (qint64 offset = 0; offset < bytes;) {
    const auto wrote = ::pwrite(output, image.constBits() + offset, size_t(bytes - offset), offset);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote <= 0)
      return {"render-failed", "Cannot write the captured pixel buffer", Stage::Render, 0, wrote < 0 ? errno : EIO};
    offset += wrote;
  }
  if (::ftruncate(output, bytes) < 0 || ::fcntl(output, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
    return {"render-failed", "Cannot seal the captured pixel buffer", Stage::Render, 0, errno};
  size = image.size();
  return {};
}
} // namespace

int main() {
#ifdef __GLIBC__
  // Many libmpv threads otherwise reserve a 64 MiB heap each on 64-bit glibc.
  if (!::mallopt(M_ARENA_MAX, 2))
    return 2;
#endif
  const rlimit noCore{0, 0};
  if (::setrlimit(RLIMIT_CORE, &noCore) || !std::setlocale(LC_NUMERIC, "C"))
    return 2;
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) \
    && !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer) && !__has_feature(memory_sanitizer)
  // Treat the helper budget as a ceiling, not permission to raise either
  // inherited limit. Sanitizer shadow memory requires a separate AS policy.
  constexpr rlim_t addressBudget = 1536ull * 1024 * 1024;
  rlimit address{};
  if (::getrlimit(RLIMIT_AS, &address))
    return 2;
  address.rlim_cur = qMin(address.rlim_cur, addressBudget);
  address.rlim_max = qMin(address.rlim_max, addressBudget);
  if (::setrlimit(RLIMIT_AS, &address))
    return 2;
#endif
  sigevent expiry{};
  expiry.sigev_notify = SIGEV_SIGNAL;
  expiry.sigev_signo = SIGKILL;
  timer_t watchdog{};
  if (::timer_create(CLOCK_MONOTONIC, &expiry, &watchdog))
    return 2;
  SeqPacket socket{Fd(ChildSocket)};
  for (;;) {
    Frame frame;
    const auto received = socket.receive(frame);
    if (received == Io::Closed)
      return 0;
    Spec spec{};
    if (received != Io::Ok || !decodeSpec(frame, spec))
      return 1;
    const auto sinceBoot = spec.deadline.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceBoot);
    const itimerspec deadline{{}, {time_t(seconds.count()), long(std::chrono::duration_cast<std::chrono::nanoseconds>(sinceBoot - seconds).count())}};
    if (::timer_settime(watchdog, TIMER_ABSTIME, &deadline, nullptr))
      return 2;
    if (!validInput(frame.fds[0].get()) || !validOutput(frame.fds[1].get(), spec.bounds, false))
      return 1;
    QSize size;
    const auto result = produce(frame.fds[0].get(), frame.fds[1].get(), spec, size);
    frame.fds.clear();
    frame.body = encodeResult(result, size);
    frame.kind = Kind::Reply;
    // Reliability of the next request must not depend on the previous
    // request's watchdog. The pool still rejects an expired reply.
    const itimerspec idle{};
    if (::timer_settime(watchdog, 0, &idle, nullptr))
      return 2;
    if (socket.send(frame) != Io::Ok)
      return 1;
  }
}
