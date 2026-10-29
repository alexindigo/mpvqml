/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * GL/FBO mechanics adapted from MpvQt edb1e32; modified 2026-10-01 and
 * 2026-10-29 (libmpv scaler-LUT upload guard). See UPSTREAM.md.
 * Snapshot/callback ownership is maintained inside mpvqml.
 */
#include "mpvrenderer_p.h"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace MpvQmlPrivate {
namespace {

// ---- libmpv float texture-creation upload guard ---------------------------
//
// Upstream defect (libmpv 0.41.0, still present on master 2026-10-28):
// video/out/gpu/video.c:1852 reinit_scaler() allocates the scaler LUT with
// talloc_array() (should be talloc_zero_array()). With the default separable
// kernels (lanczos radius 3: 6 taps, LUT rows padded to 8 floats) two floats per
// row stay uninitialised heap memory and are uploaded through glTexImage2D as
// GL_FLOAT data into an rgba16f GL_LINEAR texture. When recycled heap places a
// NaN/Inf there, the zero-weight neighbour of linear filtering turns the used
// weights into NaN on NaN-propagating drivers (llvmpipe), the scaled chroma plane
// becomes NaN and the video renders permanently black while playback, readiness
// and render calls all look healthy (intermittent: depends on heap history).
//
// Scope: only the texture-creation entry points libmpv's GL backend resolves
// through MPV_RENDER_PARAM_OPENGL_INIT_PARAMS (glTexImage1D/2D/3D; see
// video/out/opengl/common.c and ra_gl.c gl_tex_create/gl_tex_upload). Those
// carry every `initial_data` upload (LUTs, dither, user textures) plus 1D/3D
// re-uploads. Only GL_FLOAT client memory is inspected; libmpv's GL formats use
// GL_UNSIGNED_BYTE/GL_UNSIGNED_SHORT/GL_FLOAT client types only (formats.c), never
// GL_HALF_FLOAT. Integer data and per-frame 2D plane uploads (glTexSubImage2D,
// not wrapped) pass through untouched without a copy. libmpv's buffer is never
// written; a sanitised copy is uploaded only when a value needs replacing:
// non-finite -> 0, and for 16-bit float internal formats finite values beyond
// the half range are clamped to +/-65504 (some drivers store them as Inf).
// A bound GL_PIXEL_UNPACK_BUFFER makes `pixels` a buffer offset; libmpv uploads
// initial data from client memory, so that case passes through unchanged.
//
// Removal trigger: a libmpv release that zero-initialises the LUT, verified by
// the mpvqml_lut_guard_tests poisoned-padding regression without this guard.
//
// The wrappers hold no state: each call resolves the real entry point and unpack
// state from the calling thread's current context, so concurrent render threads
// and contexts never share a function pointer or buffer.

using TexImage1DFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLint, GLenum,
                                                GLenum, const void *);
using TexImage2DFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                                GLenum, GLenum, const void *);
using TexImage3DFn = void (QOPENGLF_APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                                GLint, GLenum, GLenum, const void *);

constexpr GLenum unpackSwapBytes = 0x0CF0;
constexpr GLenum unpackRowLength = 0x0CF2;
constexpr GLenum unpackSkipRows = 0x0CF3;
constexpr GLenum unpackSkipPixels = 0x0CF4;
constexpr GLenum unpackSkipImages = 0x806D;
constexpr GLenum unpackImageHeight = 0x806E;
constexpr GLenum pixelUnpackBufferBinding = 0x88EF;
constexpr float halfMax = 65504.0f;

int floatComponents(GLenum format) {
  switch (format) {
  case 0x1903: case 0x1904: case 0x1905: case 0x1906: case 0x1909: case 0x1902:
    return 1; // RED, GREEN, BLUE, ALPHA, LUMINANCE, DEPTH_COMPONENT
  case 0x8227: case 0x190A:
    return 2; // RG, LUMINANCE_ALPHA
  case 0x1907: case 0x80E0:
    return 3; // RGB, BGR
  case 0x1908: case 0x80E1:
    return 4; // RGBA, BGRA
  default:
    return 0;
  }
}

bool halfFloatStorage(GLint internalFormat) {
  // R16F, RG16F, RGB16F, RGBA16F
  return internalFormat == 0x822D || internalFormat == 0x822F
      || internalFormat == 0x881B || internalFormat == 0x881A;
}

bool needsReplacement(float value, bool half) {
  return !std::isfinite(value) || (half && std::fabs(value) > halfMax);
}

float replacement(float value) {
  return std::isfinite(value) ? std::copysign(halfMax, value) : 0.0f;
}

