#include "mpvplayer.h"
#include "mpvcontroller_adapter.h"
#include "mpvrenderer_p.h"
#include "mpvlogclient.h"
#include "mpvhookmanager.h"
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>
#include <QtGui/qguiapplication_platform.h>
#include <cmath>
#include <limits>
#include <utility>

using namespace MpvQmlPrivate;

#define CHECK_ERROR(Name, Native) static_assert(static_cast<int>(MpvError::Name) == Native)
CHECK_ERROR(Success, MPV_ERROR_SUCCESS);
CHECK_ERROR(EventQueueFull, MPV_ERROR_EVENT_QUEUE_FULL);
CHECK_ERROR(NoMem, MPV_ERROR_NOMEM);
CHECK_ERROR(Uninitialized, MPV_ERROR_UNINITIALIZED);
CHECK_ERROR(InvalidParameter, MPV_ERROR_INVALID_PARAMETER);
CHECK_ERROR(OptionNotFound, MPV_ERROR_OPTION_NOT_FOUND);
CHECK_ERROR(OptionFormat, MPV_ERROR_OPTION_FORMAT);
CHECK_ERROR(OptionError, MPV_ERROR_OPTION_ERROR);
CHECK_ERROR(PropertyNotFound, MPV_ERROR_PROPERTY_NOT_FOUND);
CHECK_ERROR(PropertyFormat, MPV_ERROR_PROPERTY_FORMAT);
CHECK_ERROR(PropertyUnavailable, MPV_ERROR_PROPERTY_UNAVAILABLE);
CHECK_ERROR(PropertyError, MPV_ERROR_PROPERTY_ERROR);
CHECK_ERROR(Command, MPV_ERROR_COMMAND);
CHECK_ERROR(LoadingFailed, MPV_ERROR_LOADING_FAILED);
CHECK_ERROR(AoInitFailed, MPV_ERROR_AO_INIT_FAILED);
CHECK_ERROR(VoInitFailed, MPV_ERROR_VO_INIT_FAILED);
CHECK_ERROR(NothingToPlay, MPV_ERROR_NOTHING_TO_PLAY);
CHECK_ERROR(UnknownFormat, MPV_ERROR_UNKNOWN_FORMAT);
CHECK_ERROR(Unsupported, MPV_ERROR_UNSUPPORTED);
CHECK_ERROR(NotImplemented, MPV_ERROR_NOT_IMPLEMENTED);
CHECK_ERROR(Generic, MPV_ERROR_GENERIC);
#undef CHECK_ERROR

struct MpvPlayer::Private {
    MpvControllerAdapter *controller = nullptr;
    std::shared_ptr<MpvRenderBridge> bridge = std::make_shared<MpvRenderBridge>();
    mpv_render_param_type displayType = MPV_RENDER_PARAM_INVALID;
    void *display = nullptr;
    QMetaObject::Connection graphicsConnection;
    quint64 failedGeneration = 0;
    bool failureReported = false;
};

static bool nearlyEqual(double a, double b) {
    return qFuzzyCompare(a, b) || (qFuzzyIsNull(a) && qFuzzyIsNull(b));
}
static QString cleanPath(const QString &path) {
    return path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
}
static QVariantList hyphenKeys(const QVariantList &source) {
    QVariantList result;
    for (const QVariant &value : source) {
        const QVariantMap fields = value.toMap();
        if (fields.isEmpty()) { result.append(value); continue; }
        QVariantMap converted;
        for (auto field = fields.cbegin(); field != fields.cend(); ++field) {
            QString key = field.key();
            if (key == QLatin1String("id")) key = QStringLiteral("trackId");
            for (int index = key.indexOf('-'); index > 0 && index + 1 < key.size(); index = key.indexOf('-', index))
                key = key.left(index) + key[index + 1].toUpper() + key.mid(index + 2);
            converted.insert(key, field.value());
        }
        result.append(converted);
    }
    return result;
}

