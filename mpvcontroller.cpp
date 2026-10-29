/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Node conversion adapted from MpvQt edb1e32; modified 2026-10-01.
 * See UPSTREAM.md. Native input ownership and reply decoding are local fixes.
 */
#include "mpvcontroller_p.h"

#include <QByteArray>
#include <QStringList>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <array>
#include <clocale>
#include <cstring>
#include <limits>
#include <new>
#include <cmath>

namespace MpvQmlPrivate {
namespace {

int convertInput(mpv_node *destination, const QVariant &value) {
  switch (value.typeId()) {
  case QMetaType::QString:
    destination->format = MPV_FORMAT_STRING;
    destination->u.string = qstrdup(value.toString().toUtf8().constData());
    return MPV_ERROR_SUCCESS;
  case QMetaType::Bool:
    destination->format = MPV_FORMAT_FLAG;
    destination->u.flag = value.toBool() ? 1 : 0;
    return MPV_ERROR_SUCCESS;
  case QMetaType::Int:
  case QMetaType::UInt:
  case QMetaType::LongLong:
  case QMetaType::ULongLong:
    destination->format = MPV_FORMAT_INT64;
    destination->u.int64 = value.toLongLong();
    return MPV_ERROR_SUCCESS;
  case QMetaType::Double:
  case QMetaType::Float:
    destination->format = MPV_FORMAT_DOUBLE;
    destination->u.double_ = value.toDouble();
    return MPV_ERROR_SUCCESS;
  case QMetaType::QByteArray: {
    const QByteArray bytes = value.toByteArray();
    destination->format = MPV_FORMAT_BYTE_ARRAY;
    auto *buffer = new mpv_byte_array{};
    destination->u.ba = buffer;
    buffer->size = static_cast<size_t>(bytes.size());
    buffer->data = new char[buffer->size];
    if (buffer->size)
      std::memcpy(buffer->data, bytes.constData(), buffer->size);
    return MPV_ERROR_SUCCESS;
  }
  case QMetaType::QStringList:
  case QMetaType::QVariantList: {
    const QVariantList values = value.toList();
    if (values.size() > std::numeric_limits<int>::max())
      return MPV_ERROR_INVALID_PARAMETER;
    destination->format = MPV_FORMAT_NODE_ARRAY;
    auto *list = new mpv_node_list{};
    destination->u.list = list;
    list->num = static_cast<int>(values.size());
    list->values = new mpv_node[list->num]{};
    for (int i = 0; i < list->num; ++i) {
      const int error = convertInput(&list->values[i], values[i]);
      if (error < 0)
        return error;
    }
    return MPV_ERROR_SUCCESS;
  }
  case QMetaType::QVariantMap: {
    const QVariantMap values = value.toMap();
    if (values.size() > std::numeric_limits<int>::max())
      return MPV_ERROR_INVALID_PARAMETER;
    destination->format = MPV_FORMAT_NODE_MAP;
    auto *list = new mpv_node_list{};
    destination->u.list = list;
    list->num = static_cast<int>(values.size());
    list->values = new mpv_node[list->num]{};
    list->keys = new char *[list->num]{};
    int i = 0;
    for (auto it = values.cbegin(); it != values.cend(); ++it, ++i) {
      list->keys[i] = qstrdup(it.key().toUtf8().constData());
      const int error = convertInput(&list->values[i], it.value());
      if (error < 0)
        return error;
    }
    return MPV_ERROR_SUCCESS;
  }
  default:
    if (!value.isValid())
      return MPV_ERROR_SUCCESS; // value-initialized MPV_FORMAT_NONE
    return MPV_ERROR_INVALID_PARAMETER;
  }
}

} // namespace

void freeInputNode(mpv_node *node) noexcept {
  if (!node)
    return;
  switch (node->format) {
  case MPV_FORMAT_STRING:
    // Qt qbytearray.cpp documents qstrdup ownership as delete[].
    delete[] node->u.string;
    break;
  case MPV_FORMAT_NODE_ARRAY:
  case MPV_FORMAT_NODE_MAP:
    if (auto *list = node->u.list) {
      for (int i = 0; i < list->num; ++i) {
        if (list->keys)
          delete[] list->keys[i];
        if (list->values)
          freeInputNode(&list->values[i]);
      }
      delete[] list->keys;
      delete[] list->values;
      delete list;
    }
    break;
  case MPV_FORMAT_BYTE_ARRAY:
    if (auto *bytes = node->u.ba) {
      delete[] static_cast<char *>(bytes->data);
      delete bytes;
    }
    break;
  default:
    break;
  }
  *node = {};
}

MpvInputNode::~MpvInputNode() { freeInputNode(&m_node); }

int MpvInputNode::assign(const QVariant &value) noexcept {
  freeInputNode(&m_node);
  int error;
  try {
    error = convertInput(&m_node, value);
  } catch (const std::bad_alloc &) {
    error = MPV_ERROR_NOMEM;
  } catch (...) {
    error = MPV_ERROR_INVALID_PARAMETER;
  }
  if (error < 0)
    freeInputNode(&m_node);
  return error;
}

QVariant copyNodeValue(const mpv_node *node) {
  if (!node)
    return {};
  switch (node->format) {
  case MPV_FORMAT_STRING:
    return QString::fromUtf8(node->u.string);
  case MPV_FORMAT_FLAG:
    return node->u.flag != 0;
  case MPV_FORMAT_INT64:
    return QVariant::fromValue(static_cast<qlonglong>(node->u.int64));
  case MPV_FORMAT_DOUBLE:
    return node->u.double_;
  case MPV_FORMAT_BYTE_ARRAY: {
    const mpv_byte_array *bytes = node->u.ba;
    if (!bytes || (!bytes->data && bytes->size)
        || bytes->size > static_cast<size_t>(std::numeric_limits<qsizetype>::max()))
      return {};
    return QByteArray(static_cast<const char *>(bytes->data),
                      static_cast<qsizetype>(bytes->size));
  }
  case MPV_FORMAT_NODE_ARRAY: {
    const mpv_node_list *list = node->u.list;
    if (!list || list->num < 0 || (!list->values && list->num))
      return {};
    QVariantList result;
    result.reserve(list->num);
    for (int i = 0; i < list->num; ++i)
      result.append(copyNodeValue(&list->values[i]));
    return result;
  }
  case MPV_FORMAT_NODE_MAP: {
    const mpv_node_list *list = node->u.list;
    if (!list || list->num < 0 || ((!list->values || !list->keys) && list->num))
      return {};
    QVariantMap result;
    for (int i = 0; i < list->num; ++i)
      result.insert(QString::fromUtf8(list->keys[i]), copyNodeValue(&list->values[i]));
    return result;
  }
  default:
    return {};
  }
}

QVariant copyPropertyValue(const mpv_event_property *property, int &error) {
  // A failed reply may have no event data at all. Do not inspect its format or
  // borrowed payload before checking the native reply status.
  if (error < 0)
    return {};
  if (!property) {
    error = MPV_ERROR_PROPERTY_UNAVAILABLE;
    return {};
  }
  if (property->format == MPV_FORMAT_NONE)
    return {}; // ordinary property invalidation, with no borrowed data
  if (!property->data) {
    error = MPV_ERROR_PROPERTY_UNAVAILABLE;
    return {};
  }
  switch (property->format) {
  case MPV_FORMAT_FLAG:
    return *static_cast<const int *>(property->data) != 0;
  case MPV_FORMAT_DOUBLE:
    return *static_cast<const double *>(property->data);
  case MPV_FORMAT_INT64:
    return QVariant::fromValue(static_cast<qlonglong>(
        *static_cast<const int64_t *>(property->data)));
  case MPV_FORMAT_STRING:
    return QString::fromUtf8(*static_cast<char *const *>(property->data));
  case MPV_FORMAT_NODE:
    return copyNodeValue(static_cast<const mpv_node *>(property->data));
  default:
    error = MPV_ERROR_PROPERTY_FORMAT;
    return {};
  }
}

struct MpvController::WakeupToken {
  WakeupToken(MpvController *object, quint64 value) : receiver(object), generation(value) {}
  MpvController *const receiver;
  const quint64 generation;
  std::atomic<bool> pending{false};
};

MpvController::MpvController(std::shared_ptr<MpvCoreLease> core)
    : QObject(nullptr), m_core(std::move(core)) {
  qRegisterMetaType<MpvNativeReply>();
}

MpvController::~MpvController() { unregisterWakeup(); }

bool MpvController::available() const noexcept {
  return !m_closing && m_available && m_core && m_core->handle();
}

void MpvController::initialize() {
  if (m_initialization.attempted)
    return;
  m_initialization.attempted = true;
  if (!m_core || m_closing) {
    m_initialization.error = MPV_ERROR_UNINITIALIZED;
    m_initialization.message = QStringLiteral("No available reserved mpv core lease");
    emit initializationFinished(false, m_initialization.error, m_initialization.message);
    return;
  }
  try {
    m_wakeup = std::make_unique<WakeupToken>(this, m_generation);
  } catch (const std::bad_alloc &) {
    m_initialization.error = MPV_ERROR_NOMEM;
    m_initialization.message = QStringLiteral("Cannot allocate mpv wakeup registration");
    emit initializationFinished(false, m_initialization.error, m_initialization.message);
    return;
  }
  std::setlocale(LC_NUMERIC, "C");
  std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> candidate(mpv_create(), mpv_destroy);
  int error = MPV_ERROR_GENERIC;
  QString message = QStringLiteral("mpv_create returned null: core creation failed");
  if (candidate) {
    error = mpv_set_option_string(candidate.get(), "vo", "libmpv");
    if (error >= 0)
      error = mpv_initialize(candidate.get());
    message = error < 0 ? QString::fromUtf8(mpv_error_string(error)) : QString();
  }
  m_initialization.error = error;
  m_initialization.message = message;
  m_initialization.success = error >= 0;
  if (m_initialization.success) {
    m_core->publish(candidate.release());
    m_available = true;
    // Registration may synchronously invoke the callback. Publish the fully
    // initialized immutable token and receiver before entering libmpv.
    m_wakeupRegistered = true;
    mpv_set_wakeup_callback(m_core->handle(), &wakeup, m_wakeup.get());
    // Bounded owned observations support recovery without reading/waiting on
    // the controller from synchronize/render/destruction.
    mpv_observe_property(m_core->handle(), 0, "playlist", MPV_FORMAT_NODE);
    mpv_observe_property(m_core->handle(), 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_core->handle(), 0, "pause", MPV_FORMAT_FLAG);
  }
  emit initializationFinished(m_initialization.success, error, message);
}

void MpvController::wakeup(void *context) noexcept {
  auto *token = static_cast<WakeupToken *>(context);
  if (token->pending.exchange(true, std::memory_order_acq_rel))
    return;
  MpvController *const receiver = token->receiver;
  const quint64 generation = token->generation;
  try {
    if (QMetaObject::invokeMethod(receiver, [receiver, generation]() {
          receiver->drain(generation);
        }, Qt::QueuedConnection))
      return;
  } catch (...) {
  }
  token->pending.store(false, std::memory_order_release);
}

void MpvController::unregisterWakeup() {
  if (!m_wakeup)
    return;
  if (m_wakeupRegistered)
    mpv_set_wakeup_callback(m_core->handle(), nullptr, nullptr);
  m_wakeupRegistered = false;
  m_wakeup.reset(); // native callback barrier has completed
}

void MpvController::shutdown() {
  if (m_closing)
    return;
  m_closing = true;
  m_recovery.valid = false;
  m_available = false;
  ++m_generation;
  unregisterWakeup();
  QThread::currentThread()->quit();
}

void MpvController::submissionResult(MpvRequestKind kind, quint64 token, int error) {
  if (error < 0)
    emit reply(MpvNativeReply{kind, token, m_generation, error, {}});
}

void MpvController::submitCommand(quint64 token, const QStringList &parameters) {
  int error = MPV_ERROR_UNINITIALIZED;
  if (available()) {
    m_core->noteNativeToken(token);
    if (!parameters.isEmpty()) {
      const QString &command = parameters[0];
      const QString mode = parameters.size() > 2 ? parameters[2] : QStringLiteral("replace");
      const bool changesFile = command == QStringLiteral("loadfile")
          && mode != QStringLiteral("append")
          && !(mode == QStringLiteral("append-play") && m_playback.loaded);
      if (changesFile || command == QStringLiteral("loadlist")
          || command == QStringLiteral("stop") || command == QStringLiteral("quit")
          || command == QStringLiteral("playlist-next") || command == QStringLiteral("playlist-prev")
          || command == QStringLiteral("playlist-play-index"))
        invalidatePlaybackIntent();
      if (command == QStringLiteral("seek") || command == QStringLiteral("revert-seek")) {
        ++m_seekRevision;
        m_seekIntent.valid = false;
        if (command == QStringLiteral("seek") && parameters.size() > 1) {
          bool valid = false;
          const double amount = parameters[1].toDouble(&valid);
          const QString mode = parameters.size() > 2 ? parameters[2] : QStringLiteral("relative");
          const bool absolute = mode == QStringLiteral("absolute") || mode == QStringLiteral("absolute+exact");
          const bool relative = mode == QStringLiteral("relative") || mode == QStringLiteral("relative+exact");
          if (valid && std::isfinite(amount) && (absolute || relative)) {
            const double base = m_recovery.valid ? m_recovery.playback.position
                : m_playback.loaded ? m_playback.position : m_terminatedPlayback.position;
            rememberSeekIntent(qMax(0.0, absolute ? amount : base + amount));
          }
        }
      }
    }
    MpvInputNode input;
    error = input.assign(parameters);
    if (error >= 0)
      error = mpv_command_node_async(m_core->handle(), token, input.get());
  }
  submissionResult(MpvRequestKind::Command, token, error);
}

void MpvController::submitPropertySet(quint64 token, const QString &name,
                                     const QVariant &value) {
  int error = MPV_ERROR_UNINITIALIZED;
  if (available()) {
    m_core->noteNativeToken(token);
    if (name == QStringLiteral("pause"))
      ++m_pauseRevision;
    if (name == QStringLiteral("time-pos")) {
      ++m_seekRevision;
      m_seekIntent.valid = false;
      bool valid = false;
      const double position = value.toDouble(&valid);
      if (valid && std::isfinite(position) && position >= 0)
        rememberSeekIntent(position);
    }
    if (name == QStringLiteral("playlist-pos") || name == QStringLiteral("playlist-current-pos")
        || name == QStringLiteral("path"))
      invalidatePlaybackIntent();
    MpvInputNode input;
    error = input.assign(value);
    if (error >= 0)
      error = mpv_set_property_async(m_core->handle(), token, name.toUtf8().constData(),
                                     MPV_FORMAT_NODE, input.get());
  }
  submissionResult(MpvRequestKind::PropertySet, token, error);
}

void MpvController::submitPropertyRead(quint64 token, const QString &name) {
  if (m_core)
    m_core->noteNativeToken(token);
  const int error = available()
      ? mpv_get_property_async(m_core->handle(), token, name.toUtf8().constData(), MPV_FORMAT_NODE)
      : MPV_ERROR_UNINITIALIZED;
  submissionResult(MpvRequestKind::PropertyRead, token, error);
}

void MpvController::submitConfiguration(quint64 token, const QString &path) {
  const int error = available()
      ? mpv_load_config_file(m_core->handle(), path.toUtf8().constData())
      : MPV_ERROR_UNINITIALIZED;
  emit reply(MpvNativeReply{MpvRequestKind::Configuration, token, m_generation, error, {}});
}

void MpvController::observe(const QString &name, mpv_format format, quint64 token) {
  const int error = available()
      ? mpv_observe_property(m_core->handle(), token, name.toUtf8().constData(), format)
      : MPV_ERROR_UNINITIALIZED;
  if (error < 0)
    emit operationFailed(QStringLiteral("observeProperty"), error,
                         QString::fromUtf8(mpv_error_string(error)));
}

void MpvController::invalidatePlaybackIntent() {
  m_core->advanceSourceIntent();
  m_recovery.valid = false;
  m_playback = {};
  m_terminatedPlayback = {};
  m_terminationError = 0;
  m_seekIntent = {};
}

void MpvController::rememberSeekIntent(double position) {
  const quint64 intent = m_core->sourceIntent();
  const quint64 renderer = m_core->retiringContext(intent);
  if (!renderer) return;
  m_seekIntent = {renderer, intent, m_seekRevision, position, true};
  if (m_recovery.valid && !m_recovery.seekSubmitted
      && m_recovery.retiredRenderer == renderer && m_recovery.playback.intent == intent) {
    m_recovery.playback.position = position;
    m_recovery.seekRevision = m_seekRevision;
  }
}

void MpvController::cachePlaybackProperty(const QString &name, const QVariant &value) {
  if (name == QStringLiteral("playlist") && value.isValid()) {
    m_playlist = value.toList();
  } else if (name == QStringLiteral("pause") && value.isValid()) {
    m_observedPause = value.toBool();
    if (m_playback.loaded) {
      m_playback.paused = m_observedPause;
      m_playback.pauseRevision = m_pauseRevision;
    }
  } else if (name == QStringLiteral("time-pos") && value.isValid() && m_playback.loaded) {
    const double position = value.toDouble();
    if (position >= 0 && std::isfinite(position)) {
      m_playback.position = position;
      m_playback.seekRevision = m_seekRevision;
    }
  }
}

void MpvController::renderContextRetiring(quint64 rendererGeneration, quint64 sourceIntent) {
  if (!available() || !rendererGeneration || sourceIntent != m_core->sourceIntent())
    return;
  if (m_recovery.valid && m_recovery.retiredRenderer == rendererGeneration
      && m_recovery.playback.intent == sourceIntent)
    return; // worker already classified the matching native loss event
  PlaybackState snapshot = m_playback.loaded ? m_playback : m_terminatedPlayback;
  if (!snapshot.loaded || snapshot.entryId < 0 || snapshot.intent != sourceIntent)
    return;
  m_recovery = {};
  m_recovery.playback = snapshot;
  m_recovery.retiredRenderer = rendererGeneration;
  m_recovery.revision = ++m_recoveryRevision;
  m_recovery.pauseRevision = snapshot.pauseRevision;
  m_recovery.seekRevision = snapshot.seekRevision;
  if (m_seekIntent.valid && m_seekIntent.renderer == rendererGeneration
      && m_seekIntent.intent == sourceIntent && m_seekIntent.revision == m_seekRevision) {
    m_recovery.playback.position = m_seekIntent.position;
    m_recovery.seekRevision = m_seekIntent.revision;
  }
  m_recovery.valid = true;
  m_recovery.terminated = m_terminationError == MPV_ERROR_VO_INIT_FAILED
      && m_terminatedPlayback.entryId == snapshot.entryId
      && m_terminatedPlayback.intent == sourceIntent;
}

void MpvController::renderContextReady(quint64 rendererGeneration) {
  if (!m_recovery.valid || rendererGeneration <= m_recovery.retiredRenderer)
    return;
  m_recovery.readyRenderer = rendererGeneration;
  tryRecoverPlayback();
}

void MpvController::failRecovery(int error) {
  if (!m_recovery.valid)
    return;
  m_recovery.valid = false;
  emit operationFailed(QStringLiteral("renderRecovery"), error,
                       QString::fromUtf8(mpv_error_string(error)));
}

quint64 MpvController::submitRecovery(RecoveryStep step, const QVariant &value,
                                     const QString &property) {
  const quint64 token = m_core->allocateNativeToken();
  if (!token) {
    failRecovery(MPV_ERROR_NOMEM);
    return 0;
  }
  m_recoveryRequests.insert(token, {m_recovery.revision, step});
  MpvInputNode input;
  int error = input.assign(value);
  if (error >= 0) {
    error = property.isEmpty()
        ? mpv_command_node_async(m_core->handle(), token, input.get())
        : mpv_set_property_async(m_core->handle(), token, property.toUtf8().constData(),
                                 MPV_FORMAT_NODE, input.get());
  }
  if (error < 0) {
    m_recoveryRequests.remove(token);
    failRecovery(error);
    return 0;
  }
  return token;
}

void MpvController::tryRecoverPlayback() {
  if (!available() || !m_recovery.valid || !m_recovery.terminated
      || !m_recovery.readyRenderer || m_recovery.replaying || !m_recoveryRequests.isEmpty())
    return;
  if (m_recovery.playback.intent != m_core->sourceIntent()) {
    m_recovery.valid = false;
    return;
  }
  int index = -1;
  for (int i = 0; i < m_playlist.size(); ++i) {
    if (m_playlist[i].toMap().value(QStringLiteral("id")).toLongLong()
        == m_recovery.playback.entryId) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    failRecovery(MPV_ERROR_NOTHING_TO_PLAY);
    return;
  }
  if (!m_recovery.pauseAcknowledged && m_recovery.pauseRevision != m_pauseRevision)
    m_recovery.pauseAcknowledged = true; // newer ordinary pause intent owns native state
  if (!m_recovery.pauseAcknowledged) {
    submitRecovery(RecoveryStep::Pause, m_recovery.playback.paused, QStringLiteral("pause"));
    return;
  }
  m_recovery.replaying = true;
  submitRecovery(RecoveryStep::Replay, QStringList{"playlist-play-index", QString::number(index)});
}

void MpvController::tryRestorePosition() {
  if (!m_recovery.valid || !m_recovery.replaying || m_recovery.seekSubmitted
      || !m_recovery.fileLoaded || !m_recovery.replayAcknowledged || !m_recovery.pauseAcknowledged)
    return;
  if (m_recovery.playback.intent != m_core->sourceIntent()) {
    m_recovery.valid = false;
    return;
  }
  if (m_recovery.seekRevision != m_seekRevision) {
    const auto completed = m_recovery;
    m_recovery.valid = false;
    emit playbackRecovered(completed.readyRenderer, completed.playback.entryId);
    return;
  }
  m_recovery.seekSubmitted = true;
  submitRecovery(RecoveryStep::Seek, QStringList{
      "seek", QString::number(m_recovery.playback.position, 'g', 17), "absolute+exact"});
}

bool MpvController::handleRecoveryReply(quint64 token, int error) {
  const auto found = m_recoveryRequests.find(token);
  if (found == m_recoveryRequests.end())
    return false;
  const RecoveryRequest request = found.value();
  m_recoveryRequests.erase(found); // remove before any external delivery
  if (!m_recovery.valid || request.revision != m_recovery.revision
      || m_recovery.playback.intent != m_core->sourceIntent()) {
    tryRecoverPlayback();
    return true;
  }
  if (error < 0) {
    failRecovery(error);
    return true;
  }
  switch (request.step) {
  case RecoveryStep::Pause:
    m_recovery.pauseAcknowledged = true;
    tryRecoverPlayback(); // pause completion precedes native replay admission
    return true;
  case RecoveryStep::Replay: m_recovery.replayAcknowledged = true; break;
  case RecoveryStep::Seek: {
    const auto completed = m_recovery;
    m_recovery.valid = false;
    emit playbackRecovered(completed.readyRenderer, completed.playback.entryId);
    return true;
  }
  }
  tryRestorePosition();
  return true;
}

void MpvController::drain(quint64 generation) {
  if (generation != m_generation || !available())
    return;
  m_wakeup->pending.store(false, std::memory_order_release);
  if (m_draining) {
    m_redrain = true;
    return;
  }
  m_draining = true;
  QPointer<MpvController> alive(this);
  while (available() && generation == m_generation) {
    const mpv_event *event = mpv_wait_event(m_core->handle(), 0);
    if (!event || event->event_id == MPV_EVENT_NONE)
      break;
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
      const auto *property = static_cast<const mpv_event_property *>(event->data);
      const QString name = property ? QString::fromUtf8(property->name) : QString();
      int error = event->error;
      const QVariant value = copyPropertyValue(property, error);
      if (error >= 0)
        cachePlaybackProperty(name, value);
      if (!name.isEmpty() && error >= 0)
        emit propertyChanged(name, value);
      break;
    }
    case MPV_EVENT_COMMAND_REPLY: {
      int error = event->error;
      const auto *command = static_cast<const mpv_event_command *>(event->data);
      QVariant value;
      if (error >= 0) {
        if (command)
          value = copyNodeValue(&command->result);
        else
          error = MPV_ERROR_COMMAND;
      }
      if (!handleRecoveryReply(event->reply_userdata, error))
        emit reply(MpvNativeReply{MpvRequestKind::Command, event->reply_userdata,
                                 generation, error, value});
      break;
    }
    case MPV_EVENT_SET_PROPERTY_REPLY:
      if (!handleRecoveryReply(event->reply_userdata, event->error))
        emit reply(MpvNativeReply{MpvRequestKind::PropertySet, event->reply_userdata,
                                 generation, event->error, {}});
      break;
    case MPV_EVENT_GET_PROPERTY_REPLY: {
      int error = event->error;
      const auto *property = static_cast<const mpv_event_property *>(event->data);
      const QVariant value = copyPropertyValue(property, error);
      if (error >= 0 && property)
        cachePlaybackProperty(QString::fromUtf8(property->name), value);
      emit reply(MpvNativeReply{MpvRequestKind::PropertyRead, event->reply_userdata,
                               generation, error, value});
      break;
    }
    case MPV_EVENT_START_FILE: {
      const auto *start = static_cast<const mpv_event_start_file *>(event->data);
      const qlonglong entryId = start ? start->playlist_entry_id : -1;
      const bool resumed = m_recovery.valid && m_recovery.replaying
          && m_recovery.playback.entryId == entryId
          && m_recovery.playback.intent == m_core->sourceIntent();
      const bool renderLossAdvance = m_recovery.valid && m_recovery.terminated
          && m_recovery.playback.intent == m_core->sourceIntent()
          && m_core->retiringContext(m_recovery.playback.intent) == m_recovery.retiredRenderer;
      if (!resumed && !renderLossAdvance && m_playback.entryId >= 0 && entryId != m_playback.entryId)
        invalidatePlaybackIntent();
      m_playback = resumed ? m_recovery.playback : PlaybackState{};
      m_playback.entryId = entryId;
      m_playback.intent = m_core->sourceIntent();
      m_playback.paused = m_observedPause;
      m_playback.pauseRevision = m_pauseRevision;
      m_playback.loaded = false;
      emit fileStarted();
      break;
    }
    case MPV_EVENT_FILE_LOADED:
      m_playback.loaded = true;
      m_playback.paused = m_observedPause;
      m_playback.pauseRevision = m_pauseRevision;
      if (m_recovery.valid && m_recovery.replaying
          && m_playback.entryId == m_recovery.playback.entryId)
        m_recovery.fileLoaded = true;
      tryRestorePosition();
      if (!alive)
        return;
      emit fileLoaded();
      break;
    case MPV_EVENT_VIDEO_RECONFIG:
      emit videoReconfig();
      break;
    case MPV_EVENT_END_FILE: {
      const auto *ending = static_cast<const mpv_event_end_file *>(event->data);
      QString reason;
      if (ending) {
        if (ending->reason == MPV_END_FILE_REASON_ERROR && ending->error == MPV_ERROR_VO_INIT_FAILED
            && m_playback.loaded) {
          const quint64 retired = m_core->retiringContext(m_playback.intent);
          if (retired)
            renderContextRetiring(retired, m_playback.intent);
        }
        if (m_playback.loaded) {
          m_terminatedPlayback = m_playback;
          m_terminationError = ending->error;
          m_playback.loaded = false;
        }
        if (m_recovery.valid && m_recovery.playback.entryId == m_terminatedPlayback.entryId
            && m_recovery.playback.intent == m_terminatedPlayback.intent
            && ending->reason == MPV_END_FILE_REASON_ERROR
            && ending->error == MPV_ERROR_VO_INIT_FAILED) {
          m_recovery.terminated = true;
          tryRecoverPlayback();
          if (!alive)
            return;
        }
        switch (ending->reason) {
        case MPV_END_FILE_REASON_EOF: reason = QStringLiteral("eof"); break;
        case MPV_END_FILE_REASON_STOP: reason = QStringLiteral("stop"); break;
        case MPV_END_FILE_REASON_ERROR: reason = QStringLiteral("error"); break;
        default: break;
        }
      }
      if (!reason.isEmpty())
        emit endFile(reason);
      break;
    }
    case MPV_EVENT_SHUTDOWN:
      m_available = false;
      unregisterWakeup();
      emit coreUnavailable();
      break;
    default:
      break;
    }
    // Every external signal can delete this sender or change its generation.
    if (!alive)
      return;
  }
  m_draining = false;
  if (m_redrain && available() && generation == m_generation) {
    m_redrain = false;
    QMetaObject::invokeMethod(this, [this, generation]() { drain(generation); }, Qt::QueuedConnection);
  }
}

