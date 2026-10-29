#pragma once

/* Child-only fault fixture ABI. Never compiled into the product module. */
enum MpvQmlFault {
  FaultCreate = 1,
  FaultInitialize = 2,
  FaultRetirementBarrier = 4,
  FaultTlsBarrier = 8,
  FaultRenderCreate = 16,
  FaultFileBarrier = 32,
  FaultSynchronousWakeup = 64
};

enum MpvQmlFaultCounter {
  FaultCreates,
  FaultInitializes,
  FaultCandidateDestroys,
  FaultThreadAttempts,
  FaultThreadFailures,
  FaultBootstrapThreadsFinished,
  FaultRetirementEntered,
  FaultRetirementCompleted,
  FaultTlsEntered,
  FaultTlsCompleted,
  FaultRenderCreates,
  FaultFileOpens,
  FaultFileWrites,
  FaultFileWritesCompleted,
  FaultSynchronousRegistrations,
  FaultHelperClients,
  FaultHelperAddressReuses,
  FaultHelperEmptyDrains,
  FaultCounterCount
};

#ifdef __cplusplus
extern "C" {
#endif
void mpvqml_fault_configure(int faults, int fail_thread);
long mpvqml_fault_counter(int counter);
void mpvqml_fault_release(void);
int mpvqml_fault_seed_tls(void);
int mpvqml_fault_bind_scope(void *consumer);
int mpvqml_fault_watch_file(const char *path);
#ifdef __cplusplus
}
#endif
