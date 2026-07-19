#include "mpvplayer.h"
#include "mpvcontroller_adapter.h"
#include "mpvlogclient.h"
#include "mpvhookmanager.h"
#include <QDir>
#include <QFileInfo>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>
#include <mpv/client.h>

// Detect drift between our QML-facing MpvError enum and libmpv's numeric
// error codes at compile time. If libmpv ever renumbers, these will fail
// to build instead of silently returning wrong values to QML.
static_assert(static_cast<int>(MpvError::Success) == MPV_ERROR_SUCCESS, "");
static_assert(static_cast<int>(MpvError::EventQueueFull) == MPV_ERROR_EVENT_QUEUE_FULL, "");
static_assert(static_cast<int>(MpvError::NoMem) == MPV_ERROR_NOMEM, "");
static_assert(static_cast<int>(MpvError::Uninitialized) == MPV_ERROR_UNINITIALIZED, "");
static_assert(static_cast<int>(MpvError::InvalidParameter) == MPV_ERROR_INVALID_PARAMETER, "");
static_assert(static_cast<int>(MpvError::OptionNotFound) == MPV_ERROR_OPTION_NOT_FOUND, "");
static_assert(static_cast<int>(MpvError::OptionFormat) == MPV_ERROR_OPTION_FORMAT, "");
static_assert(static_cast<int>(MpvError::OptionError) == MPV_ERROR_OPTION_ERROR, "");
static_assert(static_cast<int>(MpvError::PropertyNotFound) == MPV_ERROR_PROPERTY_NOT_FOUND, "");
static_assert(static_cast<int>(MpvError::PropertyFormat) == MPV_ERROR_PROPERTY_FORMAT, "");
static_assert(static_cast<int>(MpvError::PropertyUnavailable) == MPV_ERROR_PROPERTY_UNAVAILABLE, "");
static_assert(static_cast<int>(MpvError::PropertyError) == MPV_ERROR_PROPERTY_ERROR, "");
static_assert(static_cast<int>(MpvError::Command) == MPV_ERROR_COMMAND, "");
static_assert(static_cast<int>(MpvError::LoadingFailed) == MPV_ERROR_LOADING_FAILED, "");
static_assert(static_cast<int>(MpvError::AoInitFailed) == MPV_ERROR_AO_INIT_FAILED, "");
static_assert(static_cast<int>(MpvError::VoInitFailed) == MPV_ERROR_VO_INIT_FAILED, "");
static_assert(static_cast<int>(MpvError::NothingToPlay) == MPV_ERROR_NOTHING_TO_PLAY, "");
static_assert(static_cast<int>(MpvError::UnknownFormat) == MPV_ERROR_UNKNOWN_FORMAT, "");
static_assert(static_cast<int>(MpvError::Unsupported) == MPV_ERROR_UNSUPPORTED, "");
static_assert(static_cast<int>(MpvError::NotImplemented) == MPV_ERROR_NOT_IMPLEMENTED, "");
static_assert(static_cast<int>(MpvError::Generic) == MPV_ERROR_GENERIC, "");

// Fuzzy equality that also treats two values near zero as equal.
// qFuzzyCompare alone returns false when both operands are ~0.0.
static inline bool nearlyEqual(double a, double b) {
    return qFuzzyCompare(a, b) || (qFuzzyIsNull(a) && qFuzzyIsNull(b));
}

static QVariantList hyphenKeys(QVariantList src) {
    QVariantList out;
    for (const QVariant &v : src) {
        QVariantMap m = v.toMap();
        if (m.isEmpty()) { out.append(v); continue; }
        QVariantMap cm;
        for (auto it = m.begin(); it != m.end(); ++it) {
            QString key = it.key();
            if (key == QLatin1String("id"))
                key = QStringLiteral("trackId");
            for (int idx = key.indexOf(QLatin1Char('-'));
                 idx > 0 && idx + 1 < key.size();
                 idx = key.indexOf(QLatin1Char('-'), idx)) {
                QChar u = key[idx + 1].toUpper();
                key = key.left(idx) + u + key.mid(idx + 2);
            }
            cm.insert(key, it.value());
        }
        out.append(QVariant::fromValue(cm));
    }
    return out;
}

// Setter macros — parameter name is always `val`
#define MPV_SETTER_STRING(Member, SignalSuffix, MpvProp) \
  if (Member != val) { \
    Member = val; \
    m_pendingProperties[QStringLiteral(MpvProp)] = val; \
    emit SignalSuffix##Changed(); \
    m_dirtyProperties.insert(QStringLiteral(MpvProp)); \
    if (m_rendererInitialized) \
      m_ctrl->setProperty(QStringLiteral(MpvProp), val); \
  }

#define MPV_SETTER_BOOL(Member, SignalSuffix, MpvProp) \
  if (Member != val) { \
    Member = val; \
    m_pendingProperties[QStringLiteral(MpvProp)] = val; \
    emit SignalSuffix##Changed(); \
    m_dirtyProperties.insert(QStringLiteral(MpvProp)); \
    if (m_rendererInitialized) \
      m_ctrl->setProperty(QStringLiteral(MpvProp), \
        val ? QStringLiteral("yes") : QStringLiteral("no")); \
  }