// Returns `pixels`, or a sanitised copy in `storage` honouring the current
// unpack state, for an upload of width x height x depth texels.
const void *guardedPixels(GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth,
                          bool threeDimensional, GLenum format, GLenum type,
                          const void *pixels, std::vector<float> &storage) noexcept {
  const int components = floatComponents(format);
  if (type != GL_FLOAT || !pixels || components == 0 || width <= 0 || height <= 0 || depth <= 0)
    return pixels;
  QOpenGLContext *context = QOpenGLContext::currentContext();
  if (!context)
    return pixels;
  QOpenGLFunctions *gl = context->functions();
  const bool unpackState = !context->isOpenGLES() || context->format().majorVersion() >= 3;
  GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0, skipImages = 0,
        imageHeight = 0, buffer = 0, swap = 0;
  gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  if (unpackState) {
    gl->glGetIntegerv(pixelUnpackBufferBinding, &buffer);
    gl->glGetIntegerv(unpackRowLength, &rowLength);
    gl->glGetIntegerv(unpackSkipRows, &skipRows);
    gl->glGetIntegerv(unpackSkipPixels, &skipPixels);
    if (threeDimensional) {
      gl->glGetIntegerv(unpackSkipImages, &skipImages);
      gl->glGetIntegerv(unpackImageHeight, &imageHeight);
    }
    if (!context->isOpenGLES())
      gl->glGetIntegerv(unpackSwapBytes, &swap);
  }
  // A bound unpack buffer turns `pixels` into an offset; byte-swapped client
  // data is not native floats. libmpv uses neither for initial data.
  if (buffer != 0 || swap != 0 || alignment <= 0 || rowLength < 0 || skipRows < 0
      || skipPixels < 0 || skipImages < 0 || imageHeight < 0)
    return pixels;

  constexpr unsigned long long elementBytes = sizeof(float);
  const unsigned long long groupBytes = components * elementBytes;
  unsigned long long rowBytes = (rowLength > 0 ? rowLength : width) * groupBytes;
  if (elementBytes < static_cast<unsigned long long>(alignment))
    rowBytes = (rowBytes + alignment - 1) / alignment * alignment;
  const unsigned long long imageBytes = (imageHeight > 0 ? imageHeight : height) * rowBytes;
  const unsigned long long offset = (threeDimensional ? skipImages * imageBytes : 0)
      + skipRows * rowBytes + skipPixels * groupBytes;
  const unsigned long long extent = offset + (depth - 1) * imageBytes + (height - 1) * rowBytes
      + width * groupBytes;
  if (extent / elementBytes > std::numeric_limits<std::size_t>::max() / elementBytes)
    return pixels;
  const std::size_t count = static_cast<std::size_t>(extent / elementBytes);

  const bool half = halfFloatStorage(internalFormat);
  const auto *source = static_cast<const unsigned char *>(pixels);
  std::size_t first = count;
  for (std::size_t i = 0; i < count; ++i) {
    float value;
    std::memcpy(&value, source + i * elementBytes, sizeof value);
    if (needsReplacement(value, half)) {
      first = i;
      break;
    }
  }
  if (first == count)
    return pixels; // clean data: no copy
  try {
    storage.resize(count);
  } catch (...) {
    return pixels; // a C callback must not throw; upload unchanged
  }
  std::memcpy(storage.data(), pixels, count * elementBytes);
  for (std::size_t i = first; i < count; ++i) {
    if (needsReplacement(storage[i], half))
      storage[i] = replacement(storage[i]);
  }
  return storage.data();
}

template <typename Function>
Function currentEntryPoint(const char *name) {
  QOpenGLContext *context = QOpenGLContext::currentContext();
  return context ? reinterpret_cast<Function>(context->getProcAddress(name)) : nullptr;
}

void QOPENGLF_APIENTRY guardedTexImage1D(GLenum target, GLint level, GLint internalFormat,
                                         GLsizei width, GLint border, GLenum format, GLenum type,
                                         const void *pixels) {
  const auto real = currentEntryPoint<TexImage1DFn>("glTexImage1D");
  if (!real)
    return;
  std::vector<float> storage;
  real(target, level, internalFormat, width, border, format, type,
       guardedPixels(internalFormat, width, 1, 1, false, format, type, pixels, storage));
}

void QOPENGLF_APIENTRY guardedTexImage2D(GLenum target, GLint level, GLint internalFormat,
                                         GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void *pixels) {
  const auto real = currentEntryPoint<TexImage2DFn>("glTexImage2D");
  if (!real)
    return;
  std::vector<float> storage;
  real(target, level, internalFormat, width, height, border, format, type,
       guardedPixels(internalFormat, width, height, 1, false, format, type, pixels, storage));
}

