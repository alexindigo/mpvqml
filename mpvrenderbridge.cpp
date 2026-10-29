/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Handle-lifetime mechanics adapted from MpvQt edb1e32; modified 2026-10-01.
 * See UPSTREAM.md. The retirement implementation is maintained in mpvqml.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mpvrenderbridge_p.h"

#include <QMetaObject>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <array>
#include <mutex>
#include <limits>

namespace MpvQmlPrivate {
namespace {

constexpr int capacity = 32;
enum SlotState { Free, Reserved, Queued, Destroying };
struct RetirementSlot {
  std::atomic<int> state{Free};
  mpv_handle *handle = nullptr;
};

// dladdr identifies this actually loaded DSO, not a configured install path.
char moduleAnchor;
void *pinModule(void *address, QString &message) {
  Dl_info identity{};
  if (!dladdr(address, &identity) || !identity.dli_fname || !identity.dli_fbase) {
    message = QStringLiteral("Cannot identify a loaded retirement module");
    return nullptr;
  }
  char *path = realpath(identity.dli_fname, nullptr);
  if (!path) {
    message = QStringLiteral("Cannot resolve a loaded retirement module");
    return nullptr;
  }
  void *pin = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
  std::free(path);
  if (!pin) {
    const char *error = dlerror();
    message = QStringLiteral("Cannot pin retirement module: ")
        + QString::fromUtf8(error ? error : "unknown loader error");
    return nullptr;
  }
  link_map *loaded = nullptr;
  if (dlinfo(pin, RTLD_DI_LINKMAP, &loaded) != 0 || !loaded
      || reinterpret_cast<void *>(loaded->l_addr) != identity.dli_fbase) {
    // RTLD_NODELETE references intentionally remain retained, including failed
    // bootstrap attempts. No worker/core is admitted on an identity mismatch.
    message = QStringLiteral("Retirement module pin does not match loaded identity");
    return nullptr;
  }
  return pin;
}

class RetirementService final {
public:
  bool bootstrap(QString &message) {
    std::call_once(m_once, [this]() {
      // A non-interposed client API symbol identifies libmpv even when a
      // child-scoped fixture interposes mpv_create()/mpv_initialize().
      m_modulePin = pinModule(&moduleAnchor, m_error);
      if (!m_modulePin)
        return;
      m_mpvPin = pinModule(reinterpret_cast<void *>(&mpv_error_string), m_error);
      if (!m_mpvPin)
        return;
      if (sem_init(&m_work, 0, 0) != 0) {
        m_error = QStringLiteral("Cannot initialize native retirement semaphore");
        return;
      }
      int started = 0;
      for (; started < 2; ++started) {
        int result = pthread_create(&m_threads[started], nullptr, &run, this);
        if (result != 0) {
          m_error = QStringLiteral("Cannot start native retirement worker: ")
              + QString::fromLocal8Bit(std::strerror(result));
          break;
        }
      }
      if (started != 2) {
        // Bootstrap has admitted no cores. The first worker cannot be in
        // file/graphics I/O, so rollback here does not restore a teardown join.
        m_stop.store(true, std::memory_order_release);
        for (int i = 0; i < started; ++i)
          sem_post(&m_work);
        for (int i = 0; i < started; ++i)
          pthread_join(m_threads[i], nullptr);
        return;
      }
      m_ready = true;
    });
    message = m_error;
    return m_ready;
  }

  int reserve() noexcept {
    for (int i = 0; i < capacity; ++i) {
      int expected = Free;
      if (m_slots[i].state.compare_exchange_strong(
              expected, Reserved, std::memory_order_acq_rel))
        return i;
    }
    return -1;
  }

  void retire(int index, mpv_handle *handle) noexcept {
    auto &slot = m_slots[index];
    if (!handle) {
      slot.state.store(Free, std::memory_order_release);
      return;
    }
    slot.handle = handle;
    slot.state.store(Queued, std::memory_order_release);
    // There are at most 32 outstanding reservations, below SEM_VALUE_MAX.
    // No queue allocation, Qt call, join or I/O is performed by the releaser.
    sem_post(&m_work);
  }

  int outstanding() const noexcept {
    int count = 0;
    for (const auto &slot : m_slots)
      count += slot.state.load(std::memory_order_acquire) != Free;
    return count;
  }

private:
  static void *run(void *context) noexcept {
    static_cast<RetirementService *>(context)->drain();
    return nullptr;
  }

  void drain() noexcept {
    for (;;) {
      while (sem_wait(&m_work) != 0) {
        if (errno != EINTR)
          return; // no handle is discarded; its reserved slot stays owned
      }
      if (m_stop.load(std::memory_order_acquire))
        return;
      for (auto &slot : m_slots) {
        int expected = Queued;
        if (!slot.state.compare_exchange_strong(
                expected, Destroying, std::memory_order_acq_rel))
          continue;
        mpv_terminate_destroy(slot.handle);
        slot.handle = nullptr;
        slot.state.store(Free, std::memory_order_release);
        break;
      }
    }
  }

  std::once_flag m_once;
  std::array<RetirementSlot, capacity> m_slots{};
  pthread_t m_threads[2]{};
  sem_t m_work{};
  std::atomic<bool> m_stop{false};
  bool m_ready = false;
  QString m_error;
  void *m_modulePin = nullptr;
  void *m_mpvPin = nullptr;
};

RetirementService &retirement() {
  // Deliberately process-lifetime. Neither QCoreApplication teardown nor a
  // static destructor waits for native workers or blocked mpv file writers.
  static RetirementService *service = new RetirementService;
  return *service;
}

} // namespace

MpvCoreLease::MpvCoreLease(int reservation) noexcept
    : m_reservation(reservation) {}

std::shared_ptr<MpvCoreLease> MpvCoreLease::reserve(int &error, QString &message) {
  error = MPV_ERROR_GENERIC;
  if (!retirement().bootstrap(message))
    return {};
  const int index = retirement().reserve();
  if (index < 0) {
    error = MPV_ERROR_NOMEM;
    message = QStringLiteral("All 32 mpv core retirement leases are occupied");
    return {};
  }
  MpvCoreLease *raw = nullptr;
  try {
    raw = new MpvCoreLease(index);
  } catch (const std::bad_alloc &) {
    retirement().retire(index, nullptr);
    error = MPV_ERROR_NOMEM;
    message = QStringLiteral("Cannot allocate mpv core lease ownership");
    return {};
  }
  try {
    auto lease = std::shared_ptr<MpvCoreLease>(raw);
    error = MPV_ERROR_SUCCESS;
    message.clear();
    return lease;
  } catch (const std::bad_alloc &) {
    // shared_ptr deletes raw (and releases its reservation) if allocating the
    // control block fails. Releasing that slot again could free a new owner.
    error = MPV_ERROR_NOMEM;
    message = QStringLiteral("Cannot allocate mpv core lease ownership");
    return {};
  }
}

MpvCoreLease::~MpvCoreLease() {
  retirement().retire(m_reservation, m_handle);
}

void MpvCoreLease::publish(mpv_handle *handle) noexcept { m_handle = handle; }

int MpvCoreLease::outstandingReservations() noexcept {
  return retirement().outstanding();
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