#define MPV_SETTER_DOUBLE(Member, SignalSuffix, MpvProp) \
  if (Member != val) { \
    Member = val; \
    m_pendingProperties[QStringLiteral(MpvProp)] = val; \
    emit SignalSuffix##Changed(); \
    m_dirtyProperties.insert(QStringLiteral(MpvProp)); \
    if (m_rendererInitialized) \
      m_ctrl->setProperty(QStringLiteral(MpvProp), val); \
  }

#define MPV_SETTER_CLAMPED(Member, SignalSuffix, MpvProp, Min, Max) \
  { double _v = qBound(Min, val, Max); \
    if (Member != _v) { \
      Member = _v; \
      m_pendingProperties[QStringLiteral(MpvProp)] = _v; \
      emit SignalSuffix##Changed(); \
      m_dirtyProperties.insert(QStringLiteral(MpvProp)); \
      if (m_rendererInitialized) \
        m_ctrl->setProperty(QStringLiteral(MpvProp), _v); \
    } \
  }

MpvPlayer::MpvPlayer(QQuickItem *parent, MpvControllerIface *ctrl)
    : MpvAbstractItem(parent) {
  if (QQuickWindow::graphicsApi() == QSGRendererInterface::Unknown) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
  }

  if (ctrl) {
    m_ctrl = ctrl;
  } else {
    m_ctrl = new MpvControllerAdapter(mpvController(), this);
  }

  m_logClient = new MpvLogClient(this);
  m_hookManager = new MpvHookManager(this);

  setupPropertyObservers();

  connect(m_ctrl, &MpvControllerIface::propertyChanged, this,
          [this](const QString &name, const QVariant &value) {
            auto it = m_propertyHandlers.find(name);
            if (it != m_propertyHandlers.end()) {
              it.value()(value);
            }
          });

  // Forward lifecycle signals from MpvController
  connect(m_ctrl, &MpvControllerIface::fileStarted, this,
          &MpvPlayer::fileStarted);
  connect(m_ctrl, &MpvControllerIface::fileLoaded, this,
          [this]() {
    emit fileLoaded();
    if (m_pendingTrackSeek >= 0.0) {
      seekAbsolute(m_pendingTrackSeek);
      m_pendingTrackSeek = -1.0;
    }
  });
  connect(m_ctrl, &MpvControllerIface::endFile, this,
          [this](const QString &reason) {
    emit endFile(reason);
    if (reason == QStringLiteral("error"))
      emitError(MPV_ERROR_LOADING_FAILED,
                QStringLiteral("Playback ended with error"));
  });
  connect(m_ctrl, &MpvControllerIface::videoReconfig, this,
          &MpvPlayer::videoReconfig);
  connect(m_ctrl, &MpvControllerIface::asyncReply, this,
          &MpvPlayer::asyncCommandFinished);

  // Mark intentional deviations from mpv defaults. Everything routes through
  // the dirty/pending pipeline; syncProperties() flushes on renderer init.
  m_pendingProperties[QStringLiteral("hwdec")] = QStringLiteral("auto");
  m_dirtyProperties.insert(QStringLiteral("hwdec"));
  m_dirtyProperties.insert(QStringLiteral("tscale"));
  // Seed default fillMode ("preserveAspectFit") into the real mpv props it maps to
  m_pendingProperties[QStringLiteral("keepaspect")] = QStringLiteral("yes");
  m_pendingProperties[QStringLiteral("panscan")] = 0.0;
  m_dirtyProperties.insert(QStringLiteral("keepaspect"));
  m_dirtyProperties.insert(QStringLiteral("panscan"));

  // Wire helper signals
  connect(m_logClient, &MpvLogClient::logMessage, this,
          &MpvPlayer::logMessage);
  connect(m_hookManager, &MpvHookManager::hookTriggered, this,
          &MpvPlayer::hookTriggered);

  // Renderer-ready gate: once the render context is up, flush any
  // properties QML set during construction and run deferred load actions.
  // MpvAbstractItem::ready() is the idiomatic point for this.
  connect(this, &MpvAbstractItem::ready, this, [this]() {
    if (!m_rendererInitialized) {
      m_rendererInitialized = true;
      syncProperties();
    }
  });
}