void QOPENGLF_APIENTRY guardedTexImage3D(GLenum target, GLint level, GLint internalFormat,
                                         GLsizei width, GLsizei height, GLsizei depth,
                                         GLint border, GLenum format, GLenum type,
                                         const void *pixels) {
  const auto real = currentEntryPoint<TexImage3DFn>("glTexImage3D");
  if (!real)
    return;
  std::vector<float> storage;
  real(target, level, internalFormat, width, height, depth, border, format, type,
       guardedPixels(internalFormat, width, height, depth, true, format, type, pixels, storage));
}

} // namespace

void *resolveMpvGlFunction(QOpenGLContext *creating, const char *name) {
  if (!creating || !name || QOpenGLContext::currentContext() != creating)
    return nullptr;
  const QFunctionPointer real = creating->getProcAddress(QByteArray(name));
  if (!real)
    return nullptr; // keep libmpv's own availability checks truthful
  if (std::strcmp(name, "glTexImage1D") == 0)
    return reinterpret_cast<void *>(&guardedTexImage1D);
  if (std::strcmp(name, "glTexImage2D") == 0)
    return reinterpret_cast<void *>(&guardedTexImage2D);
  if (std::strcmp(name, "glTexImage3D") == 0)
    return reinterpret_cast<void *>(&guardedTexImage3D);
  return reinterpret_cast<void *>(real);
}

MpvRenderer::MpvRenderer(SnapshotSupplier supplier,
                         std::shared_ptr<MpvRenderDispatcher> dispatcher,
                         quint64 generation)
    : m_supplier(std::move(supplier)), m_dispatcher(std::move(dispatcher)),
      m_generation(generation) {}

