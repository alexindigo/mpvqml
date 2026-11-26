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
#include "mpvbackendcore_p.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <mutex>
#include <utility>

namespace MpvQmlPrivate {
namespace {

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
        if (pthread_create(&m_threads[started], nullptr, &run, this) != 0)
          break;
      }
      if (started != 2) {
        // Bootstrap has admitted no cores. The first worker cannot be in
        // file/graphics I/O, so rollback here does not restore a teardown join.
        m_stop.store(true, std::memory_order_release);
        for (int i = 0; i < started; ++i)
          sem_post(&m_work);
        for (int i = 0; i < started; ++i)
          pthread_join(m_threads[i], nullptr);
        sem_destroy(&m_work);
        // Completing call_once on failure makes the service error sticky.
        m_error = QStringLiteral("Cannot start native retirement worker");
        return;
      }
      m_ready = true;
    });
    message = m_error;
    return m_ready;
  }

  int reserve() noexcept {
    for (int i = 0; i < MpvBackendCore::Capacity; ++i) {
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
    // Capacity is below SEM_VALUE_MAX.
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
  std::array<RetirementSlot, MpvBackendCore::Capacity> m_slots{};
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

MpvBackendCore::MpvBackendCore(int reservation, Retirement mode) noexcept
    : m_reservation(reservation), m_mode(mode) {}

std::unique_ptr<MpvBackendCore> MpvBackendCore::reserve(Retirement mode, MpvCoreDiagnostic &diagnostic) {
  try {
    QString message;
    if (!retirement().bootstrap(message)) {
      diagnostic = {MpvCoreStage::Service, MPV_ERROR_GENERIC, std::move(message)};
      return {};
    }
    const int index = retirement().reserve();
    if (index < 0) {
      diagnostic = {MpvCoreStage::Capacity, MPV_ERROR_NOMEM,
                    QStringLiteral("All %1 mpv core retirement leases are occupied").arg(Capacity)};
      return {};
    }
    if (auto *core = new (std::nothrow) MpvBackendCore(index, mode)) {
      diagnostic = {};
      return std::unique_ptr<MpvBackendCore>(core);
    }
    retirement().retire(index, nullptr);
  } catch (const std::bad_alloc &) {
  }
  diagnostic = {MpvCoreStage::Allocation, MPV_ERROR_NOMEM,
                QStringLiteral("Cannot allocate mpv core lease ownership")};
  return {};
}

MpvBackendCore::~MpvBackendCore() {
  finishExclusive();
  retirement().retire(m_reservation, m_handle);
}

void MpvBackendCore::finishExclusive() noexcept {
  if (m_mode == Retirement::Exclusive && m_handle)
    mpv_terminate_destroy(std::exchange(m_handle, nullptr));
}

int MpvBackendCore::outstandingReservations() { return retirement().outstanding(); }

bool MpvBackendCore::hasCapacity() { return outstandingReservations() < Capacity; }

MpvCoreDiagnostic MpvBackendCore::initialize(std::initializer_list<Option> options) {
  if (std::exchange(m_attempted, true))
    return {MpvCoreStage::Initialize, MPV_ERROR_INVALID_PARAMETER,
            QStringLiteral("mpv core initialization was already attempted")};
  std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> candidate(nullptr, mpv_destroy);
  {
    static std::mutex bootstrap;
    const std::lock_guard lock(bootstrap);
    // libmpv requires a process-wide C numeric locale for the core's lifetime.
    // Serialize only this library's creation, not unrelated host locale changes.
    const char *numeric = std::setlocale(LC_NUMERIC, nullptr);
    if ((!numeric || std::strcmp(numeric, "C") != 0) && !std::setlocale(LC_NUMERIC, "C"))
      return {MpvCoreStage::Locale, MPV_ERROR_GENERIC, QStringLiteral("Cannot set LC_NUMERIC to C")};
    candidate.reset(mpv_create());
  }
  if (!candidate)
    return {MpvCoreStage::Create, MPV_ERROR_GENERIC, QStringLiteral("mpv_create returned null: core creation failed")};
  for (auto [name, value] : options) {
    const int error = mpv_set_option_string(candidate.get(), name, value);
    if (error < 0)
      return {MpvCoreStage::Option, error, QString::fromLatin1(name) + ": " + mpv_error_string(error)};
  }
  const int error = mpv_initialize(candidate.get());
  if (error < 0)
    return {MpvCoreStage::Initialize, error, QString::fromUtf8(mpv_error_string(error))};
  m_handle = candidate.release();
  return {};
}

} // namespace MpvQmlPrivate
