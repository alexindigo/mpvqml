/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Handle-lifetime mechanics adapted from MpvQt edb1e32; modified 2026-10-01.
 * See UPSTREAM.md. This header is private to mpvqml.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>

namespace MpvQmlPrivate {

// A reservation exists before mpv_create(). Every published handle therefore
// already has storage for its eventual allocation-free retirement transfer.
class MpvCoreLease final {
public:
  static std::shared_ptr<MpvCoreLease> reserve(int &error, QString &message);
  static int outstandingReservations() noexcept;
  ~MpvCoreLease();
  MpvCoreLease(const MpvCoreLease &) = delete;
  MpvCoreLease &operator=(const MpvCoreLease &) = delete;

  void publish(mpv_handle *handle) noexcept;
  mpv_handle *handle() const noexcept { return m_handle; }
  int reservation() const noexcept { return m_reservation; }
  bool claimRenderer(quint64 generation) noexcept;
  void releaseRenderer(quint64 generation) noexcept;
  quint64 allocateNativeToken() noexcept;
  void noteNativeToken(quint64 token) noexcept;
  quint64 sourceIntent() const noexcept { return m_sourceIntent.load(std::memory_order_acquire); }
  quint64 advanceSourceIntent() noexcept { return m_sourceIntent.fetch_add(1, std::memory_order_acq_rel) + 1; }
  void markContextRetiring(quint64 generation, quint64 intent) noexcept;
  quint64 retiringContext(quint64 intent) const noexcept;

private:
  explicit MpvCoreLease(int reservation) noexcept;
  int m_reservation;
  mpv_handle *m_handle = nullptr;
  std::atomic<quint64> m_renderOwner{0};
  std::atomic<quint64> m_nextNativeToken{1};
  std::atomic<quint64> m_sourceIntent{0};
  std::atomic<quint64> m_retiringGeneration{0};
  std::atomic<quint64> m_retiringIntent{0};
  std::atomic<quint64> m_retirementSequence{0};
};

// Parentless and GUI-affine. A renderer owns a strong reference throughout
// callback registration; the native callback never knows its consumer item.
class MpvRenderDispatcher final : public QObject {
  Q_OBJECT
public:
  explicit MpvRenderDispatcher();
  static std::shared_ptr<MpvRenderDispatcher> create();
  void postRedraw() noexcept;
  bool postStatus(quint64 generation, bool ready, int error,
                  const QString &message);
  bool postParameterError(quint64 generation, const QString &name, int error,
                          const QString &message);
  bool postContextRetiring(quint64 generation, quint64 sourceIntent);

signals:
  void redrawRequested();
  void statusChanged(quint64 generation, bool ready, int error,
                     const QString &message);
  void parameterFailed(quint64 generation, const QString &name, int error,
                        const QString &message);
  void contextRetiring(quint64 generation, quint64 sourceIntent);

private:
  std::atomic<bool> m_redrawPending{false};
};

struct MpvRenderSnapshot {
  std::shared_ptr<MpvCoreLease> core;
  quint64 generation = 0;
  quint64 sourceIntent = 0;
  mpv_render_param_type displayType = MPV_RENDER_PARAM_INVALID;
  void *nativeDisplay = nullptr;
  QVariantMap parameters;
};

struct MpvRenderBridge {
  std::shared_ptr<MpvCoreLease> core;
  std::shared_ptr<MpvRenderDispatcher> dispatcher;
  std::atomic<quint64> generation{0};
};

} // namespace MpvQmlPrivate
