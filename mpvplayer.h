#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QQuickItem>
#include <QtQml/QQmlEngine>
#include <MpvQt/MpvAbstractItem>
#include <functional>
#include "mpvcontroller_iface.h"

class MpvLogClient;
class MpvHookManager;



struct MpvError {
    Q_GADGET
    QML_NAMED_ELEMENT(mpvError)
    QML_UNCREATABLE("mpvError is an error code type")
public:
    enum Value {
        Success = 0x0,
        EventQueueFull = -1,
        NoMem = -2,
        Uninitialized = -3,
        InvalidParameter = -4,
        OptionNotFound = -5,
        OptionFormat = -6,
        OptionError = -7,
        PropertyNotFound = -8,
        PropertyFormat = -9,
        PropertyUnavailable = -10,
        PropertyError = -11,
        Command = -12,
        LoadingFailed = -13,
        AoInitFailed = -14,
        VoInitFailed = -15,
        NothingToPlay = -16,
        UnknownFormat = -17,
        Unsupported = -18,
        NotImplemented = -19,
        Generic = -20
    };
    Q_ENUM(Value)
};

// -- Main component framework

class MpvPlayer : public MpvAbstractItem {
    Q_OBJECT
    QML_ELEMENT

    // Core properties
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(double playbackSpeed READ playbackSpeed WRITE setPlaybackSpeed NOTIFY playbackSpeedChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY muteChanged)
    Q_PROPERTY(QString fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(QString screenshotDirectory READ screenshotDirectory WRITE setScreenshotDirectory NOTIFY screenshotDirectoryChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QVariantMap lastError READ lastError NOTIFY lastErrorChanged)

    // Read-only introspection properties
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoWidthChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoHeightChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY seekingChanged)
    Q_PROPERTY(bool isBuffering READ isBuffering NOTIFY isBufferingChanged)

    // Architecture properties
    Q_PROPERTY(QVariant loopFile READ loopFile WRITE setLoopFile NOTIFY loopFileChanged)
    Q_PROPERTY(QVariant loopPlaylist READ loopPlaylist WRITE setLoopPlaylist NOTIFY loopPlaylistChanged)
    Q_PROPERTY(QVariantList trackList READ trackList NOTIFY trackListChanged)
    Q_PROPERTY(QVariantList playlist READ playlist NOTIFY playlistChanged)
    Q_PROPERTY(QList<double> chapterList READ chapterList NOTIFY chapterListChanged)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QVariantList audioDeviceList READ audioDeviceList NOTIFY audioDeviceListChanged)
    Q_PROPERTY(QString currentAudioDevice READ currentAudioDevice WRITE setCurrentAudioDevice NOTIFY currentAudioDeviceChanged)

    // Video and audio processing properties
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

