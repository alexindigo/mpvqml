// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <mpv/client.h>

namespace MpvQmlPrivate {

// The sole entry point for a new core. The calling worker still owns its
// options, mpv_initialize() and retirement policy.
mpv_handle *createMpvCore();

} // namespace MpvQmlPrivate