void MpvPlayer::setupPropertyObservers() {
  struct PropertyBlueprint {
    QString name;
    mpv_format format;
    std::function<void(const QVariant &)> handler;
  };

  const QList<PropertyBlueprint> registry = {
      {QStringLiteral("video-params/w"), MPV_FORMAT_INT64,
       [this](const QVariant &v) {
         int w = v.toInt();
         if (m_videoWidth != w) {
           m_videoWidth = w;
           emit videoWidthChanged();

           bool currentHasVideo = (m_videoWidth > 0);
           if (m_hasVideo != currentHasVideo) {
             m_hasVideo = currentHasVideo;
             emit hasVideoChanged();
           }
         }
       }},

      {QStringLiteral("video-params/h"), MPV_FORMAT_INT64,
       [this](const QVariant &v) {
         int h = v.toInt();
         if (m_videoHeight != h) {
           m_videoHeight = h;
           emit videoHeightChanged();
         }
       }},

      {QStringLiteral("pause"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool p = v.toBool();
         if (m_paused != p) {
           m_paused = p;
           emit pausedChanged();
         }
       }},

      {QStringLiteral("speed"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double s = v.toDouble();
         if (!nearlyEqual(m_playbackSpeed, s)) {
           m_playbackSpeed = s;
           emit playbackSpeedChanged();
         }
       }},

      {QStringLiteral("mute"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool m = v.toBool();
         if (m_mute != m) {
           m_mute = m;
           emit muteChanged();
         }
       }},

      {QStringLiteral("screenshot-directory"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString s = v.toString();
         if (m_screenshotDirectory != s) {
           m_screenshotDirectory = s;
           emit screenshotDirectoryChanged();
         }
       }},

      {QStringLiteral("volume"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double vol = v.toDouble();
         if (!nearlyEqual(m_volume, vol)) {
           m_volume = vol;
           emit volumeChanged();
         }
       }},

      {QStringLiteral("time-pos"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double pos = v.toDouble();
         if (m_position != pos) {
           m_position = pos;
           emit positionChanged();
         }
       }},

      {QStringLiteral("duration"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double dur = v.toDouble();
         if (m_duration != dur) {
           m_duration = dur;
           emit durationChanged();
         }
       }},

      {QStringLiteral("seeking"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool s = v.toBool();
         if (m_seeking != s) {
           m_seeking = s;
           emit seekingChanged();
         }
       }},

      {QStringLiteral("paused-for-cache"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool b = v.toBool();
         if (m_isBuffering != b) {
           m_isBuffering = b;
           emit isBufferingChanged();
         }
       }},

      {QStringLiteral("eof-reached"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         if (v.toBool()) {
           emit playbackEnded();
         }
       }},

      {QStringLiteral("audio-device-list"), MPV_FORMAT_NODE,
       [this](const QVariant &v) {
         m_audioDeviceList = v.toList();
         emit audioDeviceListChanged();
       }},

      {QStringLiteral("audio-device"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString dev = v.toString();
         if (m_currentAudioDevice != dev) {
           m_currentAudioDevice = dev;
           emit currentAudioDeviceChanged();
         }
       }},

      {QStringLiteral("hwdec"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString hw = v.toString();
         if (m_hwdec != hw) {
           m_hwdec = hw;
           emit hwdecChanged();
         }
       }},

      {QStringLiteral("video-sync"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString vs = v.toString();
         if (m_videoSync != vs) {
           m_videoSync = vs;
           emit videoSyncChanged();
         }
       }},

      {QStringLiteral("interpolation"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool interp = v.toBool();
         if (m_interpolation != interp) {
           m_interpolation = interp;
           emit interpolationChanged();
         }
       }},

      {QStringLiteral("tscale"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString ts = v.toString();
         if (m_tscale != ts) {
           m_tscale = ts;
           emit tscaleChanged();
         }
       }},

      {QStringLiteral("glsl-shaders"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString us = v.toString();
         if (m_userShaders != us) {
           m_userShaders = us;
           emit userShadersChanged();
         }
       }},

      {QStringLiteral("ytdl"), MPV_FORMAT_FLAG,
       [this](const QVariant &v) {
         bool y = v.toBool();
         if (m_ytdl != y) {
           m_ytdl = y;
           emit ytdlChanged();
         }
       }},

      {QStringLiteral("audio-delay"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double ad = v.toDouble();
         if (!nearlyEqual(m_audioDelay, ad)) {
           m_audioDelay = ad;
           emit audioDelayChanged();
         }
       }},

      {QStringLiteral("sub-delay"), MPV_FORMAT_DOUBLE,
       [this](const QVariant &v) {
         double sd = v.toDouble();
         if (!nearlyEqual(m_subDelay, sd)) {
           m_subDelay = sd;
           emit subDelayChanged();
         }
       }},

       {QStringLiteral("track-list"), MPV_FORMAT_NODE,
        [this](const QVariant &v) {
          m_trackList = hyphenKeys(v.toList());
          emit trackListChanged();
        }},

      {QStringLiteral("playlist"), MPV_FORMAT_NODE,
       [this](const QVariant &v) {
         m_playlist = hyphenKeys(v.toList());
         emit playlistChanged();
       }},

      {QStringLiteral("chapter-list"), MPV_FORMAT_NODE,
       [this](const QVariant &v) {
         m_chapterList.clear();
         for (const QVariant &item : v.toList()) {
           m_chapterList.append(
               item.toMap().value(QStringLiteral("time")).toDouble());
         }
         emit chapterListChanged();
       }},

      {QStringLiteral("metadata"), MPV_FORMAT_NODE,
       [this](const QVariant &v) {
         QVariantMap incoming = v.toMap();
         if (m_metadata != incoming) {
           m_metadata = incoming;
           emit metadataChanged();
         }
       }},

      {QStringLiteral("hwdec-current"), MPV_FORMAT_STRING,
       [this](const QVariant &v) {
         QString current = v.toString();
         if (current.isEmpty())
           current = QStringLiteral("no");
         if (m_hwdecCurrent != current) {
           m_hwdecCurrent = current;
           emit hwdecCurrentChanged();
         }
       }},

      {QStringLiteral("demuxer-cache-state"), MPV_FORMAT_NODE,
       [this](const QVariant &v) {
         QVariantMap cacheData = v.toMap();
         if (m_demuxerCacheState != cacheData) {
           m_demuxerCacheState = cacheData;
           emit demuxerCacheStateChanged();
         }
       }}};

  for (const auto &prop : registry) {
    m_ctrl->observeProperty(prop.name, prop.format);
    m_propertyHandlers.insert(prop.name, prop.handler);
  }
}