MpvPlayer::MpvPlayer(QQuickItem *parent, MpvControllerIface *ctrl)
    : QQuickFramebufferObject(parent), m_private(std::make_unique<Private>()) {
    m_private->bridge->dispatcher = MpvRenderDispatcher::create();
    if (ctrl) m_ctrl = ctrl;
    else m_ctrl = m_private->controller = new MpvControllerAdapter(this);
    m_logClient = new MpvLogClient(this);
    m_hookManager = new MpvHookManager(this);

    connect(m_ctrl, &MpvControllerIface::initializationFinished, this,
            [this](bool success, int error, const QString &message) {
        if (m_shuttingDown || success) return;
        const QString ownedMessage = message;
        QPointer<MpvPlayer> alive(this);
        emit initializationFailed(error, ownedMessage);
        if (alive) emitError(error, ownedMessage);
    });
    connect(m_ctrl, &MpvControllerIface::coreUnavailable, this, [this]() {
        if (m_shuttingDown) return;
        m_coreAvailable = false;
        m_deferredInitActions.clear();
        m_pendingProperties.clear();
        m_dirtyProperties.clear();
        QPointer<MpvPlayer> alive(this);
        emit coreAvailableChanged();
        if (alive) failOperation(QStringLiteral("coreUnavailable"), MPV_ERROR_UNINITIALIZED);
    });
    connect(m_ctrl, &MpvControllerIface::operationFailed, this,
            [this](const QString &operation, int error, const QString &) {
        if (!m_shuttingDown) failOperation(operation, error);
    });
    connect(m_ctrl, &QObject::destroyed, this, [this]() {
        m_ctrl = nullptr;
        if (!m_shuttingDown) { m_coreAvailable = false; emit coreAvailableChanged(); }
    });
    connect(m_ctrl, &MpvControllerIface::propertyChanged, this,
            [this](const QString &name, const QVariant &value) {
        if (m_shuttingDown) return;
        if ((!m_rendererInitialized || m_renderError < 0) && m_pendingProperties.contains(name)) return;
        const auto handler = m_propertyHandlers.value(name); // copy before reentrant delivery
        if (handler) handler(value);
    });
    connect(m_ctrl, &MpvControllerIface::fileStarted, this, [this]() {
        if (!m_shuttingDown) emit fileStarted();
    });
    connect(m_ctrl, &MpvControllerIface::fileLoaded, this, [this]() {
        if (m_shuttingDown) return;
        const quint64 intent = m_mediaRevision;
        const double seek = std::exchange(m_pendingTrackSeek, -1.0);
        QPointer<MpvPlayer> alive(this);
        emit fileLoaded();
        if (alive && !m_shuttingDown && intent == m_mediaRevision && seek >= 0) seekAbsolute(seek);
    });
    connect(m_ctrl, &MpvControllerIface::endFile, this, [this](const QString &reason) {
        if (m_shuttingDown) return;
        const QString ownedReason = reason;
        QPointer<MpvPlayer> alive(this);
        emit endFile(ownedReason);
        if (alive && !m_shuttingDown && ownedReason == QStringLiteral("error"))
            emitError(MPV_ERROR_LOADING_FAILED, QStringLiteral("Playback ended with error"));
    });
    connect(m_ctrl, &MpvControllerIface::videoReconfig, this, [this]() {
        if (!m_shuttingDown) emit videoReconfig();
    });
    connect(m_ctrl, &MpvControllerIface::asyncCommandFinished, this,
            [this](int id, int error, const QVariant &value) { complete(id, error, value, 0); }, Qt::QueuedConnection);
    connect(m_ctrl, &MpvControllerIface::mpvPropertySetFinished, this,
            [this](int id, int error) { complete(id, error, {}, 1); }, Qt::QueuedConnection);
    connect(m_ctrl, &MpvControllerIface::mpvPropertyReadFinished, this,
            [this](int id, int error, const QVariant &value) { complete(id, error, value, 2); }, Qt::QueuedConnection);
    connect(m_ctrl, &MpvControllerIface::configurationFinished, this,
            [this](int id, int error) { complete(id, error, {}, 3); }, Qt::QueuedConnection);
    m_initialization = m_ctrl->initialize();
    m_coreAvailable = m_initialization.success && m_ctrl->available();
    if (m_private->controller) m_private->bridge->core = m_private->controller->core();
    setupPropertyObservers();
    propertyIntent(QStringLiteral("hwdec"), QStringLiteral("auto"));
    propertyIntent(QStringLiteral("tscale"), m_tscale);
    propertyIntent(QStringLiteral("keepaspect"), QStringLiteral("yes"));
    propertyIntent(QStringLiteral("panscan"), 0.0);
    connect(m_logClient, &MpvLogClient::logMessage, this, &MpvPlayer::logMessage);
    connect(m_hookManager, &MpvHookManager::hookTriggered, this, &MpvPlayer::hookTriggered);
    connect(this, &MpvPlayer::ready, this, [this]() {
        if (m_shuttingDown || !coreAvailable() || m_rendererInitialized) return;
        m_rendererInitialized = true;
        QPointer<MpvPlayer> alive(this);
        emit rendererReadyChanged();
        if (alive && !m_shuttingDown) syncProperties();
    });
    auto dispatcher = m_private->bridge->dispatcher;
    connect(dispatcher.get(), &MpvRenderDispatcher::redrawRequested, this, [this]() {
        if (!m_shuttingDown) update();
    });
    connect(dispatcher.get(), &MpvRenderDispatcher::statusChanged, this, &MpvPlayer::rendererStatus);
    connect(dispatcher.get(), &MpvRenderDispatcher::parameterFailed, this,
            [this](quint64 generation, const QString &name, int error, const QString &) {
        if (!m_shuttingDown && generation == m_private->bridge->generation.load()) failOperation(name, error);
    });
    connect(dispatcher.get(), &MpvRenderDispatcher::contextRetiring, this,
            [this](quint64 generation, quint64 intent) {
        if (!m_shuttingDown && m_private->controller) m_private->controller->contextRetiring(generation, intent);
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0) && QT_CONFIG(wayland)
    if (auto *native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        m_private->displayType = MPV_RENDER_PARAM_WL_DISPLAY;
        m_private->display = native->display();
    }
#endif
#if QT_CONFIG(xcb)
    if (auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        m_private->displayType = MPV_RENDER_PARAM_X11_DISPLAY;
        m_private->display = native->display();
    }
#endif
    connect(this, &QQuickItem::windowChanged, this, &MpvPlayer::observeWindow);
    observeWindow(window());
}

MpvPlayer::~MpvPlayer() {
    m_shuttingDown = true;
    // QQuickItem's later base destruction emits windowChanged. Disconnect our
    // derived-class slots before its vtable/private state cease to exist.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_private->graphicsConnection);
    if (m_ctrl) disconnect(m_ctrl, nullptr, this, nullptr);
    if (m_private) disconnect(m_private->bridge->dispatcher.get(), nullptr, this, nullptr);
    m_deferredInitActions.clear();
    m_requests.clear();
    m_logClient->shutdown();
    m_hookManager->shutdown();
    delete std::exchange(m_logClient, nullptr);
    delete std::exchange(m_hookManager, nullptr);
    if (m_private->controller) { m_private->controller->shutdown(); delete std::exchange(m_private->controller, nullptr); }
    m_ctrl = nullptr;
    // The scenegraph renderer keeps its own core/dispatcher lease until its
    // GL-thread cleanup. Releasing the facade does not join either worker.
}

void MpvPlayer::observeWindow(QQuickWindow *attached) {
    disconnect(m_private->graphicsConnection);
    if (!attached) return;
    QPointer<QQuickWindow> guarded(attached);
    auto inspect = [this, guarded]() {
        if (m_shuttingDown || !guarded || window() != guarded || !guarded->isSceneGraphInitialized()) return;
        const auto api = guarded->rendererInterface()->graphicsApi();
        if (api != QSGRendererInterface::Unknown && api != QSGRendererInterface::OpenGL)
            rendererStatus(m_private->bridge->generation.load(), false, MPV_ERROR_UNSUPPORTED,
                           QStringLiteral("MpvPlayer requires an OpenGL Qt Quick graphics backend"));
    };
    m_private->graphicsConnection = connect(attached, &QQuickWindow::sceneGraphInitialized,
                                            this, inspect, Qt::QueuedConnection);
    if (attached->isSceneGraphInitialized()) QMetaObject::invokeMethod(this, inspect, Qt::QueuedConnection);
}

QQuickFramebufferObject::Renderer *MpvPlayer::createRenderer() const {
    const quint64 generation = m_private->bridge->generation.fetch_add(1) + 1;
    return new MpvRenderer([this]() {
        MpvRenderSnapshot snapshot;
        if (!m_shuttingDown) snapshot.core = m_private->bridge->core;
        snapshot.sourceIntent = snapshot.core ? snapshot.core->sourceIntent() : 0;
        snapshot.displayType = m_private->displayType;
        snapshot.nativeDisplay = m_private->display;
        snapshot.parameters = m_pendingRenderParams;
        return snapshot;
    }, m_private->bridge->dispatcher, generation);
}

void MpvPlayer::rendererStatus(quint64 generation, bool readyValue, int error, const QString &message) {
    if (m_shuttingDown || generation != m_private->bridge->generation.load()) return;
    // Window API inspection and native initialization can identify the same
    // failed attempt. Preserve its first error and publish failure once.
    if (error < 0) {
        if (m_private->failureReported && m_private->failedGeneration == generation) return;
        m_private->failureReported = true;
        m_private->failedGeneration = generation;
    }
    const QString ownedMessage = message;
    QPointer<MpvPlayer> alive(this);
    if (m_renderError != error || m_renderErrorString != ownedMessage) {
        m_renderError = error;
        m_renderErrorString = ownedMessage;
        emit renderErrorChanged();
        if (!alive || m_shuttingDown) return;
    }
    if (!readyValue && m_rendererInitialized) {
        m_rendererInitialized = false;
        emit rendererReadyChanged();
        if (!alive || m_shuttingDown) return;
    }
    if (error < 0) {
        // A failed render attempt does not retire the core or caller intent.
        // A later scene-graph generation can create a successful renderer.
        emit renderingFailed(error, ownedMessage);
        if (alive) emitError(error, ownedMessage);
        return;
    }
    if (readyValue && !m_rendererInitialized) {
        emit ready();
        if (alive && !m_shuttingDown && m_private->controller) m_private->controller->contextReady(generation);
    }
}

void MpvPlayer::setupPropertyObservers() {
    auto add = [this](const QString &name, mpv_format format, std::function<void(const QVariant &)> handler) {
        m_propertyHandlers.insert(name, std::move(handler));
        if (m_ctrl && coreAvailable()) m_ctrl->observeProperty(name, format);
    };
#define OBSERVE(Name, Format, Member, Conversion, Signal) \
    add(QStringLiteral(Name), Format, [this](const QVariant &value) { \
        const auto incoming = value.Conversion(); \
        if (Member != incoming) { Member = incoming; emit Signal(); } \
    })
#define OBSERVE_DOUBLE(Name, Member, Signal) \
    add(QStringLiteral(Name), MPV_FORMAT_DOUBLE, [this](const QVariant &value) { \
        const double incoming = value.toDouble(); \
        if (!nearlyEqual(Member, incoming)) { Member = incoming; emit Signal(); } \
    })
    add(QStringLiteral("video-params/w"), MPV_FORMAT_INT64, [this](const QVariant &value) {
        const int width = value.toInt();
        if (width == m_videoWidth) return;
        m_videoWidth = width;
        QPointer<MpvPlayer> alive(this);
        emit videoWidthChanged();
        if (!alive || m_shuttingDown) return;
        const bool video = m_videoWidth > 0;
        if (m_hasVideo != video) { m_hasVideo = video; emit hasVideoChanged(); }
    });
    OBSERVE("video-params/h", MPV_FORMAT_INT64, m_videoHeight, toInt, videoHeightChanged);
    OBSERVE("pause", MPV_FORMAT_FLAG, m_paused, toBool, pausedChanged);
    OBSERVE_DOUBLE("speed", m_playbackSpeed, playbackSpeedChanged);
    OBSERVE("mute", MPV_FORMAT_FLAG, m_mute, toBool, muteChanged);
    OBSERVE("screenshot-directory", MPV_FORMAT_STRING, m_screenshotDirectory, toString, screenshotDirectoryChanged);
    OBSERVE_DOUBLE("volume", m_volume, volumeChanged);
    OBSERVE_DOUBLE("time-pos", m_position, positionChanged);
    OBSERVE_DOUBLE("duration", m_duration, durationChanged);
    OBSERVE("seeking", MPV_FORMAT_FLAG, m_seeking, toBool, seekingChanged);
    OBSERVE("paused-for-cache", MPV_FORMAT_FLAG, m_isBuffering, toBool, isBufferingChanged);
    add(QStringLiteral("eof-reached"), MPV_FORMAT_FLAG, [this](const QVariant &value) { if (value.toBool()) emit playbackEnded(); });
    OBSERVE("audio-device-list", MPV_FORMAT_NODE, m_audioDeviceList, toList, audioDeviceListChanged);
    OBSERVE("audio-device", MPV_FORMAT_STRING, m_currentAudioDevice, toString, currentAudioDeviceChanged);
    OBSERVE("hwdec", MPV_FORMAT_STRING, m_hwdec, toString, hwdecChanged);
    OBSERVE("video-sync", MPV_FORMAT_STRING, m_videoSync, toString, videoSyncChanged);
    OBSERVE("interpolation", MPV_FORMAT_FLAG, m_interpolation, toBool, interpolationChanged);
    OBSERVE("tscale", MPV_FORMAT_STRING, m_tscale, toString, tscaleChanged);
    OBSERVE("glsl-shaders", MPV_FORMAT_STRING, m_userShaders, toString, userShadersChanged);
    OBSERVE("ytdl", MPV_FORMAT_FLAG, m_ytdl, toBool, ytdlChanged);
    OBSERVE_DOUBLE("audio-delay", m_audioDelay, audioDelayChanged);
    OBSERVE_DOUBLE("sub-delay", m_subDelay, subDelayChanged);
    add(QStringLiteral("track-list"), MPV_FORMAT_NODE, [this](const QVariant &value) { m_trackList = hyphenKeys(value.toList()); emit trackListChanged(); });
    add(QStringLiteral("playlist"), MPV_FORMAT_NODE, [this](const QVariant &value) { m_playlist = hyphenKeys(value.toList()); emit playlistChanged(); });
    add(QStringLiteral("chapter-list"), MPV_FORMAT_NODE, [this](const QVariant &value) {
        m_chapterList.clear();
        for (const auto &chapter : value.toList()) m_chapterList.append(chapter.toMap().value(QStringLiteral("time")).toDouble());
        emit chapterListChanged();
    });
    OBSERVE("metadata", MPV_FORMAT_NODE, m_metadata, toMap, metadataChanged);
    add(QStringLiteral("hwdec-current"), MPV_FORMAT_STRING, [this](const QVariant &value) {
        QString incoming = value.toString();
        if (incoming.isEmpty()) incoming = QStringLiteral("no");
        if (m_hwdecCurrent != incoming) { m_hwdecCurrent = incoming; emit hwdecCurrentChanged(); }
    });
    OBSERVE("demuxer-cache-state", MPV_FORMAT_NODE, m_demuxerCacheState, toMap, demuxerCacheStateChanged);
#undef OBSERVE_DOUBLE
#undef OBSERVE
}

quint64 MpvPlayer::propertyIntent(const QString &name, const QVariant &value) {
    const quint64 revision = ++m_propertyRevisions[name];
    if (coreAvailable()) {
        m_pendingProperties.insert(name, value);
        m_dirtyProperties.insert(name);
    }
    return revision;
}

void MpvPlayer::flushProperty(const QString &name, quint64 revision) {
    if (m_propertyRevisions.value(name) != revision) return;
    if (!coreAvailable()) { failOperation(name, MPV_ERROR_UNINITIALIZED); return; }
    if (!m_rendererInitialized || m_renderError < 0) return;
    const QVariant value = m_pendingProperties.take(name);
    m_dirtyProperties.remove(name);
    submitProperty(name, value);
}

#define SET_PROPERTY(Member, Signal, Name, Value) \
    if (Member != val) { \
        Member = val; \
        const quint64 revision = propertyIntent(QStringLiteral(Name), Value); \
        QPointer<MpvPlayer> alive(this); \
        emit Signal(); \
        if (alive) flushProperty(QStringLiteral(Name), revision); \
    }

void MpvPlayer::setSource(const QString &val) {
    if (m_source == val) return;
    m_source = val;
    const quint64 revision = ++m_mediaRevision;
    m_deferredInitActions.clear();
    QPointer<MpvPlayer> alive(this);
    emit sourceChanged();
    if (alive && revision == m_mediaRevision && !m_shuttingDown) load(val);
}
void MpvPlayer::setPaused(bool val) { SET_PROPERTY(m_paused, pausedChanged, "pause", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setPlaybackSpeed(double val) { val = qMax(0.01, val); SET_PROPERTY(m_playbackSpeed, playbackSpeedChanged, "speed", val) }
void MpvPlayer::setMute(bool val) { SET_PROPERTY(m_mute, muteChanged, "mute", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setVolume(double val) { val = qBound(0.0, val, 150.0); SET_PROPERTY(m_volume, volumeChanged, "volume", val) }
void MpvPlayer::setScreenshotDirectory(const QString &value) { const QString val = cleanPath(value); SET_PROPERTY(m_screenshotDirectory, screenshotDirectoryChanged, "screenshot-directory", val) }
void MpvPlayer::setLoopFile(const QVariant &val) { SET_PROPERTY(m_loopFile, loopFileChanged, "loop-file", val.toString()) }
void MpvPlayer::setLoopPlaylist(const QVariant &val) { SET_PROPERTY(m_loopPlaylist, loopPlaylistChanged, "loop-playlist", val.toString()) }
void MpvPlayer::setCurrentAudioDevice(const QString &val) { SET_PROPERTY(m_currentAudioDevice, currentAudioDeviceChanged, "audio-device", val) }
void MpvPlayer::setHwdec(const QString &val) { SET_PROPERTY(m_hwdec, hwdecChanged, "hwdec", val) }
void MpvPlayer::setVideoSync(const QString &val) { SET_PROPERTY(m_videoSync, videoSyncChanged, "video-sync", val) }
void MpvPlayer::setInterpolation(bool val) { SET_PROPERTY(m_interpolation, interpolationChanged, "interpolation", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setTscale(const QString &val) { SET_PROPERTY(m_tscale, tscaleChanged, "tscale", val) }
void MpvPlayer::setUserShaders(const QString &val) { SET_PROPERTY(m_userShaders, userShadersChanged, "glsl-shaders", val) }
void MpvPlayer::setYtdl(bool val) { SET_PROPERTY(m_ytdl, ytdlChanged, "ytdl", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setAudioDelay(double val) { SET_PROPERTY(m_audioDelay, audioDelayChanged, "audio-delay", val) }
void MpvPlayer::setSubDelay(double val) { SET_PROPERTY(m_subDelay, subDelayChanged, "sub-delay", val) }
void MpvPlayer::setVideoZoom(double val) { SET_PROPERTY(m_videoZoom, videoZoomChanged, "video-zoom", val) }
void MpvPlayer::setVideoPanX(double val) { SET_PROPERTY(m_videoPanX, videoPanXChanged, "video-pan-x", val) }
void MpvPlayer::setVideoPanY(double val) { SET_PROPERTY(m_videoPanY, videoPanYChanged, "video-pan-y", val) }
void MpvPlayer::setAudioPitchCorrection(bool val) { SET_PROPERTY(m_audioPitchCorrection, audioPitchCorrectionChanged, "audio-pitch-correction", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setSubVisibility(bool val) { SET_PROPERTY(m_subVisibility, subVisibilityChanged, "sub-visibility", val ? QStringLiteral("yes") : QStringLiteral("no")) }
void MpvPlayer::setBrightness(double val) { val = qBound(-100.0, val, 100.0); SET_PROPERTY(m_brightness, brightnessChanged, "brightness", val) }
void MpvPlayer::setContrast(double val) { val = qBound(-100.0, val, 100.0); SET_PROPERTY(m_contrast, contrastChanged, "contrast", val) }
#undef SET_PROPERTY

void MpvPlayer::setFillMode(const QString &val) {
    if (m_fillMode == val) return;
    m_fillMode = val;
    const quint64 aspect = propertyIntent(QStringLiteral("keepaspect"), val == QStringLiteral("stretch") ? QStringLiteral("no") : QStringLiteral("yes"));
    const quint64 scan = propertyIntent(QStringLiteral("panscan"), val == QStringLiteral("preserveAspectCrop") ? 1.0 : 0.0);
    QPointer<MpvPlayer> alive(this);
    emit fillModeChanged();
    if (!alive) return;
    flushProperty(QStringLiteral("keepaspect"), aspect);
    if (alive) flushProperty(QStringLiteral("panscan"), scan);
}

void MpvPlayer::syncProperties() {
    if (!coreAvailable() || m_shuttingDown || m_renderError < 0) return;
    const QSet<QString> dirty = std::exchange(m_dirtyProperties, {});
    const QVariantMap pending = std::exchange(m_pendingProperties, {});
    const auto revisions = m_propertyRevisions;
    const QVariantMap defaults{
        {"keepaspect", m_fillMode == "stretch" ? "no" : "yes"}, {"panscan", m_fillMode == "preserveAspectCrop" ? 1.0 : 0.0},
        {"screenshot-directory", m_screenshotDirectory}, {"loop-file", m_loopFile.toString()}, {"loop-playlist", m_loopPlaylist.toString()},
        {"video-zoom", m_videoZoom}, {"video-pan-x", m_videoPanX}, {"video-pan-y", m_videoPanY},
        {"audio-pitch-correction", m_audioPitchCorrection ? "yes" : "no"}, {"sub-visibility", m_subVisibility ? "yes" : "no"},
        {"brightness", m_brightness}, {"contrast", m_contrast}, {"speed", m_playbackSpeed}, {"mute", m_mute ? "yes" : "no"},
        {"volume", m_volume}, {"audio-device", m_currentAudioDevice}, {"hwdec", m_hwdec}, {"video-sync", m_videoSync},
        {"interpolation", m_interpolation ? "yes" : "no"}, {"tscale", m_tscale}, {"glsl-shaders", m_userShaders},
        {"ytdl", m_ytdl ? "yes" : "no"}, {"audio-delay", m_audioDelay}, {"sub-delay", m_subDelay}, {"pause", m_paused ? "yes" : "no"}
    };
    // Keep the existing ordering: fill mode first and pause last, before loads.
    QStringList order{"keepaspect", "panscan"};
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it)
        if (it.key() != "keepaspect" && it.key() != "panscan" && it.key() != "pause") order.append(it.key());
    order.append(QStringLiteral("pause"));
    QPointer<MpvPlayer> alive(this);
    for (const QString &name : order) {
        if (!dirty.contains(name) || revisions.value(name) != m_propertyRevisions.value(name)) continue;
        const QVariant value = pending.value(name, defaults.value(name));
        if ((name == "screenshot-directory" || name == "glsl-shaders") && value.toString().isEmpty()) continue;
        submitProperty(name, value);
        if (!alive || m_shuttingDown || !coreAvailable()) return;
    }
    const auto actions = std::exchange(m_deferredInitActions, {});
    for (const auto &action : actions) {
        action.retry();
        if (!alive || m_shuttingDown || !coreAvailable()) return;
    }
}

void MpvPlayer::submitProperty(const QString &name, const QVariant &value) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->setMpvPropertyAsync(name, value) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(name, id);
    else m_requests.insert(id, {1, false, name});
}
void MpvPlayer::submitCommand(const QStringList &params) {
    const QString operation = params.isEmpty() ? QStringLiteral("command") : params[0];
    const int id = coreAvailable() && m_ctrl ? m_ctrl->commandAsync(params) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(operation, id);
    else m_requests.insert(id, {0, false, operation});
}
int MpvPlayer::commandAsync(const QStringList &params) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->commandAsync(params) : MPV_ERROR_UNINITIALIZED;
    if (id > 0) m_requests.insert(id, {0, true, QStringLiteral("commandAsync")});
    return id;
}
int MpvPlayer::setMpvPropertyAsync(const QString &name, const QVariant &value) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->setMpvPropertyAsync(name, value) : MPV_ERROR_UNINITIALIZED;
    if (id > 0) m_requests.insert(id, {1, true, name});
    return id;
}
int MpvPlayer::readMpvPropertyAsync(const QString &name) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->readMpvPropertyAsync(name) : MPV_ERROR_UNINITIALIZED;
    if (id > 0) m_requests.insert(id, {2, true, name});
    return id;
}
void MpvPlayer::complete(int id, int error, const QVariant &value, int kind) {
    const auto found = m_requests.find(id);
    if (found == m_requests.end() || found->kind != kind) return;
    const RequestOwner owner = found.value();
    m_requests.erase(found);
    const QVariant ownedValue = value;
    QPointer<MpvPlayer> alive(this);
    if (owner.caller) {
        if (kind == 0) emit asyncCommandFinished(id, error, ownedValue);
        else if (kind == 1) emit mpvPropertySetFinished(id, error);
        else if (kind == 2) emit mpvPropertyReadFinished(id, error, ownedValue);
    }
    if (alive && error < 0) failOperation(owner.operation, error);
}

bool MpvPlayer::deferUntilRendererReady(const QString &operation, const QString &stateKey, std::function<void()> retry) {
    if (!coreAvailable()) { failOperation(operation, MPV_ERROR_UNINITIALIZED); return true; }
    if (!m_rendererInitialized || m_renderError < 0) {
        const quint64 revision = m_mediaRevision;
        auto guardedRetry = [this, revision, retry = std::move(retry)]() {
            if (revision == m_mediaRevision) retry();
        };
        // Latest state follows any intervening commands; commands (empty key)
        // retain every occurrence. One queue shares all cancellation.
        if (!stateKey.isEmpty()) {
            for (auto it = m_deferredInitActions.begin(); it != m_deferredInitActions.end(); ++it) {
                if (it->stateKey != stateKey) continue;
                m_deferredInitActions.erase(it);
                break;
            }
        }
        m_deferredInitActions.append({stateKey, std::move(guardedRetry)});
        return true;
    }
    return false;
}

void MpvPlayer::load(const QString &url, const QString &mode) {
    if (url.isEmpty()) return;
    if (deferUntilRendererReady(QStringLiteral("load"), {}, [this, url, mode]() { load(url, mode); })) return;
    submitCommand(QStringList{"loadfile", cleanPath(url), mode});
}
void MpvPlayer::loadDirectory(const QString &path, const QStringList &fileFilters, const QString &mode, const QString &sort) {
    if (path.isEmpty()) return;
    if (deferUntilRendererReady(QStringLiteral("loadDirectory"), {}, [this, path, fileFilters, mode, sort]() {
        loadDirectory(path, fileFilters, mode, sort);
    })) return;
    const QString directoryPath = cleanPath(path);
    if (!QFileInfo(directoryPath).isDir()) return;
    QStringList filters = fileFilters;
    if (filters.isEmpty()) filters = {"*.mp4", "*.mkv", "*.avi", "*.webm", "*.mov", "*.mp3", "*.flac"};
    QStringList expanded;
    for (const auto &filter : filters) {
        expanded.append(filter);
        if (filter.startsWith("*.") && filter.toUpper() != filter) expanded.append(filter.toUpper());
    }
    QDir::SortFlags flags = QDir::IgnoreCase;
    if (sort == "date" || sort == "time") flags |= QDir::Time;
    else if (sort == "size") flags |= QDir::Size;
    else flags |= QDir::Name;
    const auto files = QDir(directoryPath).entryInfoList(expanded, QDir::Files, flags);
    QPointer<MpvPlayer> alive(this);
    bool first = true;
    const quint64 revision = m_mediaRevision;
    for (const auto &file : files) {
        const QString action = first && mode == "replace" ? QStringLiteral("replace") : QStringLiteral("append");
        submitCommand(QStringList{"loadfile", file.absoluteFilePath(), action});
        if (!alive || m_shuttingDown || revision != m_mediaRevision) return;
        first = false;
    }
}
void MpvPlayer::stop(bool keepPlaylist) {
    ++m_mediaRevision;
    m_deferredInitActions.clear();
    m_pendingTrackSeek = -1;
    const QStringList params = keepPlaylist ? QStringList{"stop", "keep-playlist"} : QStringList{"stop"};
    const int id = coreAvailable() && m_ctrl ? m_ctrl->commandAsync(params) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(QStringLiteral("stop"), id);
    else m_requests.insert(id, {0, false, QStringLiteral("stop")});
}
void MpvPlayer::clearPlaylist() { submitCommand(QStringList{"playlist-clear"}); }
void MpvPlayer::seekRelative(double seconds) { submitCommand(QStringList{"seek", QString::number(seconds), "relative"}); }
void MpvPlayer::seekAbsolute(double timestamp) { submitCommand(QStringList{"seek", QString::number(qMax(0.0, timestamp)), "absolute"}); }
void MpvPlayer::next() { ++m_mediaRevision; m_deferredInitActions.clear(); submitCommand(QStringList{"playlist-next", "force"}); }
void MpvPlayer::previous() { ++m_mediaRevision; m_deferredInitActions.clear(); submitCommand(QStringList{"playlist-prev", "force"}); }
void MpvPlayer::frameStep() { submitCommand(QStringList{"frame-step"}); }
void MpvPlayer::frameBackStep() { submitCommand(QStringList{"frame-back-step"}); }
void MpvPlayer::markSeekPosition() { submitCommand(QStringList{"revert-seek", "mark"}); }
void MpvPlayer::revertSeek() { submitCommand(QStringList{"revert-seek"}); }
void MpvPlayer::addAudioTrack(const QString &url) { if (!url.isEmpty()) submitCommand(QStringList{"audio-add", cleanPath(url)}); }
void MpvPlayer::addVideoTrack(const QString &url) { if (!url.isEmpty()) submitCommand(QStringList{"video-add", cleanPath(url)}); }
void MpvPlayer::addSubtitleTrack(const QString &url) { if (!url.isEmpty()) submitCommand(QStringList{"sub-add", cleanPath(url)}); }
void MpvPlayer::selectTrack(const QString &type, int id) {
    QString name;
    if (type == "audio") name = QStringLiteral("aid");
    else if (type == "video") name = QStringLiteral("vid");
    else if (type == "sub" || type == "subtitle") name = QStringLiteral("sid");
    if (name.isEmpty()) return;
    if (deferUntilRendererReady(QStringLiteral("selectTrack"), name, [this, type, id]() { selectTrack(type, id); })) return;
    QPointer<MpvPlayer> alive(this);
    submitProperty(name, id <= 0 ? QVariant(QStringLiteral("no")) : QVariant(id));
    if (alive && !m_shuttingDown && type == "video" && !m_source.isEmpty()) { m_pendingTrackSeek = m_position; load(m_source); }
}
void MpvPlayer::setVideoFilter(const QString &op, const QString &value) { submitCommand(QStringList{"vf", op, value}); }
void MpvPlayer::setAudioFilter(const QString &op, const QString &value) { submitCommand(QStringList{"af", op, value}); }
void MpvPlayer::adjustProperty(const QString &name, double value) { submitCommand(QStringList{"add", name, QString::number(value)}); }
void MpvPlayer::cycleProperty(const QString &name, const QString &direction) { submitCommand(QStringList{"cycle", name, direction}); }
void MpvPlayer::multiplyProperty(const QString &name, double factor) { submitCommand(QStringList{"multiply", name, QString::number(factor)}); }
void MpvPlayer::deleteProperty(const QString &name) { submitCommand(QStringList{"del", name}); }
void MpvPlayer::toggleAbLoop() { submitCommand(QStringList{"ab-loop"}); }
void MpvPlayer::saveWatchLaterState() { submitCommand(QStringList{"write-watch-later-config"}); }
void MpvPlayer::dropPlaybackBuffers() { submitCommand(QStringList{"drop-buffers"}); }
void MpvPlayer::screenshot(const QString &mode) { submitCommand(QStringList{"screenshot", mode}); }
void MpvPlayer::runExternalProcess(const QStringList &arguments) { if (!arguments.isEmpty()) submitCommand(QStringList{"run"} + arguments); }
void MpvPlayer::sendScriptMessage(const QStringList &arguments) { if (!arguments.isEmpty()) submitCommand(QStringList{"script-message"} + arguments); }
void MpvPlayer::triggerScriptBinding(const QString &name) { if (!name.isEmpty()) submitCommand(QStringList{"script-binding", name}); }
void MpvPlayer::sendMousePosition(int x, int y) {
    if (deferUntilRendererReady(QStringLiteral("sendMousePosition"), QStringLiteral("mouse-pos"), [this, x, y]() { sendMousePosition(x, y); })) return;
    QPointer<MpvPlayer> alive(this);
    submitProperty(QStringLiteral("mouse-pos/x"), x);
    if (alive && !m_shuttingDown) submitProperty(QStringLiteral("mouse-pos/y"), y);
}
void MpvPlayer::sendMouseClick(int button, bool press) {
    submitCommand(QStringList{press ? "keydown" : "keyup", QStringLiteral("MOUSE_BTN") + QString::number(button - 1)});
}

void MpvPlayer::emitError(int code, const QString &message) {
    const QString ownedMessage = message;
    const quint64 revision = ++m_errorRevision;
    m_lastError = {{QStringLiteral("code"), code}, {QStringLiteral("message"), ownedMessage}};
    QPointer<MpvPlayer> alive(this);
    emit lastErrorChanged();
    if (alive && revision == m_errorRevision) emit errorOccurred(code, ownedMessage);
}
void MpvPlayer::failOperation(const QString &operation, int error) {
    const QString ownedOperation = operation;
    const QString message = QString::fromUtf8(mpv_error_string(error));
    const quint64 previousError = m_errorRevision;
    QPointer<MpvPlayer> alive(this);
    emit operationFailed(ownedOperation, error, message);
    if (alive && previousError == m_errorRevision) emitError(error, message);
}
void MpvPlayer::setLogLevel(const QString &level) {
    if (!coreAvailable()) { failOperation(QStringLiteral("setLogLevel"), MPV_ERROR_UNINITIALIZED); return; }
    if (level != "no") m_logClient->setupLogClient(m_private->controller ? m_private->controller->helperCore() : nullptr);
    const int error = m_logClient->setLogLevel(level);
    if (error < 0) failOperation(QStringLiteral("setLogLevel"), error);
}
void MpvPlayer::loadConfigFile(const QString &path) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->loadConfigurationAsync(path) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(QStringLiteral("loadConfigFile"), id);
    else m_requests.insert(id, {3, false, QStringLiteral("loadConfigFile")});
}
void MpvPlayer::setRenderParameter(const QString &name, const QVariant &value) {
    if (!coreAvailable()) { failOperation(name, MPV_ERROR_UNINITIALIZED); return; }
    if (name != QStringLiteral("ambient-light")) { failOperation(name, MPV_ERROR_UNSUPPORTED); return; }
    bool valid = false;
    const qlonglong lux = value.toLongLong(&valid);
    if (!valid || lux < 0 || lux > std::numeric_limits<int>::max()
        || (value.typeId() != QMetaType::Int && value.typeId() != QMetaType::LongLong
            && value.typeId() != QMetaType::UInt && value.typeId() != QMetaType::ULongLong
            && !(value.typeId() == QMetaType::Double && std::isfinite(value.toDouble()) && value.toDouble() == static_cast<double>(lux)))) {
        failOperation(name, MPV_ERROR_INVALID_PARAMETER); return;
    }
    m_pendingRenderParams.insert(name, static_cast<int>(lux));
    update();
}
void MpvPlayer::hookAdd(const QString &name, int priority) {
    if (!coreAvailable()) { failOperation(QStringLiteral("hookAdd"), MPV_ERROR_UNINITIALIZED); return; }
    m_hookManager->setupHookClient(m_private->controller ? m_private->controller->helperCore() : nullptr);
    const int error = m_hookManager->hookAdd(name, priority);
    if (error < 0) failOperation(QStringLiteral("hookAdd"), error);
}
void MpvPlayer::hookContinue(quint64 id) {
    const int error = !m_shuttingDown ? m_hookManager->hookContinue(id) : MPV_ERROR_UNINITIALIZED;
    if (error < 0) failOperation(QStringLiteral("hookContinue"), error);
}
void MpvPlayer::shutdown() {
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_coreAvailable = false;
    m_rendererInitialized = false;
    ++m_mediaRevision;
    m_deferredInitActions.clear();
    m_dirtyProperties.clear();
    m_pendingProperties.clear();
    m_logClient->shutdown();
    m_hookManager->shutdown();
    const auto requests = std::exchange(m_requests, {});
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        const int id = it.key();
        const RequestOwner owner = it.value();
        QMetaObject::invokeMethod(this, [this, id, owner]() {
            if (owner.caller) {
                if (owner.kind == 0) emit asyncCommandFinished(id, MPV_ERROR_UNINITIALIZED, {});
                else if (owner.kind == 1) emit mpvPropertySetFinished(id, MPV_ERROR_UNINITIALIZED);
                else if (owner.kind == 2) emit mpvPropertyReadFinished(id, MPV_ERROR_UNINITIALIZED, {});
            } else failOperation(owner.operation, MPV_ERROR_UNINITIALIZED);
        }, Qt::QueuedConnection);
    }
    if (m_private->controller) m_private->controller->shutdown();
    QPointer<MpvPlayer> alive(this);
    emit coreAvailableChanged();
    if (alive) emit rendererReadyChanged();
}
