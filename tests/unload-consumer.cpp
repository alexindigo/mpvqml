#include "unload-consumer.h"
#include "mpvrenderbridge_p.h"

#include <array>
#include <memory>

// Loaded by a native executable with no mpvqml/MpvQt link dependency. This DSO
// finishes all of its own work before being closed while retirement is blocked.
extern "C" int mpvqml_test_start_blocked_retirement(MpvQmlUnloadReceipt *receipt) {
  using namespace MpvQmlPrivate;
  std::array<std::shared_ptr<MpvCoreLease>, 2> leases;
  for (auto &lease : leases) {
    MpvCoreDiagnostic diagnostic;
    lease = MpvCoreLease::reserve(diagnostic);
    if (!lease) return diagnostic.error;
    diagnostic = lease->initialize({{"config", "no"}});
    if (diagnostic.error < 0) return diagnostic.error;
  }
  receipt->outstanding = &MpvBackendCore::outstandingReservations;
  receipt->mpvAddress = reinterpret_cast<void *>(&mpv_error_string);
  return MPV_ERROR_SUCCESS; // local leases queue retirement before returning
}