// -- General accessors

QString MpvPlayer::source() const { return m_source; }
void MpvPlayer::setSource(const QString &val) {
  if (m_source != val) {
    m_source = val;
    emit sourceChanged();
    load(val, QStringLiteral("replace"));
  }
}

bool MpvPlayer::paused() const { return m_paused; }
void MpvPlayer::setPaused(bool val) {
  MPV_SETTER_BOOL(m_paused, paused, "pause")
}

double MpvPlayer::playbackSpeed() const { return m_playbackSpeed; }
void MpvPlayer::setPlaybackSpeed(double val) {
  if (val < 0.01)
    val = 0.01;
  MPV_SETTER_DOUBLE(m_playbackSpeed, playbackSpeed, "speed")
}

bool MpvPlayer::mute() const { return m_mute; }
void MpvPlayer::setMute(bool val) {
  MPV_SETTER_BOOL(m_mute, mute, "mute")
}

QString MpvPlayer::fillMode() const { return m_fillMode; }
void MpvPlayer::setFillMode(const QString &val) {
  if (m_fillMode != val) {
    m_fillMode = val;
    // Translate the QML-facing fillMode into the two real mpv properties
    // it actually controls, so pending/dirty flow uniformly.
    QString keepAspect = QStringLiteral("yes");
    double panScan = 0.0;
    if (val == QLatin1String("preserveAspectCrop")) {
      panScan = 1.0;
    } else if (val == QLatin1String("stretch")) {
      keepAspect = QStringLiteral("no");
    }
    m_pendingProperties[QStringLiteral("keepaspect")] = keepAspect;
    m_pendingProperties[QStringLiteral("panscan")] = panScan;
    m_dirtyProperties.insert(QStringLiteral("keepaspect"));
    m_dirtyProperties.insert(QStringLiteral("panscan"));
    emit fillModeChanged();
    if (m_rendererInitialized) {
      m_ctrl->setProperty(QStringLiteral("keepaspect"), keepAspect);
      m_ctrl->setProperty(QStringLiteral("panscan"), panScan);
    }
  }
}

QString MpvPlayer::screenshotDirectory() const { return m_screenshotDirectory; }
void MpvPlayer::setScreenshotDirectory(const QString &val) {
  QString cleanDir =
      val.startsWith(QLatin1String("file://")) ? QUrl(val).toLocalFile() : val;
  if (m_screenshotDirectory != cleanDir) {
    m_screenshotDirectory = cleanDir;
    m_pendingProperties[QStringLiteral("screenshot-directory")] = cleanDir;
    emit screenshotDirectoryChanged();
    m_dirtyProperties.insert(QStringLiteral("screenshot-directory"));
    if (m_rendererInitialized) {
      m_ctrl->setProperty(QStringLiteral("screenshot-directory"),
                                   cleanDir);
    }
  }
}

double MpvPlayer::volume() const { return m_volume; }
void MpvPlayer::setVolume(double val) {
  MPV_SETTER_CLAMPED(m_volume, volume, "volume", 0.0, 150.0)
}

int MpvPlayer::videoWidth() const { return m_videoWidth; }
int MpvPlayer::videoHeight() const { return m_videoHeight; }
bool MpvPlayer::hasVideo() const { return m_hasVideo; }
double MpvPlayer::position() const { return m_position; }
double MpvPlayer::duration() const { return m_duration; }
bool MpvPlayer::seeking() const { return m_seeking; }
bool MpvPlayer::isBuffering() const { return m_isBuffering; }

void MpvPlayer::setLoopFile(const QVariant &val) {
  if (m_loopFile != val) {
    m_loopFile = val;
    m_pendingProperties[QStringLiteral("loop-file")] = val;
    emit loopFileChanged();
    m_dirtyProperties.insert(QStringLiteral("loop-file"));
    if (m_rendererInitialized) {
      m_ctrl->setProperty(QStringLiteral("loop-file"),
                                   val.toString());
    }
  }
}

