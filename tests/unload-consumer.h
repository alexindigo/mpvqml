#pragma once

// Test-only receipt contains addresses in the production modules, not pointers
// into the disposable consumer DSO or objects it owns.
struct MpvQmlUnloadReceipt {
  int (*outstanding)();
  void *mpvAddress;
};
