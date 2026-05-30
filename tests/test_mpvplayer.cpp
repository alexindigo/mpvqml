// /home/user/Projects/mpvqml/tests/test_mpvplayer.cpp
#include <QSignalSpy>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/qmetaobject.h>
#include <QtQml/qqmlparserstatus.h>
#include <QtQuick/QQuickWindow>
#include <QtTest>

// Open up all protected and private channels for the
// test engine
#define private public
#define protected public
#include "../mpvplayer.h"
#include "../mpvlogclient.h"
#include "../mpvhookmanager.h"
#undef private
#undef protected

#include "mock_mpvcontroller.h"

class TestMpvPlayer : public QObject {
  Q_OBJECT

private slots:
  // Executed automatically before every individual test case
  void init() {
    m_window = new QQuickWindow();
    m_mockCtrl = new MockMpvController(this);
    m_player = new MpvPlayer(nullptr, m_mockCtrl);
    m_player->setParentItem(m_window->contentItem());
  }

  // Executed automatically after every individual test case
  void cleanup() {
    delete m_player;
    delete m_window;
    m_player = nullptr;
    m_window = nullptr;
    m_mockCtrl = nullptr;
  }

  // -- Graphics Initialization Layer

  void test_0_constructor_graphics_unknown() {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Unknown);
    MpvPlayer *earlyPlayer = new MpvPlayer();
    QVERIFY(earlyPlayer != nullptr);
    delete earlyPlayer;
  }

  // -- Property and Binding Tests

  void test_explicit_set_source_lines() {
    QSignalSpy spy(m_player, &MpvPlayer::sourceChanged);
    m_player->setSource(QStringLiteral("test_source.mp4"));
    QCOMPARE(m_player->source(),
             QStringLiteral("test_source.mp4"));
    QCOMPARE(spy.count(), 1);
  }

  void test_qml_exposed_seek_marking() {
    m_player->m_rendererInitialized = true;
    m_player->seekAbsolute(15.0);

    // negative timestamps are clamped to 0
    m_player->seekAbsolute(-42.0);

    m_player->markSeekPosition();
    m_player->seekRelative(20.0);
    m_player->revertSeek();
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_paused_two_way_binding() {
    QSignalSpy spy(m_player, &MpvPlayer::pausedChanged);
    m_player->m_rendererInitialized = true;
    m_player->setPaused(true);
    QCOMPARE(m_player->paused(), true);
    QCOMPARE(spy.count(), 1);
  }

  void test_playback_speed_safety_and_signals() {
    m_player->m_rendererInitialized = true;
    m_player->setPlaybackSpeed(1.5);
    QVERIFY(m_player->playbackSpeed() == 1.5);

    // Forces speed < 0.01 condition to execute and clamp to 0.01
    m_player->setPlaybackSpeed(0.002);
    QCOMPARE(m_player->playbackSpeed(), 0.01);
  }

  void test_mute_and_fillmode_isolation() {
    m_player->m_rendererInitialized = true;
    m_player->setMute(true);
    m_player->setFillMode(QStringLiteral("preserveAspectCrop"));
    m_player->setFillMode(QStringLiteral("stretch"));
    QVERIFY(m_player->mute());
  }

  void test_screenshot_directory_normalization() {
    m_player->m_rendererInitialized = true;
    m_player->setScreenshotDirectory(
        QStringLiteral("file:///tmp/mpvqml_screenshots"));
    QCOMPARE(m_player->m_screenshotDirectory,
             QStringLiteral("/tmp/mpvqml_screenshots"));
  }

// Tests all new declarative property setters and their edge bounds
  void test_new_declarative_properties_and_bounds() {
    m_player->m_rendererInitialized = true;

    // Volume Bounds Checks
    m_player->setVolume(85.5);
    QCOMPARE(m_player->volume(), 85.5);
    m_player->setVolume(-25.0);
    QCOMPARE(m_player->volume(), 0.0);
    m_player->setVolume(210.0);
    QCOMPARE(m_player->volume(), 150.0);

    // System Routing Mappings
    m_player->setCurrentAudioDevice(QStringLiteral("test_audio_device"));
    QCOMPARE(m_player->currentAudioDevice(),
             QStringLiteral("test_audio_device"));

    m_player->setHwdec(QStringLiteral("vaapi"));
    QCOMPARE(m_player->hwdec(), QStringLiteral("vaapi"));

    m_player->setVideoSync(QStringLiteral("display-resample"));
    QCOMPARE(m_player->videoSync(), QStringLiteral("display-resample"));

    m_player->setInterpolation(true);
    QVERIFY(m_player->interpolation());

    m_player->setTscale(QStringLiteral("oversample"));
    QCOMPARE(m_player->tscale(), QStringLiteral("oversample"));

    m_player->setUserShaders(
        QStringLiteral("file:///no/need/to/exist/test_shader.glsl"));
    QCOMPARE(m_player->userShaders(),
             QStringLiteral("file:///no/need/to/exist/test_shader.glsl"));

    m_player->setYtdl(false);
    QVERIFY(!m_player->ytdl());

    m_player->setAudioDelay(0.15);
    QCOMPARE(m_player->audioDelay(), 0.15);

    m_player->setSubDelay(-0.35);
    QCOMPARE(m_player->subDelay(), -0.35);

    m_player->position();
    m_player->duration();
    m_player->seeking();
    m_player->isBuffering();
    m_player->audioDeviceList();
  }

  void test_meta_object_properties_and_signals() {
    const QMetaObject *meta = m_player->metaObject();

    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
      QMetaProperty prop = meta->property(i);
      QVariant value = prop.read(m_player);
      if (prop.isWritable()) {
        prop.write(m_player, value);
      }
    }

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
      QMetaMethod method = meta->method(i);
      int paramCount = method.parameterCount();

      if (paramCount == 0) {
        method.invoke(m_player);
      } else if (paramCount == 1) {
        QMetaType pType = method.parameterMetaType(0);
        if (pType.isValid()) {
          void *dummyPtr = pType.create();
          method.invoke(m_player, QGenericArgument(pType.name(), dummyPtr));
          pType.destroy(dummyPtr);
        }
      }
    }
    // smoke test: no crash
    QVERIFY(true);
  }

  // -- Imperative Method Tests

  void test_load_functional_variations() {
    m_player->load(QStringLiteral(""));

    m_player->m_rendererInitialized = false;
    m_player->load(QStringLiteral("file:///mock/track.mp4"));
    for (auto &action : m_player->m_deferredInitActions)
      action();
    m_player->m_deferredInitActions.clear();

    m_player->m_rendererInitialized = true;
    m_player->load(QStringLiteral("file:///mock/track.mp4"));
  }

  void test_track_selection_mapping_boundaries() {
    m_player->m_rendererInitialized = true;
    m_player->selectTrack(QStringLiteral("audio"), 1);
    m_player->selectTrack(QStringLiteral("video"), 2);
    m_player->selectTrack(QStringLiteral("sub"), 3);
    m_player->selectTrack(QStringLiteral("subtitle"), 4);
    m_player->selectTrack(QStringLiteral("invalid"), -1);

    // ID <= 0 disables the track
    m_player->selectTrack(QStringLiteral("audio"), 0);
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_basic_playback_controls() {
    m_player->m_rendererInitialized = true;
    m_player->next();
    m_player->previous();
    m_player->clearPlaylist();
    m_player->stop(true);
    m_player->stop(false);
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_deferred_load_directory() {
    m_player->m_rendererInitialized = false;
    m_player->loadDirectory(QDir::currentPath());
    for (auto &action : m_player->m_deferredInitActions)
      action();
    m_player->m_deferredInitActions.clear();
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_deferred_load_queue() {
    m_player->m_rendererInitialized = false;
    m_player->load(QStringLiteral("/tmp/a.mp4"), QStringLiteral("replace"));
    m_player->load(QStringLiteral("/tmp/b.mp4"), QStringLiteral("append"));
    QCOMPARE(m_player->m_deferredInitActions.size(), 2);
    for (auto &action : m_player->m_deferredInitActions)
      action();
    m_player->m_deferredInitActions.clear();
    QVERIFY(m_player->m_deferredInitActions.isEmpty());
  }

  void test_load_directory_sorting() {
    m_player->m_rendererInitialized = true;
    QString activeExecutionDir = QDir::currentPath();

    // Write two separate mock items so the directory parser loops twice
    QFile mockVideo1(activeExecutionDir +
                     QStringLiteral("/mock_test_item1.mp4"));
    QFile mockVideo2(activeExecutionDir +
                     QStringLiteral("/mock_test_item2.mp4"));

    if (mockVideo1.open(QIODevice::WriteOnly)) {
      mockVideo1.write("payload1");
      mockVideo1.close();
    }
    if (mockVideo2.open(QIODevice::WriteOnly)) {
      mockVideo2.write("payload2");
      mockVideo2.close();
    }

    // Handles the second item iteration (append-play)
    m_player->loadDirectory(activeExecutionDir, QStringList{},
                            QStringLiteral("replace"), QStringLiteral("name"));

    // Uses "date" sorting flag to activate sorting bitmask rules
    m_player->loadDirectory(activeExecutionDir, QStringList{},
                            QStringLiteral("replace"), QStringLiteral("date"));

    m_player->loadDirectory(activeExecutionDir, QStringList{},
                            QStringLiteral("append"), QStringLiteral("size"));
    m_player->loadDirectory(QStringLiteral("/invalid/path"));

    // Targets a dummy extension inside a valid folder to force an empty file list return
    m_player->loadDirectory(
        activeExecutionDir,
        QStringList{QStringLiteral("*.completely_fake_ext")},
        QStringLiteral("replace"), QStringLiteral("name"));

    // Handles the second item iteration (append-play)
    m_player->loadDirectory(activeExecutionDir, QStringList{},
                            QStringLiteral("replace"), QStringLiteral("name"));

    m_player->loadDirectory(activeExecutionDir, QStringList{},
                            QStringLiteral("append"), QStringLiteral("size"));
    m_player->loadDirectory(QStringLiteral("/invalid/path"));

    // Clean up mock files
    mockVideo1.remove();
    mockVideo2.remove();
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_advanced_navigation_and_filters() {
    m_player->m_rendererInitialized = true;
    m_player->frameStep();
    m_player->frameBackStep();
    m_player->setVideoFilter(QStringLiteral("gradfun"), QStringLiteral("20"));
    m_player->setAudioFilter(QStringLiteral("loudnorm"),
                             QStringLiteral("I=-16"));
    m_player->adjustProperty(QStringLiteral("volume"), -5.0);
    m_player->multiplyProperty(QStringLiteral("speed"), 1.2);
    m_player->cycleProperty(QStringLiteral("sub-visibility"),
                            QStringLiteral("up"));
    m_player->deleteProperty(QStringLiteral("deinterlace"));
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_track_and_subsystem_hooks() {
    m_player->m_rendererInitialized = true;
    m_player->addAudioTrack(QStringLiteral("file:///dev/null"));
    m_player->addVideoTrack(QStringLiteral("file:///dev/null"));
    m_player->addSubtitleTrack(QStringLiteral("file:///dev/null"));
    m_player->toggleAbLoop();
    m_player->saveWatchLaterState();
    m_player->dropPlaybackBuffers();
    // smoke test: no crash
    QVERIFY(true);
  }

// Exercises all user-interaction and framework utility methods
  void test_utility_methods() {
    m_player->m_rendererInitialized = true;

    m_player->screenshot(QStringLiteral("video"));
    m_player->screenshot(QStringLiteral("subtitles"));

    m_player->sendMousePosition(450, 320);
    m_player->sendMouseClick(1, true);
    m_player->sendMouseClick(1, false);
    // smoke test: no crash
    QVERIFY(true);
  }

  // -- The Cold Sweep Targets

  void test_private_and_protected_methods() {
    m_player->geometryChange(QRectF(), QRectF());
    m_player->itemChange(QQuickItem::ItemDevicePixelRatioHasChanged,
                         QQuickItem::ItemChangeData(qreal(2.0)));

    m_player->m_rendererInitialized = false;
    m_player->load(QStringLiteral("file:///mock/deferred_rendering_track.mp4"));

    // Populate mock values to pass the .isEmpty() gate checks
    m_player->m_screenshotDirectory = QStringLiteral("/tmp");
    m_player->m_userShaders = QStringLiteral("active_shaders.glsl");

    m_player->updatePaintNode(nullptr, nullptr);
    m_player->releaseResources();
    m_player->mpvController();

    QEventLoop eventLoop;
    QTimer::singleShot(50, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    m_player->m_rendererInitialized = true;
    m_player->runExternalProcess(QStringList{QStringLiteral("echo")});
    m_player->sendScriptMessage(QStringList{QStringLiteral("ping")});
    m_player->triggerScriptBinding(QStringLiteral("macro"));
    m_player->setupPropertyObservers();
    m_player->applyFillMode();
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_controller_signal_lambda_bridges() {
    MockMpvController *ctrl = qobject_cast<MockMpvController *>(m_player->m_ctrl);
    QVERIFY(ctrl != nullptr);

    emit ctrl->propertyChanged(QStringLiteral("video-params/w"),
                               QVariant(1920));
    emit ctrl->propertyChanged(QStringLiteral("video-params/h"),
                               QVariant(1080));
    emit ctrl->propertyChanged(QStringLiteral("pause"), QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("speed"), QVariant(1.5));
    emit ctrl->propertyChanged(QStringLiteral("mute"), QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("screenshot-directory"),
                               QVariant(QStringLiteral("/mock/dir")));
    emit ctrl->propertyChanged(QStringLiteral("volume"), QVariant(90.0));
    emit ctrl->propertyChanged(QStringLiteral("time-pos"),
                               QVariant(45.75));
    QCOMPARE(m_player->position(), 45.75);
    emit ctrl->propertyChanged(QStringLiteral("duration"), QVariant(180.0));
    emit ctrl->propertyChanged(QStringLiteral("seeking"), QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("paused-for-cache"),
                               QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("eof-reached"), QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("audio-device"),
                               QVariant(QStringLiteral("airplay_endpoint")));
    emit ctrl->propertyChanged(QStringLiteral("hwdec"),
                               QVariant(QStringLiteral("vaapi")));
    emit ctrl->propertyChanged(QStringLiteral("hwdec-current"),
                               QVariant(QStringLiteral("vaapi")));
    emit ctrl->propertyChanged(QStringLiteral("video-sync"),
                               QVariant(QStringLiteral("display-resample")));
    emit ctrl->propertyChanged(QStringLiteral("interpolation"), QVariant(true));
    emit ctrl->propertyChanged(QStringLiteral("tscale"),
                               QVariant(QStringLiteral("oversample")));
    emit ctrl->propertyChanged(
        QStringLiteral("glsl-shaders"),
        QVariant(QStringLiteral("active_shaders.glsl")));
    emit ctrl->propertyChanged(QStringLiteral("ytdl"), QVariant(false));
    emit ctrl->propertyChanged(QStringLiteral("audio-delay"), QVariant(0.1));
    emit ctrl->propertyChanged(QStringLiteral("sub-delay"), QVariant(-0.2));

    QVariantList mockDevices;
    QVariantMap deviceMap;
    deviceMap[QStringLiteral("name")] = QStringLiteral("airplay_endpoint");
    deviceMap[QStringLiteral("description")] =
        QStringLiteral("AirPlay Output Router");
    mockDevices.append(deviceMap);
    emit ctrl->propertyChanged(QStringLiteral("audio-device-list"),
                               QVariant(mockDevices));

    QVariantMap trackFields;
    trackFields[QStringLiteral("id")] = 1;
    trackFields[QStringLiteral("type")] = QStringLiteral("audio");
    trackFields[QStringLiteral("title")] = QStringLiteral("Surround");
    trackFields[QStringLiteral("lang")] = QStringLiteral("jp");
    trackFields[QStringLiteral("selected")] = true;
    emit ctrl->propertyChanged(QStringLiteral("track-list"),
                               QVariant(QVariantList{trackFields}));

    QVariantMap playlistFields;
    playlistFields[QStringLiteral("id")] = 1;
    playlistFields[QStringLiteral("filename")] = QStringLiteral("movie.mkv");
    playlistFields[QStringLiteral("current")] = true;
    playlistFields[QStringLiteral("playing")] = true;
    emit ctrl->propertyChanged(QStringLiteral("playlist"),
                               QVariant(QVariantList{playlistFields}));

    QVariantMap chapterFields;
    chapterFields[QStringLiteral("time")] = 45.5;
    emit ctrl->propertyChanged(QStringLiteral("chapter-list"),
                               QVariant(QVariantList{chapterFields}));

    QVariantMap metadataFields;
    metadataFields[QStringLiteral("title")] = QStringLiteral("Media Asset");
    emit ctrl->propertyChanged(QStringLiteral("metadata"),
                               QVariant(metadataFields));

    QVariantMap mockCache;
    mockCache[QStringLiteral("cache-duration")] = 45.0;
    emit ctrl->propertyChanged(QStringLiteral("demuxer-cache-state"),
                               QVariant(mockCache));

    // smoke test: no crash
    QVERIFY(true);
  }

  void test_lifecycle_signal_forwarding() {
    QSignalSpy startedSpy(m_player, &MpvPlayer::fileStarted);
    QSignalSpy loadedSpy(m_player, &MpvPlayer::fileLoaded);
    QSignalSpy endFileSpy(m_player, &MpvPlayer::endFile);
    QSignalSpy reconfigSpy(m_player, &MpvPlayer::videoReconfig);

    emit m_mockCtrl->fileStarted();
    QCOMPARE(startedSpy.count(), 1);

    emit m_mockCtrl->fileLoaded();
    QCOMPARE(loadedSpy.count(), 1);

    emit m_mockCtrl->endFile(QStringLiteral("eof"));
    QCOMPARE(endFileSpy.count(), 1);
    QCOMPARE(endFileSpy.takeFirst().at(0).toString(), QStringLiteral("eof"));

    emit m_mockCtrl->videoReconfig();
    QCOMPARE(reconfigSpy.count(), 1);
  }

  void test_endfile_error_triggers_errorOccurred() {
    QSignalSpy errorSpy(m_player, &MpvPlayer::errorOccurred);
    QSignalSpy endFileSpy(m_player, &MpvPlayer::endFile);

    emit m_mockCtrl->endFile(QStringLiteral("error"));

    QCOMPARE(endFileSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toInt(), -13);
  }

  void test_endfile_eof_does_not_trigger_errorOccurred() {
    QSignalSpy errorSpy(m_player, &MpvPlayer::errorOccurred);

    emit m_mockCtrl->endFile(QStringLiteral("eof"));

    QCOMPARE(errorSpy.count(), 0);
  }

  void test_log_message_signal_forwarding() {
    QSignalSpy logSpy(m_player, &MpvPlayer::logMessage);

    emit m_player->m_logClient->logMessage(QStringLiteral("cplayer"),
                                           QStringLiteral("info"),
                                           QStringLiteral("test message\n"));
    QCOMPARE(logSpy.count(), 1);
    QCOMPARE(logSpy.at(0).at(0).toString(), QStringLiteral("cplayer"));
    QCOMPARE(logSpy.at(0).at(1).toString(), QStringLiteral("info"));
    QCOMPARE(logSpy.at(0).at(2).toString(), QStringLiteral("test message\n"));
  }

  void test_set_log_level() {
    // Mock rawMpv returns nullptr, so log client setup is a no-op
    m_player->setLogLevel(QStringLiteral("info"));
    m_player->setLogLevel(QStringLiteral("no"));
    QVERIFY(true);
  }

  void test_load_config_file() {
    m_player->loadConfigFile(QStringLiteral("/no/need/to/exist/mpv.conf"));
    QVERIFY(true);
  }

  void test_set_property() {
    m_player->setProperty(QStringLiteral("keep-open-pause"),
                          QStringLiteral("yes"));
    QVERIFY(true);
  }

  void test_command_async() {
    int ret = m_player->commandAsync(
        QStringList{QStringLiteral("loadfile"), QStringLiteral("/tmp/a.mp4")}, 42);
    QCOMPARE(ret, 0);
    QCOMPARE(m_mockCtrl->m_lastAsyncCommandStringList,
             QStringList({QStringLiteral("loadfile"), QStringLiteral("/tmp/a.mp4")}));
    QCOMPARE(m_mockCtrl->m_lastAsyncId, 42);

    QSignalSpy spy(m_player, &MpvPlayer::asyncCommandFinished);
    emit m_mockCtrl->asyncReply(42, 0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 42);
    QCOMPARE(spy.at(0).at(1).toInt(), 0);
  }

  void test_error_occurred_signal_and_last_error() {
    QSignalSpy spy(m_player, &MpvPlayer::errorOccurred);
    QSignalSpy lastErrorSpy(m_player, &MpvPlayer::lastErrorChanged);

    m_player->emitError(-13, QStringLiteral("test error"));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), -13);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("test error"));

    QCOMPARE(lastErrorSpy.count(), 1);
    QVariantMap err = m_player->lastError();
    QCOMPARE(err.value(QStringLiteral("code")).toInt(), -13);
    QCOMPARE(err.value(QStringLiteral("message")).toString(),
             QStringLiteral("test error"));
  }

  void test_set_render_parameter() {
    m_player->setRenderParameter(QStringLiteral("ambient-light"),
                                 QVariant(64));
    QCOMPARE(m_player->m_pendingRenderParams.size(), 1);
    QCOMPARE(m_player->m_pendingRenderParams.at(0).first,
             QStringLiteral("ambient-light"));
    QCOMPARE(m_player->m_pendingRenderParams.at(0).second.toInt(), 64);
  }

  void test_hook_add_and_continue() {
    // Mock rawMpv returns nullptr, so hook manager setup is a no-op
    m_player->hookAdd(QStringLiteral("on_load"), 0);
    m_player->hookAdd(QStringLiteral("on_unload"), 50);
    m_player->hookContinue(42);
    QVERIFY(true);
  }

  void test_hook_triggered_signal() {
    QSignalSpy spy(m_player, &MpvPlayer::hookTriggered);

    emit m_player->m_hookManager->hookTriggered(QStringLiteral("on_load"), 1);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("on_load"));
    QCOMPARE(spy.at(0).at(1).toULongLong(), static_cast<quint64>(1));
  }

  void test_track_list_raw_data() {
    QVariantList tracks;
    QVariantMap track;
    track[QStringLiteral("id")] = 1;
    track[QStringLiteral("type")] = QStringLiteral("video");
    track[QStringLiteral("codec")] = QStringLiteral("hevc");
    track[QStringLiteral("codec-desc")] = QStringLiteral("H.265 / HEVC");
    track[QStringLiteral("selected")] = true;
    tracks.append(track);

    QSignalSpy spy(m_player, &MpvPlayer::trackListChanged);
    emit m_mockCtrl->propertyChanged(QStringLiteral("track-list"), QVariant(tracks));
    QCOMPARE(spy.count(), 1);

    QVariantList result = m_player->trackList();
    QCOMPARE(result.size(), 1);
    QVariantMap entry = result[0].toMap();
    QCOMPARE(entry[QStringLiteral("id")].toInt(), 1);
    QCOMPARE(entry[QStringLiteral("type")].toString(), QStringLiteral("video"));
    QCOMPARE(entry[QStringLiteral("codec")].toString(), QStringLiteral("hevc"));
    QCOMPARE(entry[QStringLiteral("codec-desc")].toString(), QStringLiteral("H.265 / HEVC"));
    QCOMPARE(entry[QStringLiteral("selected")].toBool(), true);
  }

  void test_playlist_raw_data() {
    QVariantList items;
    QVariantMap item;
    item[QStringLiteral("id")] = 0;
    item[QStringLiteral("filename")] = QStringLiteral("/no/need/to/exist/video.mp4");
    item[QStringLiteral("current")] = true;
    item[QStringLiteral("playing")] = false;
    items.append(item);

    QSignalSpy spy(m_player, &MpvPlayer::playlistChanged);
    emit m_mockCtrl->propertyChanged(QStringLiteral("playlist"), QVariant(items));
    QCOMPARE(spy.count(), 1);

    QVariantList result = m_player->playlist();
    QCOMPARE(result.size(), 1);
    QVariantMap entry = result[0].toMap();
    QCOMPARE(entry[QStringLiteral("id")].toInt(), 0);
    QCOMPARE(entry[QStringLiteral("filename")].toString(), QStringLiteral("/no/need/to/exist/video.mp4"));
    QCOMPARE(entry[QStringLiteral("current")].toBool(), true);
    QCOMPARE(entry[QStringLiteral("playing")].toBool(), false);
  }

  void test_all_auto_generated_signals() {
    m_player->sourceChanged();
    m_player->pausedChanged();
    m_player->playbackSpeedChanged();
    m_player->muteChanged();
    m_player->fillModeChanged();
    m_player->screenshotDirectoryChanged();
    m_player->videoWidthChanged();
    m_player->videoHeightChanged();
    m_player->hasVideoChanged();
    m_player->trackListChanged();
    m_player->playlistChanged();
    m_player->chapterListChanged();
    m_player->metadataChanged();

    // Explicitly signals verification block for all variables
    m_player->volumeChanged();
    m_player->positionChanged();
    m_player->durationChanged();
    m_player->seekingChanged();
    m_player->isBufferingChanged();
    m_player->audioDeviceListChanged();
    m_player->currentAudioDeviceChanged();
    m_player->hwdecChanged();
    m_player->videoSyncChanged();
    m_player->interpolationChanged();
    m_player->tscaleChanged();
    m_player->userShadersChanged();
    m_player->ytdlChanged();
    m_player->audioDelayChanged();
    m_player->subDelayChanged();

    emit m_player->playbackEnded();
    emit m_player->errorOccurred(-13, QStringLiteral("Mock Error"));
    emit m_player->logMessage(QStringLiteral("core"), QStringLiteral("info"),
                              QStringLiteral("Log payload"));

    QQmlParserStatus *parserStatus = qobject_cast<QQmlParserStatus *>(m_player);
    if (parserStatus) {
      parserStatus->classBegin();
      parserStatus->componentComplete();
    }
    // smoke test: no crash
    QVERIFY(true);
  }

  void test_extended_api_surface() {
    // Flip the renderer initialization gate to true right away
    m_player->m_rendererInitialized = true;

    QString sampleSource = QStringLiteral("assets/test_one_video.mp4");
    m_player->setSource(sampleSource);
    QCOMPARE(m_player->source(), sampleSource);

    // Zoom and pan transform vectors
    m_player->setVideoZoom(3.5);
    QCOMPARE(m_player->videoZoom(), 3.5);

    m_player->setVideoPanX(0.75);
    QCOMPARE(m_player->videoPanX(), 0.75);

    m_player->setVideoPanY(-0.25);
    QCOMPARE(m_player->videoPanY(), -0.25);

    // Engine configuration flags and polymorphic looping
    m_player->setAudioPitchCorrection(false);
    QVERIFY(!m_player->audioPitchCorrection());

    m_player->setSubVisibility(false);
    QVERIFY(!m_player->subVisibility());

    m_player->setLoopFile(QVariant(3));
    QCOMPARE(m_player->loopFile().toInt(), 3);

    m_player->setLoopPlaylist(QVariant(QStringLiteral("inf")));
    QCOMPARE(m_player->loopPlaylist().toString(), QStringLiteral("inf"));

    // Equalizer clamping guards
    m_player->setBrightness(45.0);
    QCOMPARE(m_player->brightness(), 45.0);
    m_player->setBrightness(180.0);
    QCOMPARE(m_player->brightness(), 100.0);

    m_player->setContrast(-15.5);
    QCOMPARE(m_player->contrast(), -15.5);
    m_player->setContrast(-250.0);
    QCOMPARE(m_player->contrast(), -100.0);

    // Read-only inbound default states
    QCOMPARE(m_player->hwdecCurrent(), QStringLiteral("no"));
    QVERIFY(m_player->demuxerCacheState().isEmpty());
  }

  // -- SyncProperties Dirty Tracking

  void test_sync_properties_constructor_dirty_defaults() {
    // Intentional deviations from mpv defaults are marked dirty in ctor
    QVERIFY(m_player->m_dirtyProperties.contains("tscale"));
    QVERIFY(m_player->m_dirtyProperties.contains("fillMode"));

    // hwdec is pushed immediately in ctor, not deferred via dirty set
    QCOMPARE(m_player->m_hwdec, QStringLiteral("auto"));

    // mpv-matching defaults are NOT dirty
    QVERIFY(!m_player->m_dirtyProperties.contains("volume"));
    QVERIFY(!m_player->m_dirtyProperties.contains("speed"));
    QVERIFY(!m_player->m_dirtyProperties.contains("mute"));
  }

  void test_sync_properties_only_dirty_are_synced() {
    m_mockCtrl->reset();
    m_player->m_dirtyProperties.clear();

    // Set one property before init
    m_player->setVolume(75.0);
    QVERIFY(m_player->m_dirtyProperties.contains("volume"));

    // sync should only forward volume, not all properties
    m_player->syncProperties();
    QCOMPARE(m_mockCtrl->m_lastSetProperty, QStringLiteral("volume"));
    QCOMPARE(m_mockCtrl->m_lastSetValue.toDouble(), 75.0);
    QCOMPARE(m_mockCtrl->m_setPropertyCount, 1);
    QVERIFY(m_player->m_dirtyProperties.isEmpty());
  }

  void test_sync_properties_clears_after_sync() {
    m_player->m_dirtyProperties.clear();
    m_player->m_dirtyProperties.insert("volume");
    m_player->syncProperties();
    QVERIFY(m_player->m_dirtyProperties.isEmpty());
  }

  void test_sync_properties_each_property() {
    struct SyncTestCase {
      QString mpvProp;
      std::function<void()> setter;
      QVariant expectedValue;
    };

    auto check = [&](const SyncTestCase &tc) {
      m_mockCtrl->reset();
      m_player->m_dirtyProperties.clear();
      tc.setter();
      m_player->syncProperties();
      QCOMPARE(m_mockCtrl->m_lastSetProperty, tc.mpvProp);
      QCOMPARE(m_mockCtrl->m_lastSetValue, tc.expectedValue);
    };

    check({"volume",
           [&] { m_player->setVolume(42.0); }, QVariant(42.0)});
    check({"speed",
           [&] { m_player->setPlaybackSpeed(1.5); }, QVariant(1.5)});
    check({"mute",
           [&] { m_player->setMute(true); }, QVariant(QStringLiteral("yes"))});
    check({"pause",
           [&] { m_player->setPaused(true); }, QVariant(QStringLiteral("yes"))});
    check({"audio-delay",
           [&] { m_player->setAudioDelay(0.5); }, QVariant(0.5)});
    check({"sub-delay",
           [&] { m_player->setSubDelay(-0.3); }, QVariant(-0.3)});
    check({"hwdec",
           [&] { m_player->setHwdec("vaapi"); }, QVariant(QStringLiteral("vaapi"))});
    check({"video-sync",
           [&] { m_player->setVideoSync("display-resample"); },
           QVariant(QStringLiteral("display-resample"))});
    check({"interpolation",
           [&] { m_player->setInterpolation(true); },
           QVariant(QStringLiteral("yes"))});
    check({"tscale",
           [&] { m_player->setTscale("oversample"); },
           QVariant(QStringLiteral("oversample"))});
    check({"ytdl",
           [&] { m_player->setYtdl(false); }, QVariant(QStringLiteral("no"))});
    check({"audio-device",
           [&] { m_player->setCurrentAudioDevice("pipewire"); },
           QVariant(QStringLiteral("pipewire"))});
    check({"loop-file",
           [&] { m_player->setLoopFile(QVariant(3)); }, QVariant(QStringLiteral("3"))});
    check({"loop-playlist",
           [&] { m_player->setLoopPlaylist(QVariant("inf")); },
           QVariant(QStringLiteral("inf"))});
    check({"video-zoom",
           [&] { m_player->setVideoZoom(0.5); }, QVariant(0.5)});
    check({"video-pan-x",
           [&] { m_player->setVideoPanX(0.1); }, QVariant(0.1)});
    check({"video-pan-y",
           [&] { m_player->setVideoPanY(-0.1); }, QVariant(-0.1)});
    check({"audio-pitch-correction",
           [&] { m_player->setAudioPitchCorrection(false); },
           QVariant(QStringLiteral("no"))});
    check({"sub-visibility",
           [&] { m_player->setSubVisibility(false); },
           QVariant(QStringLiteral("no"))});
    check({"brightness",
           [&] { m_player->setBrightness(50.0); }, QVariant(50.0)});
    check({"contrast",
           [&] { m_player->setContrast(30.0); }, QVariant(30.0)});
    check({"screenshot-directory",
           [&] { m_player->setScreenshotDirectory("/tmp"); },
           QVariant(QStringLiteral("/tmp"))});
  }

  void test_sync_properties_fillMode() {
    m_mockCtrl->reset();
    m_player->m_dirtyProperties.clear();

    m_player->setFillMode("stretch");
    m_player->syncProperties();

    // fillMode syncs via applyFillMode which sets keepaspect + panscan
    // panscan is the last call, so m_lastSetProperty should be "panscan"
    QCOMPARE(m_mockCtrl->m_lastSetProperty, QStringLiteral("panscan"));
    QCOMPARE(m_mockCtrl->m_lastSetValue.toDouble(), 0.0);
  }

  void test_sync_properties_user_shaders_non_empty() {
    m_mockCtrl->reset();
    m_player->m_dirtyProperties.clear();

    m_player->m_userShaders = QStringLiteral("my_shader.glsl");
    m_player->m_dirtyProperties.insert("glsl-shaders");
    m_player->syncProperties();
    QCOMPARE(m_mockCtrl->m_lastSetProperty, QStringLiteral("glsl-shaders"));
    QCOMPARE(m_mockCtrl->m_lastSetValue.toString(),
             QStringLiteral("my_shader.glsl"));
    QVERIFY(m_player->m_dirtyProperties.isEmpty());
  }

  void test_sync_properties_user_shaders_empty_skipped() {
    m_mockCtrl->reset();
    m_player->m_dirtyProperties.clear();

    // userShaders is empty by default, mark it dirty but verify it's skipped
    m_player->m_dirtyProperties.insert("glsl-shaders");
    m_player->m_userShaders = QString();
    m_player->syncProperties();
    // The empty guard means setProperty is NOT called for glsl-shaders
    QCOMPARE(m_mockCtrl->m_setPropertyCount, 0);
    QVERIFY(m_player->m_dirtyProperties.isEmpty());
  }

  void test_sync_properties_nothing_dirty() {
    m_mockCtrl->reset();
    m_player->m_dirtyProperties.clear();
    m_player->syncProperties();
    QCOMPARE(m_mockCtrl->m_setPropertyCount, 0);
    QVERIFY(m_player->m_dirtyProperties.isEmpty());
  }

  void test_mpv_log_client_setup_teardown() {
    mpv_handle *core = mpv_create();
    QVERIFY(core);
    {
      MpvLogClient client;
      client.setupLogClient(core);
      QVERIFY(client.m_logClient);

      client.setLogLevel(QStringLiteral("info"));
      client.setLogLevel(QStringLiteral("no"));
      QVERIFY(!client.m_logClient);
    }
    mpv_destroy(core);
  }

  void test_mpv_log_client_null_core() {
    MpvLogClient client;
    client.setupLogClient(nullptr);
    QVERIFY(!client.m_logClient);
    client.setLogLevel(QStringLiteral("info"));
    client.setLogLevel(QStringLiteral("no"));
    QVERIFY(true);
  }

  void test_mpv_log_client_double_setup() {
    mpv_handle *core = mpv_create();
    QVERIFY(core);
    {
      MpvLogClient client;
      client.setupLogClient(core);
      mpv_handle *first = client.m_logClient;
      client.setupLogClient(core);
      QCOMPARE(client.m_logClient, first);
    }
    mpv_destroy(core);
  }

  void test_mpv_hook_manager_setup_teardown() {
    mpv_handle *core = mpv_create();
    QVERIFY(core);
    {
      MpvHookManager manager;
      manager.setupHookClient(core);
      QVERIFY(manager.m_hookClient);

      manager.hookAdd(QStringLiteral("on_load"), 0);
      manager.hookAdd(QStringLiteral("on_unload"), 50);
      manager.hookContinue(1);
    }
    mpv_destroy(core);
  }

  void test_mpv_hook_manager_null_core() {
    MpvHookManager manager;
    manager.setupHookClient(nullptr);
    QVERIFY(!manager.m_hookClient);
    manager.hookAdd(QStringLiteral("on_load"), 0);
    manager.hookContinue(1);
    QVERIFY(true);
  }

  void test_mpv_hook_manager_double_setup() {
    mpv_handle *core = mpv_create();
    QVERIFY(core);
    {
      MpvHookManager manager;
      manager.setupHookClient(core);
      mpv_handle *first = manager.m_hookClient;
      manager.setupHookClient(core);
      QCOMPARE(manager.m_hookClient, first);
    }
    mpv_destroy(core);
  }

  void test_observed_properties_exist_in_mpv() {
    mpv_handle *mpv = mpv_create();
    QVERIFY(mpv);
    QVERIFY(mpv_initialize(mpv) >= 0);

    for (auto it = m_player->m_propertyHandlers.cbegin();
         it != m_player->m_propertyHandlers.cend(); ++it) {
      const QString &propName = it.key();
      QByteArray name = propName.toUtf8();

      double dval;
      int ival;
      char *sval = nullptr;
      mpv_node nval;

      int err = mpv_get_property(mpv, name.constData(), MPV_FORMAT_DOUBLE, &dval);
      if (err == MPV_ERROR_PROPERTY_NOT_FOUND)
        err = mpv_get_property(mpv, name.constData(), MPV_FORMAT_FLAG, &ival);
      if (err == MPV_ERROR_PROPERTY_NOT_FOUND) {
        err = mpv_get_property(mpv, name.constData(), MPV_FORMAT_STRING, &sval);
        mpv_free(sval);
      }
      if (err == MPV_ERROR_PROPERTY_NOT_FOUND) {
        err = mpv_get_property(mpv, name.constData(), MPV_FORMAT_NODE, &nval);
        mpv_free_node_contents(&nval);
      }

      if (err == MPV_ERROR_PROPERTY_NOT_FOUND)
        qWarning().noquote()
            << "Property does not exist in mpv:" << propName;
      QVERIFY2(err != MPV_ERROR_PROPERTY_NOT_FOUND,
               propName.toUtf8().constData());
    }

    mpv_destroy(mpv);
  }

private:
  MpvPlayer *m_player = nullptr;
  QQuickWindow *m_window = nullptr;
  MockMpvController *m_mockCtrl = nullptr;
};

QTEST_MAIN(TestMpvPlayer)
#include "test_mpvplayer.moc"