void MpvPlayer::setLoopPlaylist(const QVariant &val) {
  if (m_loopPlaylist != val) {
    m_loopPlaylist = val;
    m_pendingProperties[QStringLiteral("loop-playlist")] = val;
    emit loopPlaylistChanged();
    m_dirtyProperties.insert(QStringLiteral("loop-playlist"));
    if (m_rendererInitialized) {
      m_ctrl->setProperty(QStringLiteral("loop-playlist"),
                                   val.toString());
    }
  }
}

QVariantList MpvPlayer::trackList() const { return m_trackList; }
QVariantList MpvPlayer::playlist() const { return m_playlist; }
QList<double> MpvPlayer::chapterList() const { return m_chapterList; }
QVariantMap MpvPlayer::metadata() const { return m_metadata; }
QVariantList MpvPlayer::audioDeviceList() const { return m_audioDeviceList; }

QString MpvPlayer::currentAudioDevice() const { return m_currentAudioDevice; }
void MpvPlayer::setCurrentAudioDevice(const QString &val) {
  MPV_SETTER_STRING(m_currentAudioDevice, currentAudioDevice, "audio-device")
}

QString MpvPlayer::hwdec() const { return m_hwdec; }
void MpvPlayer::setHwdec(const QString &val) {
  MPV_SETTER_STRING(m_hwdec, hwdec, "hwdec")
}

QString MpvPlayer::videoSync() const { return m_videoSync; }
void MpvPlayer::setVideoSync(const QString &val) {
  MPV_SETTER_STRING(m_videoSync, videoSync, "video-sync")
}

bool MpvPlayer::interpolation() const { return m_interpolation; }
void MpvPlayer::setInterpolation(bool val) {
  MPV_SETTER_BOOL(m_interpolation, interpolation, "interpolation")
}

QString MpvPlayer::tscale() const { return m_tscale; }
void MpvPlayer::setTscale(const QString &val) {
  MPV_SETTER_STRING(m_tscale, tscale, "tscale")
}

QString MpvPlayer::userShaders() const { return m_userShaders; }
void MpvPlayer::setUserShaders(const QString &val) {
  MPV_SETTER_STRING(m_userShaders, userShaders, "glsl-shaders")
}

bool MpvPlayer::ytdl() const { return m_ytdl; }
void MpvPlayer::setYtdl(bool val) {
  MPV_SETTER_BOOL(m_ytdl, ytdl, "ytdl")
}

double MpvPlayer::audioDelay() const { return m_audioDelay; }
void MpvPlayer::setAudioDelay(double val) {
  MPV_SETTER_DOUBLE(m_audioDelay, audioDelay, "audio-delay")
}

double MpvPlayer::subDelay() const { return m_subDelay; }
void MpvPlayer::setSubDelay(double val) {
  MPV_SETTER_DOUBLE(m_subDelay, subDelay, "sub-delay")
}

// -- Invocable operations

void MpvPlayer::load(const QString &url, const QString &mode) {
  if (url.isEmpty())
    return;
  if (!m_rendererInitialized) {
    m_deferredInitActions.append(
        [this, url, mode]() { load(url, mode); });
    return;
  }
  QString cleanPath =
      url.startsWith(QLatin1String("file://")) ? QUrl(url).toLocalFile() : url;
  m_ctrl->command(
      QStringList{QStringLiteral("loadfile"), cleanPath, mode});
}

void MpvPlayer::loadDirectory(const QString &path,
                              const QStringList &fileFilters,
                              const QString &mode, const QString &sortOrder) {
  if (path.isEmpty()) {
    return;
  }
  if (!m_rendererInitialized) {
    m_deferredInitActions.append(
        [this, path, fileFilters, mode, sortOrder]() {
      loadDirectory(path, fileFilters, mode, sortOrder);
    });
    return;
  }

  QString cleanPath = path.startsWith(QLatin1String("file://"))
                          ? QUrl(path).toLocalFile()
                          : path;

  QFileInfo fileInfo(cleanPath);
  if (!fileInfo.isDir()) {
    return;
  }

  QStringList filters = fileFilters;
  if (filters.isEmpty()) {
    filters << "*.mp4" << "*.mkv" << "*.avi" << "*.webm" << "*.mov" << "*.mp3"
            << "*.flac";
  }

  QStringList expandedFilters;
  for (const QString &f : filters) {
    expandedFilters << f;
    if (f.startsWith(QLatin1String("*."))) {
      QString upper = f.toUpper();
      if (upper != f)
        expandedFilters << upper;
    }
  }

  QDir::Filters itemFilters = QDir::Files;
  QDir::SortFlags sortFlags = QDir::IgnoreCase;
  if (sortOrder == QLatin1String("date") || sortOrder == QLatin1String("time"))
    sortFlags |= QDir::Time;
  else if (sortOrder == QLatin1String("size"))
    sortFlags |= QDir::Size;
  else
    sortFlags |= QDir::Name;

  QDir directory(cleanPath);
  QFileInfoList fileList =
      directory.entryInfoList(expandedFilters, itemFilters, sortFlags);
  if (fileList.isEmpty()) {
    return;
  }
  bool isFirstItem = true;
  for (const QFileInfo &file : fileList) {
    QString action =
        (isFirstItem && mode == QLatin1String("replace"))
            ? QStringLiteral("replace")
            : QStringLiteral("append");
    m_ctrl->command(QStringList{QStringLiteral("loadfile"),
                                         file.absoluteFilePath(), action});
    isFirstItem = false;
  }
}

