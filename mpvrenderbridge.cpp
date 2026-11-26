// SPDX-License-Identifier: LGPL-3.0-only
#include "mpvrenderbridge_p.h"

#include <QMetaObject>
#include <limits>
#include <new>
#include <utility>

namespace MpvQmlPrivate {

MpvCoreLease::MpvCoreLease(std::unique_ptr<MpvBackendCore> backend) noexcept
    : m_backend(std::move(backend)) {}

std::shared_ptr<MpvCoreLease> MpvCoreLease::reserve(MpvCoreDiagnostic &diagnostic) {
  auto backend = MpvBackendCore::reserve(MpvBackendCore::Retirement::Shared, diagnostic);
  if (!backend)
    return {};
  try {
    return std::shared_ptr<MpvCoreLease>(new MpvCoreLease(std::move(backend)));
  } catch (const std::bad_alloc &) {
    diagnostic = {MpvCoreStage::Allocation, MPV_ERROR_NOMEM,
                  QStringLiteral("Cannot allocate playback lease ownership")};
    return {};
  }
}

bool MpvCoreLease::claimRenderer(quint64 generation) noexcept {
  quint64 expected = 0;
  return generation != 0 && m_renderOwner.compare_exchange_strong(
      expected, generation, std::memory_order_acq_rel);
}

void MpvCoreLease::releaseRenderer(quint64 generation) noexcept {
  m_renderOwner.compare_exchange_strong(generation, 0, std::memory_order_acq_rel);
}

quint64 MpvCoreLease::allocateNativeToken() noexcept {
  quint64 next = m_nextNativeToken.load(std::memory_order_acquire);
  while (next) {
    const quint64 following = next == std::numeric_limits<quint64>::max() ? 0 : next + 1;
    if (m_nextNativeToken.compare_exchange_weak(next, following, std::memory_order_acq_rel))
      return next;
  }
  return 0; // exhausted, never wrap or reuse a native token
}

void MpvCoreLease::noteNativeToken(quint64 token) noexcept {
  quint64 next = m_nextNativeToken.load(std::memory_order_acquire);
  while (next && token >= next) {
    const quint64 following = token == std::numeric_limits<quint64>::max() ? 0 : token + 1;
    if (m_nextNativeToken.compare_exchange_weak(next, following, std::memory_order_acq_rel))
      return;
  }
}

void MpvCoreLease::markContextRetiring(quint64 generation, quint64 intent) noexcept {
  m_retirementSequence.fetch_add(1);
  m_retiringIntent.store(intent);
  m_retiringGeneration.store(generation);
  m_retirementSequence.fetch_add(1);
}

quint64 MpvCoreLease::retiringContext(quint64 intent) const noexcept {
  const quint64 sequence = m_retirementSequence.load();
  if (sequence & 1)
    return 0;
  const quint64 generation = m_retiringGeneration.load();
  const quint64 markedIntent = m_retiringIntent.load();
  return sequence == m_retirementSequence.load() && generation && markedIntent == intent
      ? generation : 0;
}

MpvRenderDispatcher::MpvRenderDispatcher() : QObject(nullptr) {}

std::shared_ptr<MpvRenderDispatcher> MpvRenderDispatcher::create() {
  return {new MpvRenderDispatcher, [](MpvRenderDispatcher *dispatcher) {
    // This is a strong-reference release from a possible render thread.
    // QObject's deferred deletion is scheduled only on its owning GUI thread.
    QMetaObject::invokeMethod(dispatcher, &QObject::deleteLater, Qt::QueuedConnection);
  }};
}

void MpvRenderDispatcher::postRedraw() noexcept {
  if (m_redrawPending.exchange(true, std::memory_order_acq_rel))
    return;
  try {
    if (QMetaObject::invokeMethod(this, [this]() {
          m_redrawPending.store(false, std::memory_order_release);
          emit redrawRequested();
        }, Qt::QueuedConnection))
      return;
  } catch (...) {
    // A C callback must not propagate an allocation/Qt invocation exception.
  }
  m_redrawPending.store(false, std::memory_order_release);
}

bool MpvRenderDispatcher::postStatus(quint64 generation, bool ready, int error,
                                     const QString &message) {
  return QMetaObject::invokeMethod(this, [this, generation, ready, error, message]() {
    emit statusChanged(generation, ready, error, message);
  }, Qt::QueuedConnection);
}

bool MpvRenderDispatcher::postParameterError(quint64 generation, const QString &name,
                                             int error, const QString &message) {
  return QMetaObject::invokeMethod(this, [this, generation, name, error, message]() {
    emit parameterFailed(generation, name, error, message);
  }, Qt::QueuedConnection);
}

bool MpvRenderDispatcher::postContextRetiring(quint64 generation, quint64 sourceIntent) {
  return QMetaObject::invokeMethod(this, [this, generation, sourceIntent]() {
    emit contextRetiring(generation, sourceIntent);
  }, Qt::QueuedConnection);
}

} // namespace MpvQmlPrivate