struct MpvWorkerControl {
  QThread *thread = nullptr;
  MpvController *controller = nullptr;
  std::shared_ptr<MpvCoreLease> core;
};

namespace {
struct WorkerRegistry {
  std::array<std::shared_ptr<MpvWorkerControl>, 32> controls;
  QObject *reaper = new QObject; // GUI-affine, no application/static teardown parent
};

WorkerRegistry &workers() {
  static WorkerRegistry *registry = new WorkerRegistry;
  return *registry;
}

void reapWorker(std::shared_ptr<MpvWorkerControl> control) {
  // finished/isRunning may precede native TLS cleanup. wait(0) observes actual
  // termination without making a facade/window destructor join a running worker.
  if (!control->thread->wait(0)) {
    QTimer::singleShot(1, workers().reaper, [control]() { reapWorker(control); });
    return;
  }
  delete control->thread;
  control->thread = nullptr;
  control->controller = nullptr;
  const int index = control->core->reservation();
  workers().controls[index].reset();
  control->core.reset();
}
} // namespace

MpvBackendWorker::MpvBackendWorker() {
  int error;
  QString message;
  auto core = MpvCoreLease::reserve(error, message);
  if (!core) {
    m_result = {true, false, error, message};
    return;
  }
  m_control = std::make_shared<MpvWorkerControl>();
  m_control->core = std::move(core);
  m_control->controller = new MpvController(m_control->core);
  m_control->thread = new QThread;
}

