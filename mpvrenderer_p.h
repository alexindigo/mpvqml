/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * GL/FBO mechanics adapted from MpvQt edb1e32; modified 2026-10-01 and
 * 2026-10-29. See UPSTREAM.md. This header is private to mpvqml.
 */
#pragma once

#include "mpvrenderbridge_p.h"
#include <QQuickFramebufferObject>
#include <functional>
#include <memory>
#include <mpv/render_gl.h>

class QOpenGLContext;
class QRunnable;

namespace MpvQmlPrivate {

// libmpv's get_proc_address for `creating` (which must be current). Returns the
// real entry point, except glTexImage1D/2D/3D, which are returned wrapped by the
// stateless float-upload guard documented in mpvrenderer.cpp.
void *resolveMpvGlFunction(QOpenGLContext *creating, const char *name);

class MpvRenderer final : public QQuickFramebufferObject::Renderer {
public:
  using SnapshotSupplier = std::function<MpvRenderSnapshot()>;
  MpvRenderer(SnapshotSupplier supplier, std::shared_ptr<MpvRenderDispatcher> dispatcher,
              quint64 generation);
  ~MpvRenderer() override;
  void synchronize(QQuickFramebufferObject *item) override;
  QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;
  void render() override;
  // Created during synchronization, scheduled by the facade on its window.
  // Holds no item/renderer pointer and cannot prolong native ownership.
  std::unique_ptr<QRunnable> releaseJob() const;

private:
  struct ContextOwner;
  static void *getProcAddress(void *context, const char *name);
  static void redraw(void *context) noexcept;
  bool createContext();
  void fail(int error, const QString &message);
  void applyParameters();
  SnapshotSupplier m_supplier;
  MpvRenderSnapshot m_snapshot;
  std::shared_ptr<MpvRenderDispatcher> m_dispatcher;
  const quint64 m_generation;
  std::shared_ptr<ContextOwner> m_native;
  bool m_attempted = false;
  bool m_failed = false;
  bool m_ready = false;
  QVariantMap m_appliedParameters;
};

} // namespace MpvQmlPrivate
