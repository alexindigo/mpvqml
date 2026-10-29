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
    int error = 0;
    QString message;
    lease = MpvCoreLease::reserve(error, message);
    if (!lease) return error;
    std::unique_ptr<mpv_handle, decltype(&mpv_destroy)> candidate(mpv_create(), mpv_destroy);
    if (!candidate) return MPV_ERROR_GENERIC;
    error = mpv_set_option_string(candidate.get(), "config", "no");
    if (error >= 0) error = mpv_initialize(candidate.get());
    if (error < 0) return error;
    lease->publish(candidate.release());
  }
  receipt->outstanding = &MpvCoreLease::outstandingReservations;
  receipt->mpvAddress = reinterpret_cast<void *>(&mpv_error_string);
  return MPV_ERROR_SUCCESS; // local leases queue retirement before returning
}