void MpvPlayer::clearPlaylist() {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("playlist-clear")});
}

void MpvPlayer::stop(bool keepPlaylist) {
  if (!m_rendererInitialized)
    return;
  if (keepPlaylist)
    m_ctrl->command(
        QStringList{QStringLiteral("stop"), QStringLiteral("keep-playlist")});
  else
    m_ctrl->command(QStringList{QStringLiteral("stop")});
}

void MpvPlayer::seekRelative(double seconds) {
  if (m_rendererInitialized) {
    m_ctrl->command(QStringList{QStringLiteral("seek"),
                                         QString::number(seconds),
                                         QStringLiteral("relative")});
  }
}

void MpvPlayer::seekAbsolute(double timestamp) {
  if (m_rendererInitialized) {
    if (timestamp < 0.0)
      timestamp = 0.0;
    m_ctrl->command(QStringList{QStringLiteral("seek"),
                                         QString::number(timestamp),
                                         QStringLiteral("absolute")});
  }
}

void MpvPlayer::next() {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("playlist-next"), QStringLiteral("force")});
}

void MpvPlayer::previous() {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("playlist-prev"), QStringLiteral("force")});
}

void MpvPlayer::markSeekPosition() {
  if (m_rendererInitialized) {
    m_ctrl->command(
        QStringList{QStringLiteral("revert-seek"), QStringLiteral("mark")});
  }
}

void MpvPlayer::revertSeek() {
  if (m_rendererInitialized) {
    m_ctrl->command(QStringList{QStringLiteral("revert-seek")});
  }
}

void MpvPlayer::frameStep() {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("frame-step")});
}

void MpvPlayer::frameBackStep() {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("frame-back-step")});
}

void MpvPlayer::addAudioTrack(const QString &url) {
  if (m_rendererInitialized && !url.isEmpty()) {
    QString cleanPath = url.startsWith(QLatin1String("file://"))
                            ? QUrl(url).toLocalFile()
                            : url;
    m_ctrl->command(
        QStringList{QStringLiteral("audio-add"), cleanPath});
  }
}

void MpvPlayer::addVideoTrack(const QString &url) {
  if (m_rendererInitialized && !url.isEmpty()) {
    QString cleanPath = url.startsWith(QLatin1String("file://"))
                            ? QUrl(url).toLocalFile()
                            : url;
    m_ctrl->command(
        QStringList{QStringLiteral("video-add"), cleanPath});
  }
}

void MpvPlayer::addSubtitleTrack(const QString &url) {
  if (m_rendererInitialized && !url.isEmpty()) {
    QString cleanPath = url.startsWith(QLatin1String("file://"))
                            ? QUrl(url).toLocalFile()
                            : url;
    m_ctrl->command(QStringList{QStringLiteral("sub-add"), cleanPath});
  }
}

void MpvPlayer::selectTrack(const QString &type, int id) {
  if (!m_rendererInitialized)
    return;
  QString targetProperty;
  if (type == QLatin1String("audio"))
    targetProperty = QStringLiteral("aid");
  else if (type == QLatin1String("video"))
    targetProperty = QStringLiteral("vid");
  else if (type == QLatin1String("sub") || type == QLatin1String("subtitle"))
    targetProperty = QStringLiteral("sid");

  if (targetProperty.isEmpty())
    return;

  if (id <= 0)
    m_ctrl->setProperty(targetProperty, QStringLiteral("no"));
  else
    m_ctrl->setProperty(targetProperty, id);

  if (type == QLatin1String("video") && !m_source.isEmpty()) {
    m_pendingTrackSeek = m_position;
    load(m_source, QStringLiteral("replace"));
  }
}

void MpvPlayer::setVideoFilter(const QString &op, const QString &val) {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("vf"), op, val});
}

void MpvPlayer::setAudioFilter(const QString &op, const QString &val) {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("af"), op, val});
}

void MpvPlayer::adjustProperty(const QString &name, double value) {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("add"), name, QString::number(value)});
}

void MpvPlayer::cycleProperty(const QString &name, const QString &direction) {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("cycle"), name, direction});
}

void MpvPlayer::multiplyProperty(const QString &name, double factor) {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("multiply"), name, QString::number(factor)});
}

void MpvPlayer::deleteProperty(const QString &name) {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("del"), name});
}

void MpvPlayer::emitError(int code, const QString &message) {
  m_lastError[QStringLiteral("code")] = code;
  m_lastError[QStringLiteral("message")] = message;
  emit lastErrorChanged();
  emit errorOccurred(code, message);
}

void MpvPlayer::setLogLevel(const QString &level) {
  m_logClient->setupLogClient(m_ctrl->rawMpv());
  m_logClient->setLogLevel(level);
}

void MpvPlayer::loadConfigFile(const QString &path) {
  mpv_handle *handle = m_ctrl->rawMpv();
  if (handle)
    mpv_load_config_file(handle, path.toUtf8().constData());
}