MpvBackendWorker::~MpvBackendWorker() { shutdown(); }

MpvController *MpvBackendWorker::controller() const noexcept {
  return m_control ? m_control->controller : nullptr;
}

std::shared_ptr<MpvCoreLease> MpvBackendWorker::core() const noexcept {
  return m_control ? m_control->core : std::shared_ptr<MpvCoreLease>();
}

MpvInitializationResult MpvBackendWorker::initialize() {
  if (m_result.attempted || !m_control)
    return m_result;
  auto control = m_control;
  control->thread->start();
  if (!control->thread->isRunning()) {
    m_result = {true, false, MPV_ERROR_GENERIC, QStringLiteral("mpv worker thread failed to start")};
    // The QObject never moved and the thread never started. finished/deleteLater
    // cannot clean this path; both objects still belong to the construction thread.
    delete control->controller;
    delete control->thread;
    m_control.reset();
    return m_result;
  }
  control->controller->moveToThread(control->thread);
  QObject::connect(control->thread, &QThread::finished, control->controller, &QObject::deleteLater);
  workers().controls[control->core->reservation()] = control;
  QObject::connect(control->thread, &QThread::finished, workers().reaper,
                   [control]() { reapWorker(control); }, Qt::QueuedConnection);
  MpvInitializationResult snapshot;
  const bool accepted = QMetaObject::invokeMethod(control->controller, [&snapshot, control]() {
    control->controller->initialize();
    snapshot = control->controller->initializationResult();
  }, Qt::BlockingQueuedConnection);
  m_result = accepted ? snapshot : MpvInitializationResult{
      true, false, MPV_ERROR_GENERIC, QStringLiteral("mpv initialization queue admission failed")};
  return m_result;
}

bool MpvBackendWorker::post(std::function<void(MpvController *)> operation) {
  if (m_closed || !m_control || !m_result.success || !m_control->thread->isRunning())
    return false;
  MpvController *receiver = m_control->controller;
  return QMetaObject::invokeMethod(receiver, [receiver, operation = std::move(operation)]() {
    operation(receiver);
  }, Qt::QueuedConnection);
}

void MpvBackendWorker::shutdown() {
  if (m_closed)
    return;
  m_closed = true;
  if (!m_control)
    return;
  auto control = std::move(m_control);
  if (!m_result.attempted) {
    delete control->controller;
    delete control->thread;
    return;
  }
  if (control->thread && control->thread->isRunning()) {
    QMetaObject::invokeMethod(control->controller, &MpvController::shutdown, Qt::QueuedConnection);
  }
  // Registry ownership persists until the reaper observes native termination.
}

} // namespace MpvQmlPrivate