public:
    explicit MpvPlayer(QQuickItem *parent = nullptr,
                      MpvControllerIface *ctrl = nullptr);
    ~MpvPlayer() override = default;

    // Accessors
    QString source() const;
    void setSource(const QString &src);

    bool paused() const;
    void setPaused(bool val);

    double playbackSpeed() const;
    void setPlaybackSpeed(double val);

    bool mute() const;
    void setMute(bool val);

    QString fillMode() const;
    void setFillMode(const QString &val);

    QString screenshotDirectory() const;
    void setScreenshotDirectory(const QString &val);

    double volume() const;
    void setVolume(double val);

    int videoWidth() const;
    int videoHeight() const;
    bool hasVideo() const;

    double position() const;
    double duration() const;
    bool seeking() const;
    bool isBuffering() const;

    QVariant loopFile() const { return m_loopFile; }
    void setLoopFile(const QVariant &val);

    QVariant loopPlaylist() const { return m_loopPlaylist; }
    void setLoopPlaylist(const QVariant &val);

    double videoZoom() const { return m_videoZoom; }
    double videoPanX() const { return m_videoPanX; }
    double videoPanY() const { return m_videoPanY; }
    bool audioPitchCorrection() const { return m_audioPitchCorrection; }
    bool subVisibility() const { return m_subVisibility; }
    double brightness() const { return m_brightness; }
    double contrast() const { return m_contrast; }
    QString hwdecCurrent() const { return m_hwdecCurrent; }
    QVariantMap demuxerCacheState() const { return m_demuxerCacheState; }
    QVariantMap lastError() const { return m_lastError; }

    void setVideoZoom(double val);
    void setVideoPanX(double val);
    void setVideoPanY(double val);
    void setAudioPitchCorrection(bool val);
    void setSubVisibility(bool val);
    void setBrightness(double val);
    void setContrast(double val);

    QVariantList trackList() const;
    QVariantList playlist() const;
    QList<double> chapterList() const;
    QVariantMap metadata() const;
    QVariantList audioDeviceList() const;

    QString currentAudioDevice() const;
    void setCurrentAudioDevice(const QString &val);

    QString hwdec() const;
    void setHwdec(const QString &val);

    QString videoSync() const;
    void setVideoSync(const QString &val);

    bool interpolation() const;
    void setInterpolation(bool val);

    QString tscale() const;
    void setTscale(const QString &val);

    QString userShaders() const;
    void setUserShaders(const QString &val);

    bool ytdl() const;
    void setYtdl(bool val);

    double audioDelay() const;
    void setAudioDelay(double val);

    double subDelay() const;
    void setSubDelay(double val);

    // Content loading
    Q_INVOKABLE void load(const QString &url, const QString &mode = QStringLiteral("replace"));
    Q_INVOKABLE void loadDirectory(const QString &path,
                                  const QStringList &fileFilters = {},
                                  const QString &mode = QStringLiteral("replace"),
                                  const QString &sortOrder = QStringLiteral("name"));
    Q_INVOKABLE void clearPlaylist();
    Q_INVOKABLE void stop(bool keepPlaylist = false);

    // Navigation
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekAbsolute(double timestamp);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void frameStep();
    Q_INVOKABLE void frameBackStep();

    Q_INVOKABLE void markSeekPosition();
    Q_INVOKABLE void revertSeek();

    // Track composition
    Q_INVOKABLE void addAudioTrack(const QString &url);
    Q_INVOKABLE void addVideoTrack(const QString &url);
    Q_INVOKABLE void addSubtitleTrack(const QString &url);
    Q_INVOKABLE void selectTrack(const QString &type, int id);

    // Filtering
    Q_INVOKABLE void setVideoFilter(const QString &op, const QString &val);
    Q_INVOKABLE void setAudioFilter(const QString &op, const QString &val);

    // Parameter modification
    Q_INVOKABLE void adjustProperty(const QString &name, double value);
    Q_INVOKABLE void cycleProperty(const QString &name, const QString &direction = QStringLiteral("up"));
    Q_INVOKABLE void multiplyProperty(const QString &name, double factor);
    Q_INVOKABLE void deleteProperty(const QString &name);

    // Utilities
    Q_INVOKABLE void setLogLevel(const QString &level);
    Q_INVOKABLE void loadConfigFile(const QString &path);
    Q_INVOKABLE void setProperty(const QString &name, const QString &value);
    Q_INVOKABLE int commandAsync(const QStringList &params, int id = 0);
    Q_INVOKABLE void setRenderParameter(const QString &name, const QVariant &value);
    Q_INVOKABLE void hookAdd(const QString &name, int priority = 0);
    Q_INVOKABLE void hookContinue(quint64 id);
    Q_INVOKABLE void toggleAbLoop();
    Q_INVOKABLE void saveWatchLaterState();
    Q_INVOKABLE void dropPlaybackBuffers();
    Q_INVOKABLE void screenshot(const QString &mode = QStringLiteral("video"));

    // External process and scripting
    Q_INVOKABLE void runExternalProcess(const QStringList &arguments);
    Q_INVOKABLE void sendScriptMessage(const QStringList &arguments);
    Q_INVOKABLE void triggerScriptBinding(const QString &name);

    // Mouse interaction
    Q_INVOKABLE void sendMousePosition(int x, int y);
    Q_INVOKABLE void sendMouseClick(int button, bool isPress);

signals:
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

    // Diagnostic signals
    void playbackEnded();
    void errorOccurred(int code, const QString &message);
    void logMessage(const QString &facility, const QString &level, const QString &text);

    // Lifecycle signals (forwarded from MpvController)
    void fileStarted();
    void fileLoaded();
    void endFile(const QString &reason);
    void videoReconfig();
    void asyncCommandFinished(int id, int error);
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
    void setupPropertyObservers();
    void syncProperties();
    void emitError(int code, const QString &message);


    QString m_source;
    bool m_paused = false;
    double m_playbackSpeed = 1.0;
    bool m_mute = false;
    QString m_fillMode = QStringLiteral("preserveAspectFit");
    QString m_screenshotDirectory;
    double m_volume = 100.0;

    int m_videoWidth = 0;
    int m_videoHeight = 0;
    bool m_hasVideo = false;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_seeking = false;
    bool m_isBuffering = false;

    QVariant m_loopFile = QStringLiteral("no");
    QVariant m_loopPlaylist = QStringLiteral("no");

    QVariantList m_trackList;
    QVariantList m_playlist;
    QList<double> m_chapterList;
    QVariantMap m_metadata;
    QVariantList m_audioDeviceList;
    QString m_currentAudioDevice = QStringLiteral("auto");

    QString m_hwdec = QStringLiteral("auto");
    QString m_videoSync = QStringLiteral("audio");
    bool m_interpolation = false;
    QString m_tscale = QStringLiteral("mitchell");
    QString m_userShaders;
    bool m_ytdl = true;
    double m_audioDelay = 0.0;
    double m_subDelay = 0.0;

    double m_videoZoom = 0.0;
    double m_videoPanX = 0.0;
    double m_videoPanY = 0.0;
    bool m_audioPitchCorrection = true;
    bool m_subVisibility = true;
    double m_brightness = 0.0;
    double m_contrast = 0.0;
    QString m_hwdecCurrent = QStringLiteral("no");
    QVariantMap m_demuxerCacheState;

    MpvControllerIface *m_ctrl = nullptr;
    MpvLogClient *m_logClient = nullptr;
    MpvHookManager *m_hookManager = nullptr;
    QHash<QString, std::function<void(const QVariant&)>> m_propertyHandlers;
    QList<QPair<QString, QVariant>> m_pendingRenderParams;
    QVariantMap m_lastError{{QStringLiteral("code"), 0},
                            {QStringLiteral("message"), QString()}};

    double m_pendingTrackSeek = -1.0;
    bool m_rendererInitialized = false;
    QVector<std::function<void()>> m_deferredInitActions;
    QSet<QString> m_dirtyProperties;
    QVariantMap m_pendingProperties;
};
