#include "mpvplayer.h"
#include "mpvrenderbridge_p.h"
#include "mpvrenderer_p.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QQuickWindow>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace MpvQmlPrivate;

namespace {

constexpr GLenum texture3D = 0x806F;
constexpr GLenum rgba32f = 0x8814;
constexpr GLenum rgba16f = 0x881A;
constexpr GLenum unpackRowLength = 0x0CF2;
constexpr GLenum unpackSkipRows = 0x0CF3;
constexpr GLenum unpackSkipPixels = 0x0CF4;
constexpr GLenum unpackSkipImages = 0x806D;
constexpr GLenum unpackImageHeight = 0x806E;
constexpr GLenum pixelUnpackBuffer = 0x88EC;
constexpr GLenum streamDraw = 0x88E0;

using TexImage2DFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                                GLenum, GLenum, const void *);
using TexImage3DFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                                GLint, GLenum, GLenum, const void *);
using GetTexImageFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLenum, GLenum, void *);

QString fixtureName;

bool sameBits(const std::vector<float> &a, const std::vector<float> &b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

// Runs only inside the preloaded child: actual product player and window pixels.
class LutGuardFixture : public QObject {
  Q_OBJECT
private slots:
  void paddingPoisonedVideoRenders() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(160, 120);
    window.setPersistentSceneGraph(false);
    window.setPersistentGraphics(false);
    auto *player = new MpvPlayer(window.contentItem());
    player->setSize(window.size());
    connect(&window, &QQuickWindow::widthChanged, player, [player](int value) { player->setWidth(value); });
    connect(&window, &QQuickWindow::heightChanged, player, [player](int value) { player->setHeight(value); });
    player->setHwdec(QStringLiteral("no"));
    player->setLoopFile(QStringLiteral("inf"));
    player->setSource(QStringLiteral(MPVQML_TEST_VIDEO));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(player->rendererReady(), 5000);
    auto blue = [&window]() {
      const QImage image = window.grabWindow();
      if (image.isNull())
        return false;
      const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
      return pixel.blue() > 150 && pixel.red() < 80 && pixel.green() < 80;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blue(), 5000);
#if defined(__SANITIZE_ADDRESS__)
    // The sanitizer allocator owns malloc; its fill option poisons every fresh
    // block (including the LUT padding) with 0xFF, a NaN bit pattern.
    qInfo("LUT_GUARD_EXERCISED %s asan-fill", qPrintable(fixtureName));
#else
    using Count = long (*)();
    const auto count = reinterpret_cast<Count>(dlsym(RTLD_DEFAULT, "mpvqml_lut_poison_count"));
    QVERIFY2(count, "LUT poison interposer is not preloaded; case NOT_EXERCISED");
    QVERIFY2(count() >= 1, "no scaler-LUT-sized allocation was poisoned; case NOT_EXERCISED");
    qInfo("LUT_GUARD_EXERCISED %s poisoned=%ld", qPrintable(fixtureName), count());
#endif
    delete player;
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(MpvCoreLease::outstandingReservations(), 0, 5000);
  }
};