void MpvPlayer::setProperty(const QString &name, const QString &value) {
  m_ctrl->rawSetPropertyString(name, value);
}

int MpvPlayer::commandAsync(const QStringList &params, int id) {
  return m_ctrl->commandAsync(params, id);
}

void MpvPlayer::setRenderParameter(const QString &name, const QVariant &value) {
  m_pendingRenderParams.append({name, value});
}

void MpvPlayer::hookAdd(const QString &name, int priority) {
  m_hookManager->setupHookClient(m_ctrl->rawMpv());
  m_hookManager->hookAdd(name, priority);
}

void MpvPlayer::hookContinue(quint64 id) {
  m_hookManager->hookContinue(id);
}

void MpvPlayer::toggleAbLoop() {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("ab-loop")});
}

void MpvPlayer::saveWatchLaterState() {
  if (m_rendererInitialized)
    m_ctrl->command(
        QStringList{QStringLiteral("write-watch-later-config")});
}

void MpvPlayer::dropPlaybackBuffers() {
  if (m_rendererInitialized)
    m_ctrl->command(QStringList{QStringLiteral("drop-buffers")});
}

void MpvPlayer::screenshot(const QString &mode) {
  if (m_rendererInitialized) {
    m_ctrl->command(QStringList{QStringLiteral("screenshot"), mode});
  }
}

void MpvPlayer::runExternalProcess(const QStringList &arguments) {
  if (!m_rendererInitialized || arguments.isEmpty())
    return;
  QStringList completeCmd = arguments;
  completeCmd.prepend(QStringLiteral("run"));
  m_ctrl->command(completeCmd);
}

void MpvPlayer::sendScriptMessage(const QStringList &arguments) {
  if (!m_rendererInitialized || arguments.isEmpty())
    return;
  QStringList completeCmd = arguments;
  completeCmd.prepend(QStringLiteral("script-message"));
  m_ctrl->command(completeCmd);
}

void MpvPlayer::triggerScriptBinding(const QString &name) {
  if (m_rendererInitialized && !name.isEmpty())
    m_ctrl->command(
        QStringList{QStringLiteral("script-binding"), name});
}

void MpvPlayer::sendMousePosition(int x, int y) {
  if (m_rendererInitialized) {
    m_ctrl->setProperty(QStringLiteral("mouse-pos/x"), x);
    m_ctrl->setProperty(QStringLiteral("mouse-pos/y"), y);
  }
}

// button is 1-based in mpv's ordering: 1=left, 2=middle, 3=right.
// Translated to mpv's zero-based MOUSE_BTN0/1/2/... naming.
// Note: this does NOT match Qt::MouseButton values (Left=1, Right=2, Middle=4);
// callers coming from a Qt::MouseEvent must remap.
void MpvPlayer::sendMouseClick(int button, bool isPress) {
  if (m_rendererInitialized) {
    QString buttonName =
        QStringLiteral("MOUSE_BTN") + QString::number(button - 1);
    QString action =
        isPress ? QStringLiteral("keydown") : QStringLiteral("keyup");
    m_ctrl->command(QStringList{action, buttonName});
  }
}

void MpvPlayer::setVideoZoom(double val) {
  MPV_SETTER_DOUBLE(m_videoZoom, videoZoom, "video-zoom")
}

void MpvPlayer::setVideoPanX(double val) {
  MPV_SETTER_DOUBLE(m_videoPanX, videoPanX, "video-pan-x")
}

void MpvPlayer::setVideoPanY(double val) {
  MPV_SETTER_DOUBLE(m_videoPanY, videoPanY, "video-pan-y")
}

void MpvPlayer::setAudioPitchCorrection(bool val) {
  MPV_SETTER_BOOL(m_audioPitchCorrection, audioPitchCorrection,
                  "audio-pitch-correction")
}

void MpvPlayer::setSubVisibility(bool val) {
  MPV_SETTER_BOOL(m_subVisibility, subVisibility, "sub-visibility")
}

void MpvPlayer::setBrightness(double val) {
  MPV_SETTER_CLAMPED(m_brightness, brightness, "brightness", -100.0, 100.0)
}

void MpvPlayer::setContrast(double val) {
  MPV_SETTER_CLAMPED(m_contrast, contrast, "contrast", -100.0, 100.0)
}

// -- Rendering attachment

