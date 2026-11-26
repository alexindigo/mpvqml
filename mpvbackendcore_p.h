// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QString>
#include <initializer_list>
#include <memory>
#include <mpv/client.h>

namespace MpvQmlPrivate {

enum class MpvCoreStage { None, Service, Capacity, Allocation, Locale, Create, Option, Initialize };

struct MpvCoreDiagnostic {
  MpvCoreStage stage = MpvCoreStage::None;
  int error = MPV_ERROR_SUCCESS;
  QString message;
};

// Non-GL ownership of one native reservation. No handle can be adopted from
// outside: creation and initialization are single-attempt under this permit.
class MpvBackendCore final {
public:
  enum class Retirement { Shared, Exclusive };
  struct Option { const char *name; const char *value; };
  static constexpr int Capacity = 32;

  static std::unique_ptr<MpvBackendCore> reserve(Retirement mode, MpvCoreDiagnostic &diagnostic);
  static int outstandingReservations();
  static bool hasCapacity();

  ~MpvBackendCore();
  MpvBackendCore(const MpvBackendCore &) = delete;
  MpvBackendCore &operator=(const MpvBackendCore &) = delete;

  MpvCoreDiagnostic initialize(std::initializer_list<Option> options);
  mpv_handle *handle() const noexcept { return m_handle; }
  int reservation() const noexcept { return m_reservation; }

  // Exclusive workers free their native context first and may keep other
  // resources alive through this call, then release the reservation separately.
  void finishExclusive() noexcept;

private:
  MpvBackendCore(int reservation, Retirement mode) noexcept;
  const int m_reservation;
  const Retirement m_mode;
  bool m_attempted = false;
  mpv_handle *m_handle = nullptr;
};

} // namespace MpvQmlPrivate
