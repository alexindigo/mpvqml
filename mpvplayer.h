#pragma once

#include <QQuickFramebufferObject>
#include <QtQml/QQmlEngine>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <functional>
#include <memory>
#include "mpvcontroller_iface.h"

class MpvLogClient;
class MpvHookManager;

struct MpvError {
    Q_GADGET
    QML_NAMED_ELEMENT(mpvError)
    QML_UNCREATABLE("mpvError is an error code type")
public:
    enum Value {
        Success = 0, EventQueueFull = -1, NoMem = -2, Uninitialized = -3,
        InvalidParameter = -4, OptionNotFound = -5, OptionFormat = -6,
        OptionError = -7, PropertyNotFound = -8, PropertyFormat = -9,
        PropertyUnavailable = -10, PropertyError = -11, Command = -12,
        LoadingFailed = -13, AoInitFailed = -14, VoInitFailed = -15,
        NothingToPlay = -16, UnknownFormat = -17, Unsupported = -18,
        NotImplemented = -19, Generic = -20
    };
    Q_ENUM(Value)
};

class MpvPlayer : public QQuickFramebufferObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(double playbackSpeed READ playbackSpeed WRITE setPlaybackSpeed NOTIFY playbackSpeedChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY muteChanged)
    Q_PROPERTY(QString fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(QString screenshotDirectory READ screenshotDirectory WRITE setScreenshotDirectory NOTIFY screenshotDirectoryChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QVariantMap lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoWidthChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoHeightChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY seekingChanged)
    Q_PROPERTY(bool isBuffering READ isBuffering NOTIFY isBufferingChanged)
    Q_PROPERTY(QVariant loopFile READ loopFile WRITE setLoopFile NOTIFY loopFileChanged)
    Q_PROPERTY(QVariant loopPlaylist READ loopPlaylist WRITE setLoopPlaylist NOTIFY loopPlaylistChanged)
    Q_PROPERTY(QVariantList trackList READ trackList NOTIFY trackListChanged)
    Q_PROPERTY(QVariantList playlist READ playlist NOTIFY playlistChanged)
    Q_PROPERTY(QList<double> chapterList READ chapterList NOTIFY chapterListChanged)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QVariantList audioDeviceList READ audioDeviceList NOTIFY audioDeviceListChanged)
    Q_PROPERTY(QString currentAudioDevice READ currentAudioDevice WRITE setCurrentAudioDevice NOTIFY currentAudioDeviceChanged)
    Q_PROPERTY(QString hwdec READ hwdec WRITE setHwdec NOTIFY hwdecChanged)
    Q_PROPERTY(QString videoSync READ videoSync WRITE setVideoSync NOTIFY videoSyncChanged)
    Q_PROPERTY(bool interpolation READ interpolation WRITE setInterpolation NOTIFY interpolationChanged)
    Q_PROPERTY(QString tscale READ tscale WRITE setTscale NOTIFY tscaleChanged)
    Q_PROPERTY(QString userShaders READ userShaders WRITE setUserShaders NOTIFY userShadersChanged)
    Q_PROPERTY(bool ytdl READ ytdl WRITE setYtdl NOTIFY ytdlChanged)
    Q_PROPERTY(double audioDelay READ audioDelay WRITE setAudioDelay NOTIFY audioDelayChanged)
    Q_PROPERTY(double subDelay READ subDelay WRITE setSubDelay NOTIFY subDelayChanged)
    Q_PROPERTY(double videoZoom READ videoZoom WRITE setVideoZoom NOTIFY videoZoomChanged)
    Q_PROPERTY(double videoPanX READ videoPanX WRITE setVideoPanX NOTIFY videoPanXChanged)
    Q_PROPERTY(double videoPanY READ videoPanY WRITE setVideoPanY NOTIFY videoPanYChanged)
    Q_PROPERTY(bool audioPitchCorrection READ audioPitchCorrection WRITE setAudioPitchCorrection NOTIFY audioPitchCorrectionChanged)
    Q_PROPERTY(bool subVisibility READ subVisibility WRITE setSubVisibility NOTIFY subVisibilityChanged)
    Q_PROPERTY(double brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(double contrast READ contrast WRITE setContrast NOTIFY contrastChanged)
    Q_PROPERTY(QString hwdecCurrent READ hwdecCurrent NOTIFY hwdecCurrentChanged)
    Q_PROPERTY(QVariantMap demuxerCacheState READ demuxerCacheState NOTIFY demuxerCacheStateChanged)
    Q_PROPERTY(bool coreAvailable READ coreAvailable NOTIFY coreAvailableChanged)
    Q_PROPERTY(int initializationError READ initializationError NOTIFY initializationErrorChanged)
    Q_PROPERTY(QString initializationErrorString READ initializationErrorString NOTIFY initializationErrorChanged)
    Q_PROPERTY(bool rendererReady READ rendererReady NOTIFY rendererReadyChanged)
    Q_PROPERTY(int renderError READ renderError NOTIFY renderErrorChanged)
    Q_PROPERTY(QString renderErrorString READ renderErrorString NOTIFY renderErrorChanged)
public:
    explicit MpvPlayer(QQuickItem *parent = nullptr, MpvControllerIface *ctrl = nullptr);
    ~MpvPlayer() override;
    Renderer *createRenderer() const override;
    bool coreAvailable() const { return !m_shuttingDown && m_coreAvailable; }
    int initializationError() const { return m_initialization.error; }
    QString initializationErrorString() const { return m_initialization.message; }
    bool rendererReady() const { return m_rendererInitialized && !m_shuttingDown; }
    int renderError() const { return m_renderError; }
    QString renderErrorString() const { return m_renderErrorString; }
    QString source() const { return m_source; }
    void setSource(const QString &value);
    bool paused() const { return m_paused; }
    void setPaused(bool value);
    double playbackSpeed() const { return m_playbackSpeed; }
    void setPlaybackSpeed(double value);
    bool mute() const { return m_mute; }
    void setMute(bool value);
    QString fillMode() const { return m_fillMode; }
    void setFillMode(const QString &value);
    QString screenshotDirectory() const { return m_screenshotDirectory; }
    void setScreenshotDirectory(const QString &value);
    double volume() const { return m_volume; }
    void setVolume(double value);
    int videoWidth() const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }
    bool hasVideo() const { return m_hasVideo; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool seeking() const { return m_seeking; }
    bool isBuffering() const { return m_isBuffering; }
    QVariant loopFile() const { return m_loopFile; }
    void setLoopFile(const QVariant &value);
    QVariant loopPlaylist() const { return m_loopPlaylist; }
    void setLoopPlaylist(const QVariant &value);
    QVariantList trackList() const { return m_trackList; }
    QVariantList playlist() const { return m_playlist; }
    QList<double> chapterList() const { return m_chapterList; }
    QVariantMap metadata() const { return m_metadata; }
    QVariantList audioDeviceList() const { return m_audioDeviceList; }
    QString currentAudioDevice() const { return m_currentAudioDevice; }
    void setCurrentAudioDevice(const QString &value);
    QString hwdec() const { return m_hwdec; }
    void setHwdec(const QString &value);
    QString videoSync() const { return m_videoSync; }
    void setVideoSync(const QString &value);
    bool interpolation() const { return m_interpolation; }
    void setInterpolation(bool value);
    QString tscale() const { return m_tscale; }
    void setTscale(const QString &value);
    QString userShaders() const { return m_userShaders; }
    void setUserShaders(const QString &value);
    bool ytdl() const { return m_ytdl; }
    void setYtdl(bool value);
    double audioDelay() const { return m_audioDelay; }
    void setAudioDelay(double value);
    double subDelay() const { return m_subDelay; }
    void setSubDelay(double value);
    double videoZoom() const { return m_videoZoom; }
    void setVideoZoom(double value);
    double videoPanX() const { return m_videoPanX; }
    void setVideoPanX(double value);
    double videoPanY() const { return m_videoPanY; }
    void setVideoPanY(double value);
    bool audioPitchCorrection() const { return m_audioPitchCorrection; }
    void setAudioPitchCorrection(bool value);
    bool subVisibility() const { return m_subVisibility; }
    void setSubVisibility(bool value);
    double brightness() const { return m_brightness; }
    void setBrightness(double value);
    double contrast() const { return m_contrast; }
    void setContrast(double value);
    QString hwdecCurrent() const { return m_hwdecCurrent; }
    QVariantMap demuxerCacheState() const { return m_demuxerCacheState; }
    QVariantMap lastError() const { return m_lastError; }

    Q_INVOKABLE void load(const QString &url, const QString &mode = QStringLiteral("replace"));
    Q_INVOKABLE void loadDirectory(const QString &path, const QStringList &filters = {}, const QString &mode = QStringLiteral("replace"), const QString &sort = QStringLiteral("name"));
    Q_INVOKABLE void clearPlaylist();
    Q_INVOKABLE void stop(bool keepPlaylist = false);
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekAbsolute(double timestamp);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void frameStep();
    Q_INVOKABLE void frameBackStep();
    Q_INVOKABLE void markSeekPosition();
    Q_INVOKABLE void revertSeek();
    Q_INVOKABLE void addAudioTrack(const QString &url);
    Q_INVOKABLE void addVideoTrack(const QString &url);
    Q_INVOKABLE void addSubtitleTrack(const QString &url);
    Q_INVOKABLE void selectTrack(const QString &type, int id);
    Q_INVOKABLE void setVideoFilter(const QString &op, const QString &value);
    Q_INVOKABLE void setAudioFilter(const QString &op, const QString &value);
    Q_INVOKABLE void adjustProperty(const QString &name, double value);
    Q_INVOKABLE void cycleProperty(const QString &name, const QString &direction = QStringLiteral("up"));
    Q_INVOKABLE void multiplyProperty(const QString &name, double factor);
    Q_INVOKABLE void deleteProperty(const QString &name);
    Q_INVOKABLE void setLogLevel(const QString &level);
    Q_INVOKABLE void loadConfigFile(const QString &path);
    Q_INVOKABLE int commandAsync(const QStringList &params);
    Q_INVOKABLE int setMpvPropertyAsync(const QString &name, const QVariant &value);
    Q_INVOKABLE int readMpvPropertyAsync(const QString &name);
    Q_INVOKABLE void setRenderParameter(const QString &name, const QVariant &value);
    Q_INVOKABLE void hookAdd(const QString &name, int priority = 0);
    Q_INVOKABLE void hookContinue(quint64 id);
    Q_INVOKABLE void toggleAbLoop();
    Q_INVOKABLE void saveWatchLaterState();
    Q_INVOKABLE void dropPlaybackBuffers();
    Q_INVOKABLE void screenshot(const QString &mode = QStringLiteral("video"));
    Q_INVOKABLE void runExternalProcess(const QStringList &arguments);
    Q_INVOKABLE void sendScriptMessage(const QStringList &arguments);
    Q_INVOKABLE void triggerScriptBinding(const QString &name);
    Q_INVOKABLE void sendMousePosition(int x, int y);
    Q_INVOKABLE void sendMouseClick(int button, bool isPress);
    Q_INVOKABLE void shutdown();
signals:
    void ready();
    void coreAvailableChanged();
    void initializationErrorChanged();
    void initializationFailed(int code, const QString &message);
    void rendererReadyChanged();
    void renderErrorChanged();
    void renderingFailed(int code, const QString &message);
    void operationFailed(const QString &operation, int code, const QString &message);
    void sourceChanged();
    void pausedChanged();
    void playbackSpeedChanged();
    void muteChanged();
    void fillModeChanged();
    void screenshotDirectoryChanged();
    void volumeChanged();
    void videoWidthChanged();
    void videoHeightChanged();
    void hasVideoChanged();
    void positionChanged();
    void durationChanged();
    void seekingChanged();
    void isBufferingChanged();
    void loopFileChanged();
    void loopPlaylistChanged();
    void trackListChanged();
    void playlistChanged();
    void chapterListChanged();
    void metadataChanged();
    void audioDeviceListChanged();
    void currentAudioDeviceChanged();
    void hwdecChanged();
    void videoSyncChanged();
    void interpolationChanged();
    void tscaleChanged();
    void userShadersChanged();
    void ytdlChanged();
    void audioDelayChanged();
    void subDelayChanged();
    void playbackEnded();
    void errorOccurred(int code, const QString &message);
    void logMessage(const QString &facility, const QString &level, const QString &text);
    void fileStarted();
    void fileLoaded();
    void endFile(const QString &reason);
    void videoReconfig();
    void asyncCommandFinished(int id, int error, const QVariant &result);
    void mpvPropertySetFinished(int id, int error);
    void mpvPropertyReadFinished(int id, int error, const QVariant &value);
    void hookTriggered(const QString &name, quint64 id);
    void videoZoomChanged();
    void videoPanXChanged();
    void videoPanYChanged();
    void audioPitchCorrectionChanged();
    void subVisibilityChanged();
    void brightnessChanged();
    void contrastChanged();
    void hwdecCurrentChanged();
    void demuxerCacheStateChanged();
    void lastErrorChanged();
private:
    struct Private;
    std::unique_ptr<Private> m_private;
    void setupPropertyObservers();
    void syncProperties();
    void emitError(int code, const QString &message);
    void failOperation(const QString &operation, int error);
    quint64 propertyIntent(const QString &name, const QVariant &value);
    void flushProperty(const QString &name, quint64 revision);
    void submitProperty(const QString &name, const QVariant &value);
    void submitCommand(const QStringList &params);
    void rendererStatus(quint64 generation, bool ready, int error, const QString &message);
    void observeWindow(QQuickWindow *window);
    void complete(int id, int error, const QVariant &value, int kind);
    QString m_source;
    bool m_paused = false;
    double m_playbackSpeed = 1.0;
    bool m_mute = false;
    QString m_fillMode = QStringLiteral("preserveAspectFit");
    QString m_screenshotDirectory;
    double m_volume = 100.0;
    int m_videoWidth = 0, m_videoHeight = 0;
    bool m_hasVideo = false;
    double m_position = 0.0, m_duration = 0.0;
    bool m_seeking = false, m_isBuffering = false;
    QVariant m_loopFile = QStringLiteral("no"), m_loopPlaylist = QStringLiteral("no");
    QVariantList m_trackList, m_playlist, m_audioDeviceList;
    QList<double> m_chapterList;
    QVariantMap m_metadata;
    QString m_currentAudioDevice = QStringLiteral("auto");
    QString m_hwdec = QStringLiteral("auto"), m_videoSync = QStringLiteral("audio");
    bool m_interpolation = false;
    QString m_tscale = QStringLiteral("mitchell"), m_userShaders;
    bool m_ytdl = true;
    double m_audioDelay = 0.0, m_subDelay = 0.0;
    double m_videoZoom = 0.0, m_videoPanX = 0.0, m_videoPanY = 0.0;
    bool m_audioPitchCorrection = true, m_subVisibility = true;
    double m_brightness = 0.0, m_contrast = 0.0;
    QString m_hwdecCurrent = QStringLiteral("no");
    QVariantMap m_demuxerCacheState;
    MpvControllerIface *m_ctrl = nullptr;
    MpvLogClient *m_logClient = nullptr;
    MpvHookManager *m_hookManager = nullptr;
    QHash<QString, std::function<void(const QVariant &)>> m_propertyHandlers;
    QVariantMap m_pendingRenderParams;
    QVariantMap m_lastError{{QStringLiteral("code"), 0}, {QStringLiteral("message"), QString()}};
    double m_pendingTrackSeek = -1.0;
    bool m_rendererInitialized = false;
    bool m_coreAvailable = false;
    bool m_shuttingDown = false;
    MpvInitializationStatus m_initialization;
    int m_renderError = 0;
    QString m_renderErrorString;
    quint64 m_mediaRevision = 0;
    quint64 m_errorRevision = 0;
    QVector<std::function<void()>> m_deferredInitActions;
    QSet<QString> m_dirtyProperties;
    QVariantMap m_pendingProperties;
    QHash<QString, quint64> m_propertyRevisions;
    struct RequestOwner { int kind; bool caller; QString operation; };
    QHash<int, RequestOwner> m_requests;
};