void MpvPlayer::syncProperties() {
  // Helper: pick value from pending (pre-init intent) or fall back to the live
  // member. The observer on the worker thread may have already overwritten the
  // member with mpv's defaults; pending preserves the QML-intended value.
  auto pending = [&](const QString &key, const QVariant &fallback) {
    return m_pendingProperties.value(key, fallback);
  };

  if (m_dirtyProperties.contains(QStringLiteral("keepaspect")))
    m_ctrl->setProperty(QStringLiteral("keepaspect"),
        pending(QStringLiteral("keepaspect"),
                QStringLiteral("yes")).toString());
  if (m_dirtyProperties.contains(QStringLiteral("panscan")))
    m_ctrl->setProperty(QStringLiteral("panscan"),
        pending(QStringLiteral("panscan"), 0.0).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("screenshot-directory"))
      && !m_screenshotDirectory.isEmpty())
    m_ctrl->setProperty(QStringLiteral("screenshot-directory"),
        pending(QStringLiteral("screenshot-directory"),
                m_screenshotDirectory).toString());
  if (m_dirtyProperties.contains(QStringLiteral("loop-file")))
    m_ctrl->setProperty(QStringLiteral("loop-file"),
        pending(QStringLiteral("loop-file"), m_loopFile).toString());
  if (m_dirtyProperties.contains(QStringLiteral("loop-playlist")))
    m_ctrl->setProperty(QStringLiteral("loop-playlist"),
        pending(QStringLiteral("loop-playlist"), m_loopPlaylist).toString());
  if (m_dirtyProperties.contains(QStringLiteral("video-zoom")))
    m_ctrl->setProperty(QStringLiteral("video-zoom"),
        pending(QStringLiteral("video-zoom"), m_videoZoom).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("video-pan-x")))
    m_ctrl->setProperty(QStringLiteral("video-pan-x"),
        pending(QStringLiteral("video-pan-x"), m_videoPanX).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("video-pan-y")))
    m_ctrl->setProperty(QStringLiteral("video-pan-y"),
        pending(QStringLiteral("video-pan-y"), m_videoPanY).toDouble());
  if (m_dirtyProperties.contains(
          QStringLiteral("audio-pitch-correction")))
    m_ctrl->setProperty(QStringLiteral("audio-pitch-correction"),
        pending(QStringLiteral("audio-pitch-correction"),
                m_audioPitchCorrection).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
  if (m_dirtyProperties.contains(QStringLiteral("sub-visibility")))
    m_ctrl->setProperty(QStringLiteral("sub-visibility"),
        pending(QStringLiteral("sub-visibility"), m_subVisibility).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
  if (m_dirtyProperties.contains(QStringLiteral("brightness")))
    m_ctrl->setProperty(QStringLiteral("brightness"),
        pending(QStringLiteral("brightness"), m_brightness).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("contrast")))
    m_ctrl->setProperty(QStringLiteral("contrast"),
        pending(QStringLiteral("contrast"), m_contrast).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("speed")))
    m_ctrl->setProperty(QStringLiteral("speed"),
        pending(QStringLiteral("speed"), m_playbackSpeed).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("mute")))
    m_ctrl->setProperty(QStringLiteral("mute"),
        pending(QStringLiteral("mute"), m_mute).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
  if (m_dirtyProperties.contains(QStringLiteral("volume")))
    m_ctrl->setProperty(QStringLiteral("volume"),
        pending(QStringLiteral("volume"), m_volume).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("audio-device")))
    m_ctrl->setProperty(QStringLiteral("audio-device"),
        pending(QStringLiteral("audio-device"),
                m_currentAudioDevice).toString());
  if (m_dirtyProperties.contains(QStringLiteral("hwdec")))
    m_ctrl->setProperty(QStringLiteral("hwdec"),
        pending(QStringLiteral("hwdec"), m_hwdec).toString());
  if (m_dirtyProperties.contains(QStringLiteral("video-sync")))
    m_ctrl->setProperty(QStringLiteral("video-sync"),
        pending(QStringLiteral("video-sync"), m_videoSync).toString());
  if (m_dirtyProperties.contains(QStringLiteral("interpolation")))
    m_ctrl->setProperty(QStringLiteral("interpolation"),
        pending(QStringLiteral("interpolation"), m_interpolation).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
  if (m_dirtyProperties.contains(QStringLiteral("tscale")))
    m_ctrl->setProperty(QStringLiteral("tscale"),
        pending(QStringLiteral("tscale"), m_tscale).toString());
  if (m_dirtyProperties.contains(QStringLiteral("glsl-shaders"))
      && !m_userShaders.isEmpty())
    m_ctrl->setProperty(QStringLiteral("glsl-shaders"),
        pending(QStringLiteral("glsl-shaders"), m_userShaders).toString());
  if (m_dirtyProperties.contains(QStringLiteral("ytdl")))
    m_ctrl->setProperty(QStringLiteral("ytdl"),
        pending(QStringLiteral("ytdl"), m_ytdl).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
  if (m_dirtyProperties.contains(QStringLiteral("audio-delay")))
    m_ctrl->setProperty(QStringLiteral("audio-delay"),
        pending(QStringLiteral("audio-delay"), m_audioDelay).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("sub-delay")))
    m_ctrl->setProperty(QStringLiteral("sub-delay"),
        pending(QStringLiteral("sub-delay"), m_subDelay).toDouble());
  if (m_dirtyProperties.contains(QStringLiteral("pause")))
    m_ctrl->setProperty(QStringLiteral("pause"),
        pending(QStringLiteral("pause"), m_paused).toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));

  m_pendingProperties.clear();

  for (auto &action : m_deferredInitActions)
    action();
  m_deferredInitActions.clear();
  m_dirtyProperties.clear();
}