class TestLutGuard : public QObject {
  Q_OBJECT
private slots:
  void guardSanitizesOnlyClientFloatTextureCreation() {
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext context;
    QVERIFY(context.create());
    QVERIFY(context.makeCurrent(&surface));
    if (context.isOpenGLES())
      QSKIP("glGetTexImage readback requires desktop OpenGL");
    QOpenGLFunctions *gl = context.functions();
    const auto raw = [&context](const char *name) {
      return reinterpret_cast<void *>(context.getProcAddress(name));
    };

    // Resolution: only texture creation is wrapped; nothing without the
    // creating context current; every other name (including ones the platform
    // answers with a dispatch stub) is exactly the context's own answer.
    QCOMPARE(resolveMpvGlFunction(nullptr, "glTexImage2D"), nullptr);
    QCOMPARE(resolveMpvGlFunction(&context, "glNoSuchEntryPointForMpvQml"),
             raw("glNoSuchEntryPointForMpvQml"));
    QCOMPARE(resolveMpvGlFunction(&context, "glTexSubImage2D"), raw("glTexSubImage2D"));
    QCOMPARE(resolveMpvGlFunction(&context, "glClear"), raw("glClear"));
    void *texImage2D = resolveMpvGlFunction(&context, "glTexImage2D");
    void *texImage3D = resolveMpvGlFunction(&context, "glTexImage3D");
    QVERIFY(texImage2D && texImage2D != raw("glTexImage2D"));
    QVERIFY(texImage3D && texImage3D != raw("glTexImage3D"));
    QVERIFY(resolveMpvGlFunction(&context, "glTexImage1D") != raw("glTexImage1D"));
    const auto upload2D = reinterpret_cast<TexImage2DFn>(texImage2D);
    const auto upload3D = reinterpret_cast<TexImage3DFn>(texImage3D);
    const auto readBack = reinterpret_cast<GetTexImageFn>(raw("glGetTexImage"));
    QVERIFY(readBack);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // 3x3 RGBA client image; the 2x2 upload region starts at pixel (1,1) via
    // ROW_LENGTH 3, SKIP_ROWS 1, SKIP_PIXELS 1. Skipped pixel (0,0) holds NaN.
    std::vector<float> source(3 * 3 * 4);
    for (std::size_t i = 0; i < source.size(); ++i)
      source[i] = 0.01f * static_cast<float>(i);
    auto pixel = [&source](int x, int y) { return source.data() + (y * 3 + x) * 4; };
    auto set = [](float *texel, std::initializer_list<float> values) {
      std::copy(values.begin(), values.end(), texel);
    };
    set(pixel(0, 0), {nan, nan, nan, nan});
    set(pixel(1, 1), {nan, inf, -inf, 2.0f});
    set(pixel(2, 1), {1e30f, -1e30f, 0.5f, 1.0f});
    const std::vector<float> original = source;

    GLuint textures[5]{};
    gl->glGenTextures(5, textures);
    auto uploadRegion = [&](GLuint texture, GLenum internalFormat) {
      gl->glBindTexture(GL_TEXTURE_2D, texture);
      gl->glPixelStorei(unpackRowLength, 3);
      gl->glPixelStorei(unpackSkipRows, 1);
      gl->glPixelStorei(unpackSkipPixels, 1);
      gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      upload2D(GL_TEXTURE_2D, 0, internalFormat, 2, 2, 0, GL_RGBA, GL_FLOAT, source.data());
      gl->glPixelStorei(unpackRowLength, 0);
      gl->glPixelStorei(unpackSkipRows, 0);
      gl->glPixelStorei(unpackSkipPixels, 0);
      std::vector<float> texels(2 * 2 * 4);
      readBack(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, texels.data());
      return texels;
    };
    const std::vector<float> full = uploadRegion(textures[0], rgba32f);
    QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
    QVERIFY2(sameBits(source, original), "the caller's buffer must never be written");
    std::vector<float> expected;
    for (const float *texel : {pixel(1, 1), pixel(2, 1), pixel(1, 2), pixel(2, 2)})
      expected.insert(expected.end(), texel, texel + 4);
    expected[0] = expected[1] = expected[2] = 0.0f; // non-finite -> 0
    QVERIFY2(sameBits(full, expected), "unpack-state region must be sanitised exactly");

    const std::vector<float> half = uploadRegion(textures[1], rgba16f);
    QCOMPARE(half[0], 0.0f);
    QCOMPARE(half[3], 2.0f);
    QCOMPARE(half[4], 65504.0f); // finite beyond half range: clamped, not Inf
    QCOMPARE(half[5], -65504.0f);
    QCOMPARE(half[6], 0.5f);

    // Integer data passes through untouched.
    const unsigned char bytes[8]{255, 0, 128, 7, 1, 2, 3, 4};
    gl->glBindTexture(GL_TEXTURE_2D, textures[2]);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    upload2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
    unsigned char bytesBack[8]{};
    readBack(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytesBack);
    QCOMPARE(std::memcmp(bytes, bytesBack, sizeof bytes), 0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // With an unpack buffer bound the pointer is an offset: never dereferenced,
    // passed through unchanged (the buffer's NaN reaches the texture).
    using BufferFn = void (QOPENGLF_APIENTRY *)(GLenum, GLsizeiptr, const void *, GLenum);
    GLuint buffer = 0;
    gl->glGenBuffers(1, &buffer);
    gl->glBindBuffer(pixelUnpackBuffer, buffer);
    const float buffered[8]{1.0f, 1.0f, 1.0f, 1.0f, nan, 0.25f, 0.5f, 1.0f};
    reinterpret_cast<BufferFn>(raw("glBufferData"))(pixelUnpackBuffer, sizeof buffered, buffered, streamDraw);
    gl->glBindTexture(GL_TEXTURE_2D, textures[3]);
    upload2D(GL_TEXTURE_2D, 0, rgba32f, 1, 1, 0, GL_RGBA, GL_FLOAT,
             reinterpret_cast<const void *>(sizeof(float) * 4));
    gl->glBindBuffer(pixelUnpackBuffer, 0);
    gl->glDeleteBuffers(1, &buffer);
    float bufferedBack[4]{};
    readBack(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, bufferedBack);
    QVERIFY(std::isnan(bufferedBack[0]));
    QCOMPARE(bufferedBack[1], 0.25f);

    // 3D: IMAGE_HEIGHT 2 and SKIP_IMAGES 1 select images 1 and 2, row 0.
    std::vector<float> volume(3 * 2 * 4, 0.75f);
    volume[(1 * 2 + 0) * 4 + 0] = nan;
    volume[(2 * 2 + 0) * 4 + 1] = inf;
    volume[(0 * 2 + 0) * 4 + 2] = nan; // skipped image
    gl->glBindTexture(texture3D, textures[4]);
    gl->glPixelStorei(unpackImageHeight, 2);
    gl->glPixelStorei(unpackSkipImages, 1);
    upload3D(texture3D, 0, rgba32f, 1, 1, 2, 0, GL_RGBA, GL_FLOAT, volume.data());
    gl->glPixelStorei(unpackImageHeight, 0);
    gl->glPixelStorei(unpackSkipImages, 0);
    float volumeBack[8]{};
    readBack(texture3D, 0, GL_RGBA, GL_FLOAT, volumeBack);
    const float volumeExpected[8]{0.0f, 0.75f, 0.75f, 0.75f, 0.75f, 0.0f, 0.75f, 0.75f};
    QCOMPARE(std::memcmp(volumeBack, volumeExpected, sizeof volumeBack), 0);

    QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
    gl->glDeleteTextures(5, textures);
    context.doneCurrent();
  }

  void poisonedLutPaddingStillRendersVideo() {
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("LD_PRELOAD");
#if defined(__SANITIZE_ADDRESS__)
    environment.insert("ASAN_OPTIONS", environment.value("ASAN_OPTIONS")
        + QStringLiteral(":malloc_fill_byte=255:max_malloc_fill_size=16384"));
#else
    environment.insert("LD_PRELOAD", QStringLiteral(MPVQML_LUT_POISON_LIBRARY));
#endif
    environment.insert("QT_FORCE_STDERR_LOGGING", "1");
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    const QString name = QStringLiteral("paddingPoisonedVideoRenders");
    child.start(QCoreApplication::applicationFilePath(), QStringList{"--fixture", name});
    QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
    if (!child.waitForFinished(60000)) {
      child.kill(); // this owned test child is not qs or the locker
      child.waitForFinished(5000);
      QFAIL("LUT guard child timed out; case NOT_EXERCISED");
    }
    const QByteArray output = child.readAll();
    qInfo().noquote() << QString::fromUtf8(output);
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QVERIFY2(output.contains(("LUT_GUARD_EXERCISED " + name).toUtf8()),
             "child did not observe rendered video under poisoned LUT padding");
  }
};

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() == 3 && arguments[1] == "--fixture") {
    fixtureName = arguments[2];
    LutGuardFixture fixture;
    return QTest::qExec(&fixture, QStringList{arguments[0], fixtureName, "-v1"});
  }
  TestLutGuard tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "test_lut_guard.moc"
