/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Node conversion adapted from MpvQt edb1e32; modified 2026-10-01.
 * See UPSTREAM.md. This header is private to mpvqml.
 */
#pragma once

#include "mpvrenderbridge_p.h"
#include <QObject>
#include <QVariant>
#include <QHash>
#include <functional>
#include <memory>
#include <mpv/client.h>

namespace MpvQmlPrivate {

// Construct this owner before conversion. User-built trees use new/new[];
// mpv_free_node_contents is exclusively for successful libmpv-owned outputs.
class MpvInputNode final {
public:
  MpvInputNode() = default;
  ~MpvInputNode();
  MpvInputNode(const MpvInputNode &) = delete;
  MpvInputNode &operator=(const MpvInputNode &) = delete;
  int assign(const QVariant &value) noexcept;
  mpv_node *get() noexcept { return &m_node; }

private:
  mpv_node m_node{};
};

void freeInputNode(mpv_node *node) noexcept;
QVariant copyNodeValue(const mpv_node *node);
QVariant copyPropertyValue(const mpv_event_property *property, int &error);

enum class MpvRequestKind { Command, PropertySet, PropertyRead, Configuration };

struct MpvInitializationResult {
  bool attempted = false;
  bool success = false;
  int error = MPV_ERROR_UNINITIALIZED;
  QString message;
};

struct MpvNativeReply {
  MpvRequestKind kind = MpvRequestKind::Command;
  quint64 token = 0;
  quint64 generation = 0;
  int error = MPV_ERROR_SUCCESS;
  QVariant value;
};

class MpvController final : public QObject {
  Q_OBJECT
public:
  explicit MpvController(std::shared_ptr<MpvCoreLease> core);
  ~MpvController() override;
  void initialize();
  void shutdown();
  MpvInitializationResult initializationResult() const { return m_initialization; }
  void submitCommand(quint64 token, const QStringList &parameters);
  void submitPropertySet(quint64 token, const QString &name, const QVariant &value);
  void submitPropertyRead(quint64 token, const QString &name);
  void submitConfiguration(quint64 token, const QString &path);
  void observe(const QString &name, mpv_format format, quint64 token);
  void renderContextRetiring(quint64 rendererGeneration, quint64 sourceIntent);
  void renderContextReady(quint64 rendererGeneration);

signals:
  void initializationFinished(bool success, int error, const QString &message);
  void coreUnavailable();
  void reply(const MpvQmlPrivate::MpvNativeReply &value);
  void operationFailed(const QString &operation, int error, const QString &message);
  void propertyChanged(const QString &name, const QVariant &value);
  void fileStarted();
  void fileLoaded();
  void endFile(const QString &reason);
  void videoReconfig();
  void playbackRecovered(quint64 rendererGeneration, qlonglong entryId);

private:
  struct WakeupToken;
  static void wakeup(void *context) noexcept;
  void drain(quint64 generation);
  void unregisterWakeup();
  bool available() const noexcept;
  void submissionResult(MpvRequestKind kind, quint64 token, int error);
  struct PlaybackState {
    qlonglong entryId = -1;
    quint64 intent = 0;
    double position = 0;
    bool paused = false;
    bool loaded = false;
    quint64 pauseRevision = 0;
    quint64 seekRevision = 0;
  };
  struct RecoveryState {
    PlaybackState playback;
    quint64 retiredRenderer = 0;
    quint64 readyRenderer = 0;
    quint64 revision = 0;
    quint64 pauseRevision = 0;
    quint64 seekRevision = 0;
    bool valid = false;
    bool terminated = false;
    bool replaying = false;
    bool replayAcknowledged = false;
    bool pauseAcknowledged = false;
    bool fileLoaded = false;
    bool seekSubmitted = false;
  };
  enum class RecoveryStep { Pause, Replay, Seek };
  struct RecoveryRequest { quint64 revision; RecoveryStep step; };
  void invalidatePlaybackIntent();
  void cachePlaybackProperty(const QString &name, const QVariant &value);
  void tryRecoverPlayback();
  void tryRestorePosition();
  quint64 submitRecovery(RecoveryStep step, const QVariant &input, const QString &property = {});
  bool handleRecoveryReply(quint64 token, int error);
  void failRecovery(int error);
  void rememberSeekIntent(double position);
  struct SeekIntent {
    quint64 renderer = 0;
    quint64 intent = 0;
    quint64 revision = 0;
    double position = 0;
    bool valid = false;
  };
  SeekIntent m_seekIntent;
  QVariantList m_playlist;
  PlaybackState m_playback;
  PlaybackState m_terminatedPlayback;
  int m_terminationError = 0;
  RecoveryState m_recovery;
  QHash<quint64, RecoveryRequest> m_recoveryRequests;
  quint64 m_recoveryRevision = 0;
  quint64 m_pauseRevision = 0;
  quint64 m_seekRevision = 0;
  bool m_observedPause = false;
  std::shared_ptr<MpvCoreLease> m_core;
  MpvInitializationResult m_initialization;
  std::unique_ptr<WakeupToken> m_wakeup;
  quint64 m_generation = 1;
  bool m_available = false;
  bool m_wakeupRegistered = false;
  bool m_closing = false;
  bool m_draining = false;
  bool m_redrain = false;
};

struct MpvWorkerControl;

// GUI-owned facade over a parentless, independently retained native Qt worker.
// Destruction requests shutdown; it never waits for a running thread.
class MpvBackendWorker final {
public:
  MpvBackendWorker();
  ~MpvBackendWorker();
  MpvBackendWorker(const MpvBackendWorker &) = delete;
  MpvBackendWorker &operator=(const MpvBackendWorker &) = delete;
  MpvInitializationResult initialize();
  MpvController *controller() const noexcept;
  std::shared_ptr<MpvCoreLease> core() const noexcept;
  bool post(std::function<void(MpvController *)> operation);
  void shutdown();

private:
  std::shared_ptr<MpvWorkerControl> m_control;
  MpvInitializationResult m_result;
  bool m_closed = false;
};

} // namespace MpvQmlPrivate

Q_DECLARE_METATYPE(MpvQmlPrivate::MpvNativeReply)
