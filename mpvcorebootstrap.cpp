// SPDX-License-Identifier: LGPL-3.0-only
#include "mpvcorebootstrap_p.h"

#include <clocale>
#include <cstring>
#include <mutex>

mpv_handle *MpvQmlPrivate::createMpvCore() {
  static std::mutex bootstrap;
  const std::lock_guard lock(bootstrap);
  // libmpv checks the global numeric locale in mpv_create() and requires
  // it for the core's lifetime; a thread-local override is insufficient.
  // Keep playback's existing C numeric locale without touching LC_ALL or
  // restoring it while a playback or capture core may still be using it.
  const char *numeric = std::setlocale(LC_NUMERIC, nullptr);
  if ((!numeric || std::strcmp(numeric, "C") != 0) && !std::setlocale(LC_NUMERIC, "C"))
    return nullptr;
  return mpv_create();
}