MpvRenderer::~MpvRenderer() {
  // Qt's scenegraph owns this object and makes its GL context current for
  // node cleanup before destroying the RHI/context (Qt 6.11.2 render loop).
  // This renderer owns only its own context, never a replacement's context.
  if (m_context) {
    m_snapshot.core->markContextRetiring(m_snapshot.generation, m_snapshot.sourceIntent);
    m_dispatcher->postContextRetiring(m_snapshot.generation, m_snapshot.sourceIntent);
    mpv_render_context_set_update_callback(m_context, nullptr, nullptr);
    // The VO can be waiting for the last queued frame when callbacks cease.
    // Consume it without drawing or waiting for presentation before native
    // context free requests VO shutdown. Otherwise flip_page waits its 200 ms
    // missing-render timeout while Qt's GUI is waiting for GL-thread cleanup.
    // This is a render-API handoff, not cancellation of screenshot file I/O.
    if (QOpenGLContext::currentContext() == m_creatingContext
        && (mpv_render_context_update(m_context) & MPV_RENDER_UPDATE_FRAME)) {
      int skip = 1;
      int block = 0;
      mpv_render_param releaseFrame[]{
          {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
          {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
          {MPV_RENDER_PARAM_INVALID, nullptr}};
      mpv_render_context_render(m_context, releaseFrame);
    }
    mpv_render_context_free(m_context);
    m_context = nullptr;
  }
  if (m_ownsContextSlot)
    m_snapshot.core->releaseRenderer(m_snapshot.generation);
  if (m_ready)
    m_dispatcher->postStatus(m_snapshot.generation, false, MPV_ERROR_SUCCESS, {});
  // Native callback barrier/free precede release of the strong dispatcher and
  // core lease. Final core destruction is never joined on the rendering thread.
}

void MpvRenderer::synchronize(QQuickFramebufferObject *item) {
  Q_UNUSED(item)
  // This is the sole invocation of the item-provided callback. Qt blocks the
  // GUI thread here; subsequent rendering/callback/destruction uses only values.
  m_snapshot = m_supplier();
  m_snapshot.generation = m_generation;
}

void *MpvRenderer::getProcAddress(void *context, const char *name) {
  return resolveMpvGlFunction(static_cast<QOpenGLContext *>(context), name);
}

void MpvRenderer::redraw(void *context) noexcept {
  static_cast<MpvRenderDispatcher *>(context)->postRedraw();
}

void MpvRenderer::fail(int error, const QString &message) {
  if (m_failed)
    return;
  m_failed = true;
  m_ready = false;
  m_dispatcher->postStatus(m_snapshot.generation, false, error, message);
}

bool MpvRenderer::createContext() {
  m_attempted = true;
  if (!m_snapshot.core || !m_snapshot.core->handle()) {
    fail(MPV_ERROR_UNINITIALIZED, QStringLiteral("No initialized core for mpv rendering"));
    return false;
  }
  m_creatingContext = QOpenGLContext::currentContext();
  if (!m_creatingContext || !m_creatingContext->isValid()) {
    fail(MPV_ERROR_UNSUPPORTED, QStringLiteral("No current valid OpenGL context for mpv rendering"));
    return false;
  }
  if (!m_snapshot.core->claimRenderer(m_snapshot.generation)) {
    fail(MPV_ERROR_UNSUPPORTED, QStringLiteral("Overlapping mpv render-context ownership is unsupported"));
    return false;
  }
  m_ownsContextSlot = true;
  mpv_opengl_init_params gl{getProcAddress, m_creatingContext};
  mpv_render_param parameters[4]{
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
      {MPV_RENDER_PARAM_INVALID, nullptr},
      {MPV_RENDER_PARAM_INVALID, nullptr}};
  if (m_snapshot.displayType != MPV_RENDER_PARAM_INVALID) {
    if (!m_snapshot.nativeDisplay) {
      fail(MPV_ERROR_UNSUPPORTED, QStringLiteral("Native display unavailable for mpv rendering"));
      return false;
    }
    parameters[2] = {m_snapshot.displayType, m_snapshot.nativeDisplay};
  }
  const int error = mpv_render_context_create(&m_context, m_snapshot.core->handle(), parameters);
  if (error < 0 || !m_context) {
    fail(error < 0 ? error : MPV_ERROR_GENERIC,
         QString::fromUtf8(mpv_error_string(error < 0 ? error : MPV_ERROR_GENERIC)));
    return false;
  }
  mpv_render_context_set_update_callback(m_context, &redraw, m_dispatcher.get());
  return true;
}

QOpenGLFramebufferObject *MpvRenderer::createFramebufferObject(const QSize &size) {
  // Qt immediately reads the returned FBO. Return an owned target even when
  // libmpv initialization failed; never return a null FBO or a false-ready latch.
  QOpenGLFramebufferObject *target = QQuickFramebufferObject::Renderer::createFramebufferObject(size);
  if (!m_attempted)
    createContext();
  if (!target || !target->isValid()) {
    fail(MPV_ERROR_VO_INIT_FAILED, QStringLiteral("Cannot create a valid mpv framebuffer target"));
  } else if (!m_failed && m_context && !m_ready) {
    m_ready = true;
    m_dispatcher->postStatus(m_snapshot.generation, true, MPV_ERROR_SUCCESS, {});
  }
  return target;
}

void MpvRenderer::applyParameters() {
  for (auto it = m_snapshot.parameters.cbegin(); it != m_snapshot.parameters.cend(); ++it) {
    if (m_appliedParameters.value(it.key()) == it.value())
      continue;
    m_appliedParameters.insert(it.key(), it.value());
    // The facade validates/coalesces supported names and integer types. Native
    // context changes are performed exclusively with this matching GL context.
    if (it.key() == QStringLiteral("ambient-light")) {
      int lux = it.value().toInt();
      const int error = mpv_render_context_set_parameter(
          m_context, mpv_render_param{MPV_RENDER_PARAM_AMBIENT_LIGHT, &lux});
      if (error < 0)
        m_dispatcher->postParameterError(m_snapshot.generation, it.key(), error,
                                         QString::fromUtf8(mpv_error_string(error)));
    }
  }
}

void MpvRenderer::render() {
  QOpenGLFramebufferObject *target = framebufferObject();
  QOpenGLContext *current = QOpenGLContext::currentContext();
  if (!target || !target->isValid() || !current) {
    fail(MPV_ERROR_VO_INIT_FAILED, QStringLiteral("mpv framebuffer/OpenGL environment is unavailable"));
    return;
  }
  if (m_failed || !m_context) {
    current->functions()->glClearColor(0, 0, 0, 0);
    current->functions()->glClear(GL_COLOR_BUFFER_BIT);
    QQuickOpenGLUtils::resetOpenGLState();
    return;
  }
  if (current != m_creatingContext) {
    fail(MPV_ERROR_UNSUPPORTED, QStringLiteral("mpv render context belongs to a different OpenGL context"));
    current->functions()->glClearColor(0, 0, 0, 0);
    current->functions()->glClear(GL_COLOR_BUFFER_BIT);
    QQuickOpenGLUtils::resetOpenGLState();
    return;
  }
  applyParameters();
  mpv_opengl_fbo fbo{static_cast<int>(target->handle()), target->width(), target->height(), 0};
  int flip = 0;
  mpv_render_param parameters[]{
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flip},
      {MPV_RENDER_PARAM_INVALID, nullptr}};
  const int error = mpv_render_context_render(m_context, parameters);
  if (error < 0) {
    fail(error, QString::fromUtf8(mpv_error_string(error)));
    current->functions()->glClearColor(0, 0, 0, 0);
    current->functions()->glClear(GL_COLOR_BUFFER_BIT);
  }
  QQuickOpenGLUtils::resetOpenGLState();
}

} // namespace MpvQmlPrivate
