#include "mpvplayer.h"
#include "mpvcontroller_adapter.h"
#include "mpvrenderer_p.h"
#include "mpvlogclient.h"
#include "mpvhookmanager.h"
#include "mpvmediaeffect_p.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QPromise>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QThreadPool>
#include <QTimer>
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

struct DirectoryScan {
    enum Status { MissingDirectory, NoMatches, Found } status;
    QStringList files;
};

struct MpvPlayer::Private {
    struct Property {
        mpv_format format = MPV_FORMAT_NONE;
        std::function<QVariant(const QVariant &)> fromNative;
        std::function<QVariant(const QVariant &)> toNative;
        std::function<QVariant(const QVariant &)> normalize;
        std::function<QVariant()> current;
        std::function<void(const QVariant &)> publish;
        QVariant observed;
        QVariant desired; // invalid means no unresolved intent; an empty string is a real value
        quint64 revision = 0;
        quint64 writing = 0;
        bool queued = false;
    };
    QStringList propertyOrder;
    QHash<QString, Property> properties;
    QHash<QString, QString> observedAliases;
    MpvControllerAdapter *controller = nullptr;
    std::shared_ptr<MpvRenderBridge> bridge = std::make_shared<MpvRenderBridge>();
    mpv_render_param_type displayType = MPV_RENDER_PARAM_INVALID;
    void *display = nullptr;
    QMetaObject::Connection graphicsConnection;
    std::unique_ptr<QRunnable> renderRelease;
    QPointer<QQuickWindow> renderWindow;
    quint64 failedGeneration = 0;
    bool failureReported = false;
    bool drainingMedia = false;
    quint64 directoryTicket = 0;
    bool directoryActive = false;
    QPointer<QFutureWatcher<DirectoryScan>> directoryWatcher;
    QStringList directoryFiles;
    qsizetype directoryIndex = 0;
    QString directoryFirstMode;
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
        QPointer<MpvPlayer> alive(this);
        shutdown();
        if (alive) failOperation(QStringLiteral("coreUnavailable"), MPV_ERROR_UNINITIALIZED);
    });
    connect(m_ctrl, &MpvControllerIface::operationFailed, this,
            [this](const QString &operation, int error, const QString &) {
        if (!m_shuttingDown) failOperation(operation, error);
    });
    connect(m_ctrl, &QObject::destroyed, this, [this]() {
        m_ctrl = nullptr;
        if (!m_shuttingDown) shutdown();
    });
    connect(m_ctrl, &MpvControllerIface::propertyChanged, this,
            [this](const QString &name, const QVariant &value) {
        if (m_shuttingDown) return;
        const QString ownedName = m_private->observedAliases.value(name, name);
        if (m_private->properties.contains(ownedName)) {
            observeOwnedProperty(ownedName, value);
            return;
        }
        const auto handler = m_propertyHandlers.value(name); // copy before reentrant delivery
        if (handler) handler(value);
    });
    connect(m_ctrl, &MpvControllerIface::fileStarted, this, [this]() {
        if (!m_shuttingDown) emit fileStarted();
    });
    connect(m_ctrl, &MpvControllerIface::fileLoaded, this, [this]() {
        if (!m_shuttingDown) emit fileLoaded();
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
    // The production adapter already receives queued worker values. Consume
    // replies and observations in that order, not on different GUI turns.
    const auto replyDelivery = ctrl ? Qt::QueuedConnection : Qt::DirectConnection;
    connect(m_ctrl, &MpvControllerIface::asyncCommandFinished, this,
            [this](int id, int error, const QVariant &value) { complete(id, error, value, 0); }, Qt::QueuedConnection);
    connect(m_ctrl, &MpvControllerIface::mpvPropertySetFinished, this,
            [this](int id, int error) { complete(id, error, {}, 1); }, replyDelivery);
    connect(m_ctrl, &MpvControllerIface::mpvPropertyReadFinished, this,
            [this](int id, int error, const QVariant &value) { complete(id, error, value, 2); }, replyDelivery);
    connect(m_ctrl, &MpvControllerIface::configurationFinished, this,
            [this](int id, int error) { complete(id, error, {}, 3); }, Qt::QueuedConnection);
    m_initialization = m_ctrl->initialize();
    m_coreAvailable = m_initialization.success && m_ctrl->available();
    if (m_private->controller) m_private->bridge->core = m_private->controller->core();
    setupPropertyObservers();
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
    // QQuickItem's later base destruction emits windowChanged. Disconnect our
    // derived-class slots before its vtable/private state cease to exist.
    disconnect(this, nullptr, this, nullptr);
    detach(false);
    delete std::exchange(m_logClient, nullptr);
    delete std::exchange(m_hookManager, nullptr);
    delete std::exchange(m_private->controller, nullptr);
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
    auto *renderer = new MpvRenderer([this]() {
        MpvRenderSnapshot snapshot;
        if (!m_shuttingDown) snapshot.core = m_private->bridge->core;
        snapshot.sourceIntent = snapshot.core ? snapshot.core->sourceIntent() : 0;
        snapshot.displayType = m_private->displayType;
        snapshot.nativeDisplay = m_private->display;
        snapshot.parameters = m_pendingRenderParams;
        return snapshot;
    }, m_private->bridge->dispatcher, generation);
    m_private->renderRelease = renderer->releaseJob();
    m_private->renderWindow = window();
    return renderer;
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

static QVariant propertyIdentity(const QVariant &value) { return value; }
static QVariant propertyFlag(const QVariant &value) { return value.toBool(); }
static QVariant propertyNumber(const QVariant &value) { return value.toDouble(); }
static QVariant propertyString(const QVariant &value) { return value.toString(); }
static QVariant propertyYesNo(const QVariant &value) {
    return value.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
}
static QVariant shaderPathList(const QVariant &value) {
    if (value.metaType() != QMetaType::fromType<QVariantList>()) return value.toString();
    QStringList paths;
    for (const auto &path : value.toList()) paths.append(path.toString());
#ifdef Q_OS_WIN
    return paths.join(u';');
#else
    return paths.join(u':');
#endif
}
static QVariant shaderPathsToNative(const QVariant &value) {
    // A string "" is parsed as one empty path, not an empty path list.
    return value.toString().isEmpty() ? QVariant(QVariantList()) : value;
}
static QVariant screenshotPath(const QVariant &value) { return cleanPath(value.toString()); }
static QVariant minimumSpeed(const QVariant &value) { return qMax(0.01, value.toDouble()); }
static QVariant volumeRange(const QVariant &value) { return qBound(0.0, value.toDouble(), 150.0); }
static QVariant colorRange(const QVariant &value) { return qBound(-100.0, value.toDouble(), 100.0); }
static QVariant loopIntent(const QVariant &value) { return value.isValid() ? value : QVariant(QString()); }
template<class T> static bool propertyEqual(const T &a, const T &b) { return a == b; }
static bool propertyEqual(double a, double b) { return nearlyEqual(a, b); }

// Order is part of readiness: apply aspect policy first and pause before
// deferred media admission. Seed only the library's intentional defaults.
#define OWNED_PROPERTIES(PROPERTY) \
    PROPERTY(screenshotDirectory, ScreenshotDirectory, const QString &, "screenshot-directory", STRING, propertyString, propertyIdentity, screenshotPath, false) \
    PROPERTY(loopFile, LoopFile, const QVariant &, "loop-file", NODE, propertyIdentity, propertyString, loopIntent, false) \
    PROPERTY(loopPlaylist, LoopPlaylist, const QVariant &, "loop-playlist", NODE, propertyIdentity, propertyString, loopIntent, false) \
    PROPERTY(videoZoom, VideoZoom, double, "video-zoom", DOUBLE, propertyNumber, propertyIdentity, propertyIdentity, false) \
    PROPERTY(videoPanX, VideoPanX, double, "video-pan-x", DOUBLE, propertyNumber, propertyIdentity, propertyIdentity, false) \
    PROPERTY(videoPanY, VideoPanY, double, "video-pan-y", DOUBLE, propertyNumber, propertyIdentity, propertyIdentity, false) \
    PROPERTY(audioPitchCorrection, AudioPitchCorrection, bool, "audio-pitch-correction", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false) \
    PROPERTY(subVisibility, SubVisibility, bool, "sub-visibility", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false) \
    PROPERTY(brightness, Brightness, double, "brightness", DOUBLE, propertyNumber, propertyIdentity, colorRange, false) \
    PROPERTY(contrast, Contrast, double, "contrast", DOUBLE, propertyNumber, propertyIdentity, colorRange, false) \
    PROPERTY(playbackSpeed, PlaybackSpeed, double, "speed", DOUBLE, propertyNumber, propertyIdentity, minimumSpeed, false) \
    PROPERTY(mute, Mute, bool, "mute", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false) \
    PROPERTY(volume, Volume, double, "volume", DOUBLE, propertyNumber, propertyIdentity, volumeRange, false) \
    PROPERTY(currentAudioDevice, CurrentAudioDevice, const QString &, "audio-device", STRING, propertyString, propertyIdentity, propertyIdentity, false) \
    PROPERTY(hwdec, Hwdec, const QString &, "hwdec", STRING, propertyString, propertyIdentity, propertyIdentity, true) \
    PROPERTY(videoSync, VideoSync, const QString &, "video-sync", STRING, propertyString, propertyIdentity, propertyIdentity, false) \
    PROPERTY(interpolation, Interpolation, bool, "interpolation", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false) \
    PROPERTY(tscale, Tscale, const QString &, "tscale", STRING, propertyString, propertyIdentity, propertyIdentity, true) \
    PROPERTY(userShaders, UserShaders, const QString &, "glsl-shaders", NODE, shaderPathList, shaderPathsToNative, propertyIdentity, false) \
    PROPERTY(ytdl, Ytdl, bool, "ytdl", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false) \
    PROPERTY(audioDelay, AudioDelay, double, "audio-delay", DOUBLE, propertyNumber, propertyIdentity, propertyIdentity, false) \
    PROPERTY(subDelay, SubDelay, double, "sub-delay", DOUBLE, propertyNumber, propertyIdentity, propertyIdentity, false) \
    PROPERTY(paused, Paused, bool, "pause", FLAG, propertyFlag, propertyYesNo, propertyIdentity, false)

namespace {
const auto aspectName = QStringLiteral("keepaspect");
const auto scanName = QStringLiteral("panscan");
}

void MpvPlayer::setupPropertyObservers() {
    auto registerOwned = [this](const QString &name, Private::Property state, bool seed) {
        if (seed && coreAvailable()) {
            state.desired = state.observed;
            state.revision = 1;
            state.queued = true;
        }
        m_private->propertyOrder.append(name);
        m_private->properties.insert(name, state);
        // libmpv's option-change callback notifies the canonical option name,
        // even when an older spelling is still accepted for writes/reads.
        const QString observed = m_private->observedAliases.key(name, name);
        if (state.format != MPV_FORMAT_NONE && m_ctrl && coreAvailable()) m_ctrl->observeProperty(observed, state.format);
    };
    // fillMode is an intent for two properties, not a projection of arbitrary
    // raw keepaspect/panscan combinations.
    registerOwned(aspectName, {MPV_FORMAT_NONE, propertyIdentity, propertyYesNo, propertyIdentity,
                               {}, {}, true}, true);
    registerOwned(scanName, {MPV_FORMAT_NONE, propertyIdentity, propertyIdentity, propertyIdentity,
                             {}, {}, 0.0}, true);
    m_private->observedAliases.insert(QStringLiteral("screenshot-dir"), QStringLiteral("screenshot-directory"));
#define REGISTER(Member, Setter, Type, Native, Format, Decode, Encode, Normalize, Seed) \
    registerOwned(QStringLiteral(Native), {MPV_FORMAT_##Format, Decode, Encode, Normalize, \
        [this]() { return QVariant(m_##Member); }, [this](const QVariant &value) { \
            using Value = decltype(m_##Member); \
            const auto incoming = value.value<Value>(); \
            if (propertyEqual(m_##Member, incoming)) return; \
            m_##Member = incoming; \
            emit Member##Changed(); \
        }, QVariant(m_##Member)}, Seed);
    OWNED_PROPERTIES(REGISTER)
#undef REGISTER
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
    OBSERVE_DOUBLE("time-pos", m_position, positionChanged);
    OBSERVE_DOUBLE("duration", m_duration, durationChanged);
    OBSERVE("seeking", MPV_FORMAT_FLAG, m_seeking, toBool, seekingChanged);
    OBSERVE("paused-for-cache", MPV_FORMAT_FLAG, m_isBuffering, toBool, isBufferingChanged);
    add(QStringLiteral("eof-reached"), MPV_FORMAT_FLAG, [this](const QVariant &value) { if (value.toBool()) emit playbackEnded(); });
    OBSERVE("audio-device-list", MPV_FORMAT_NODE, m_audioDeviceList, toList, audioDeviceListChanged);
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

void MpvPlayer::observeOwnedProperty(const QString &name, const QVariant &value) {
    if (!value.isValid()) return; // unavailable is not false, zero, or an explicit clear
    auto &state = m_private->properties[name];
    const QVariant observed = state.observed = state.fromNative(value);
    if (state.desired.isValid() || !state.publish) return;
    const auto publish = state.publish;
    publish(observed);
}

void MpvPlayer::setOwnedProperty(const QString &name, const QVariant &value) {
    auto &state = m_private->properties[name];
    const QVariant desired = state.normalize(value);
    if (state.current() == desired) return;
    const auto revision = ++state.revision;
    state.desired = desired;
    state.queued = coreAvailable();
    const auto publish = state.publish;
    QPointer<MpvPlayer> alive(this);
    publish(desired);
    if (alive) flushProperty(name, revision);
}

void MpvPlayer::flushProperty(const QString &name, quint64 revision) {
    auto &state = m_private->properties[name];
    if (state.revision != revision) return;
    if (!coreAvailable()) { failOperation(name, MPV_ERROR_UNINITIALIZED); return; }
    if (!m_rendererInitialized || m_renderError < 0 || !state.queued || state.writing) return;
    state.queued = false;
    state.writing = revision;
    const int id = m_ctrl->setMpvPropertyAsync(name, state.toNative(state.desired));
    if (id > 0) {
        m_requests.insert(id, {1, false, name, revision});
        return;
    }
    state.writing = 0;
    QPointer<MpvPlayer> alive(this);
    reconcileProperty(name, revision, {});
    if (alive && !m_shuttingDown) failOperation(name, id);
}

void MpvPlayer::acknowledgeProperty(const QString &name, quint64 revision, int error) {
    auto &state = m_private->properties[name];
    if (state.writing != revision) return;
    state.writing = 0;
    if (state.revision != revision) {
        flushProperty(name, state.revision);
        return;
    }
    if (!state.publish) {
        if (error >= 0) state.observed = state.desired;
        reconcileProperty(name, revision, {});
        return;
    }
    // libmpv coalesces observations and does not guarantee notification for
    // a write (including rejected writes). Read after the matching reply.
    const int id = coreAvailable() ? m_ctrl->readMpvPropertyAsync(name) : MPV_ERROR_UNINITIALIZED;
    if (id > 0) m_requests.insert(id, {2, false, name, revision});
    else reconcileProperty(name, revision, {});
}

void MpvPlayer::reconcileProperty(const QString &name, quint64 revision, const QVariant &value) {
    auto &state = m_private->properties[name];
    if (state.revision != revision) return;
    state.desired.clear();
    if (value.isValid()) state.observed = state.fromNative(value);
    const QVariant observed = state.observed;
    const auto publish = state.publish;
    if (publish) publish(observed);
}

void MpvPlayer::setSource(const QString &val) {
    if (m_source == val) return;
    m_source = val;
    supersedeMedia();
    const quint64 revision = m_mediaRevision;
    if (auto core = m_private->bridge->core) core->advanceSourceIntent();
    QPointer<MpvPlayer> alive(this);
    emit sourceChanged();
    if (alive && revision == m_mediaRevision && !m_shuttingDown) load(val);
}
#define SETTER(Member, Setter, Type, Native, ...) \
    void MpvPlayer::set##Setter(Type value) { setOwnedProperty(QStringLiteral(Native), value); }
OWNED_PROPERTIES(SETTER)
#undef SETTER
#undef OWNED_PROPERTIES

void MpvPlayer::setFillMode(const QString &val) {
    if (m_fillMode == val) return;
    m_fillMode = val;
    auto defer = [this](const QString &name, const QVariant &desired) {
        auto &state = m_private->properties[name];
        state.desired = desired;
        state.queued = coreAvailable();
        return ++state.revision;
    };
    const quint64 aspect = defer(aspectName, val != QStringLiteral("stretch"));
    const quint64 scan = defer(scanName, val == QStringLiteral("preserveAspectCrop") ? 1.0 : 0.0);
    QPointer<MpvPlayer> alive(this);
    emit fillModeChanged();
    if (!alive) return;
    flushProperty(aspectName, aspect);
    if (alive) flushProperty(scanName, scan);
}

void MpvPlayer::syncProperties() {
    if (!coreAvailable() || m_shuttingDown || m_renderError < 0) return;
    const QStringList order = m_private->propertyOrder;
    QPointer<MpvPlayer> alive(this);
    for (const QString &name : order) {
        flushProperty(name, m_private->properties[name].revision);
        if (!alive || m_shuttingDown || !coreAvailable()) return;
    }
    if (m_private->directoryActive && !m_private->directoryWatcher)
        dispatchDirectory(m_private->directoryTicket);
    else
        drainMedia();
}

void MpvPlayer::submitProperty(const QString &name, const QVariant &value) {
    const int id = coreAvailable() && m_ctrl ? m_ctrl->setMpvPropertyAsync(name, value) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(name, id);
    else m_requests.insert(id, {1, false, name});
}
void MpvPlayer::submitCommand(const QStringList &params, bool orderedLoad) {
    const QString operation = params.isEmpty() ? QStringLiteral("command") : params[0];
    const auto effect = orderedLoad ? MediaEffect::None : commandMediaEffect(params);
    if (effect == MediaEffect::Replace || effect == MediaEffect::ClearPendingLoads)
        supersedeMedia(effect == MediaEffect::ClearPendingLoads);
    QPointer<MpvPlayer> alive(this);
    const int id = coreAvailable() && m_ctrl ? m_ctrl->commandAsync(params) : MPV_ERROR_UNINITIALIZED;
    if (id < 0) failOperation(operation, id);
    else m_requests.insert(id, {0, false, operation});
    if (alive && effect == MediaEffect::ClearPendingLoads) drainMedia();
}
int MpvPlayer::commandAsync(const QStringList &params) {
    const auto effect = commandMediaEffect(params);
    if (effect == MediaEffect::Replace || effect == MediaEffect::ClearPendingLoads)
        supersedeMedia(effect == MediaEffect::ClearPendingLoads);
    const int id = coreAvailable() && m_ctrl ? m_ctrl->commandAsync(params) : MPV_ERROR_UNINITIALIZED;
    if (id > 0) m_requests.insert(id, {0, true, QStringLiteral("commandAsync")});
    if (effect == MediaEffect::ClearPendingLoads) drainMedia();
    return id;
}
int MpvPlayer::setMpvPropertyAsync(const QString &name, const QVariant &value) {
    if (propertyMediaEffect(name) == MediaEffect::Replace) supersedeMedia();
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
    if (owner.propertyRevision) {
        if (kind == 2) {
            reconcileProperty(owner.operation, owner.propertyRevision, error >= 0 ? ownedValue : QVariant());
            return;
        }
        acknowledgeProperty(owner.operation, owner.propertyRevision, error);
    }
    if (owner.caller) {
        if (kind == 0) emit asyncCommandFinished(id, error, ownedValue);
        else if (kind == 1) emit mpvPropertySetFinished(id, error);
        else if (kind == 2) emit mpvPropertyReadFinished(id, error, ownedValue);
    }
    if (alive && error < 0) failOperation(owner.operation, error);
}

void MpvPlayer::enqueueMedia(const QString &operation, const QString &stateKey, std::function<void()> action) {
    if (!coreAvailable()) { failOperation(operation, MPV_ERROR_UNINITIALIZED); return; }
    // Latest state follows intervening commands; stateless loads stay in order.
    if (!stateKey.isEmpty()) {
        m_deferredInitActions.removeIf([&](const auto &old) { return old.stateKey == stateKey; });
    }
    m_deferredInitActions.append({stateKey, std::move(action)});
    drainMedia();
}

void MpvPlayer::drainMedia() {
    if (m_private->drainingMedia) return;
    m_private->drainingMedia = true;
    QPointer<MpvPlayer> alive(this);
    while (coreAvailable() && m_rendererInitialized && m_renderError >= 0
           && !m_deferredInitActions.isEmpty() && !m_private->directoryActive) {
        const auto action = m_deferredInitActions.takeFirst();
        action.retry();
        if (!alive) return;
    }
    m_private->drainingMedia = false;
}

void MpvPlayer::supersedeMedia(bool loadsOnly) {
    ++m_mediaRevision;
    if (loadsOnly) {
        m_deferredInitActions.removeIf([](const auto &action) { return action.stateKey.isEmpty(); });
    } else {
        m_deferredInitActions.clear();
    }
    ++m_private->directoryTicket;
    m_private->directoryActive = false;
    m_private->directoryFiles.clear();
    if (auto watcher = std::exchange(m_private->directoryWatcher, nullptr)) {
        watcher->cancel();
        watcher->deleteLater();
    }
}

void MpvPlayer::load(const QString &url, const QString &mode) {
    if (url.isEmpty()) return;
    enqueueMedia(QStringLiteral("load"), {}, [this, url, mode]() {
        submitCommand(QStringList{"loadfile", cleanPath(url), mode}, true);
    });
}
void MpvPlayer::loadDirectory(const QString &path, const QStringList &fileFilters, const QString &mode, const QString &sort) {
    if (path.isEmpty()) return;
    enqueueMedia(QStringLiteral("loadDirectory"), {}, [this, path, fileFilters, mode, sort]() {
        scanDirectory(cleanPath(path), fileFilters, mode, sort);
    });
}

static DirectoryScan directoryEntries(const QString &path, QStringList filters, const QString &sort) {
    if (!QFileInfo(path).isDir()) return {DirectoryScan::MissingDirectory, {}};
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
    const QDir dir(path);
    const auto files = dir.entryList(expanded, QDir::Files, flags);
    QStringList result;
    result.reserve(files.size());
    for (const auto &file : files) result.append(dir.absoluteFilePath(file));
    return {result.isEmpty() ? DirectoryScan::NoMatches : DirectoryScan::Found, std::move(result)};
}

void MpvPlayer::scanDirectory(const QString &path, const QStringList &filters, const QString &mode, const QString &sort) {
    auto *watcher = new QFutureWatcher<DirectoryScan>(this);
    const quint64 ticket = ++m_private->directoryTicket;
    m_private->directoryWatcher = watcher;
    m_private->directoryActive = true;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, ticket, mode]() {
        if (ticket != m_private->directoryTicket) return;
        m_private->directoryWatcher.clear();
        auto result = watcher->result();
        watcher->deleteLater();
        if (result.status != DirectoryScan::Found) {
            m_private->directoryActive = false; // missing/empty is a legacy no-op
            drainMedia();
            return;
        }
        m_private->directoryFiles = std::move(result.files);
        m_private->directoryIndex = 0;
        m_private->directoryFirstMode = mode == "replace" ? QStringLiteral("replace") : QStringLiteral("append");
        dispatchDirectory(ticket);
    });
    QPromise<DirectoryScan> promise;
    promise.start();
    watcher->setFuture(promise.future());
    QThreadPool::globalInstance()->start([path, filters, sort, promise = std::move(promise)]() mutable {
        if (!promise.isCanceled()) promise.addResult(directoryEntries(path, filters, sort));
        promise.finish();
    });
}

void MpvPlayer::dispatchDirectory(quint64 ticket) {
    if (!coreAvailable() || ticket != m_private->directoryTicket || !m_private->directoryActive) return;
    if (!m_rendererInitialized || m_renderError < 0) return; // next ready resumes the same batch
    QPointer<MpvPlayer> alive(this);
    constexpr int batchSize = 16;
    for (int i = 0; i < batchSize && m_private->directoryIndex < m_private->directoryFiles.size(); ++i) {
        const auto index = m_private->directoryIndex++;
        const auto file = m_private->directoryFiles[index];
        const auto mode = index == 0 ? m_private->directoryFirstMode : QStringLiteral("append");
        submitCommand({"loadfile", file, mode}, true);
        if (!alive || ticket != m_private->directoryTicket || !m_rendererInitialized || m_renderError < 0) return;
    }
    if (m_private->directoryIndex < m_private->directoryFiles.size()) {
        QTimer::singleShot(0, this, [this, ticket]() { dispatchDirectory(ticket); });
        return;
    }
    m_private->directoryFiles.clear();
    m_private->directoryActive = false;
    drainMedia();
}
void MpvPlayer::stop(bool keepPlaylist) {
    const QStringList params = keepPlaylist ? QStringList{"stop", "keep-playlist"} : QStringList{"stop"};
    submitCommand(params);
}
void MpvPlayer::clearPlaylist() { submitCommand(QStringList{"playlist-clear"}); }
void MpvPlayer::seekRelative(double seconds) { submitCommand(QStringList{"seek", QString::number(seconds), "relative"}); }
void MpvPlayer::seekAbsolute(double timestamp) { submitCommand(QStringList{"seek", QString::number(qMax(0.0, timestamp)), "absolute"}); }
void MpvPlayer::next() { submitCommand(QStringList{"playlist-next", "force"}); }
void MpvPlayer::previous() { submitCommand(QStringList{"playlist-prev", "force"}); }
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
    enqueueMedia(QStringLiteral("selectTrack"), name, [this, name, id]() {
        submitProperty(name, id <= 0 ? QVariant(QStringLiteral("no")) : QVariant(id));
    });
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
    enqueueMedia(QStringLiteral("sendMousePosition"), QStringLiteral("mouse-pos"), [this, x, y]() {
        QPointer<MpvPlayer> alive(this);
        submitProperty(QStringLiteral("mouse-pos/x"), x);
        if (alive && !m_shuttingDown) submitProperty(QStringLiteral("mouse-pos/y"), y);
    });
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
    detach(true);
}

void MpvPlayer::detach(bool notify) {
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_coreAvailable = false;
    m_rendererInitialized = false;
    supersedeMedia();
    disconnect(m_private->graphicsConnection);
    disconnect(m_private->bridge->dispatcher.get(), nullptr, this, nullptr);
    if (m_ctrl) disconnect(m_ctrl, nullptr, this, nullptr);
    // The job is weak and generation-specific. Qt executes it on the window's
    // GL thread, including scene-graph cleanup, even if this item no longer
    // participates in synchronization. Destruction remains a second safe path.
    if (m_private->renderWindow && m_private->renderRelease) {
        m_private->renderWindow->scheduleRenderJob(m_private->renderRelease.release(), QQuickWindow::BeforeSynchronizingStage);
        m_private->renderWindow->update();
    }
    update();
    m_logClient->shutdown();
    m_hookManager->shutdown();
    const auto requests = std::exchange(m_requests, {});
    if (notify) for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
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
    m_private->bridge->core.reset();
    if (!notify) return;
    QPointer<MpvPlayer> alive(this);
    emit coreAvailableChanged();
    if (alive) emit rendererReadyChanged();
}
