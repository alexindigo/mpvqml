#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "faults.h"
#include "unload-consumer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QtTest>
#include <clocale>
#include <dlfcn.h>
#include <link.h>

// Intentionally not linked with libmpvqml, MpvQt, or libmpv. The only load of
// these modules is the disposable test consumer's DT_NEEDED chain.
namespace {
bool mapped(void *base) {
  struct Probe { void *base; bool found; } probe{base, false};
  dl_iterate_phdr([](dl_phdr_info *info, size_t, void *context) {
    auto &probe = *static_cast<Probe *>(context);
    if (reinterpret_cast<void *>(info->dlpi_addr) == probe.base) {
      probe.found = true;
      return 1;
    }
    return 0;
  }, &probe);
  return probe.found;
}

struct Resources {
  void *consumer = nullptr;
  decltype(&mpvqml_fault_release) release = nullptr;
  ~Resources() {
    if (release) release();
    if (consumer) dlclose(consumer);
  }
};

#define CHECK(condition, explanation) \
  do { if (!(condition)) { qCritical() << explanation; return 1; } } while (false)
} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  Resources resources;
  auto configure = reinterpret_cast<decltype(&mpvqml_fault_configure)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_configure"));
  auto counter = reinterpret_cast<decltype(&mpvqml_fault_counter)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_counter"));
  auto bindScope = reinterpret_cast<decltype(&mpvqml_fault_bind_scope)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_bind_scope"));
  resources.release = reinterpret_cast<decltype(resources.release)>(dlsym(RTLD_DEFAULT, "mpvqml_fault_release"));
  CHECK(configure && counter && bindScope && resources.release, "fault fixture missing; NOT_EXERCISED");
  CHECK(std::setlocale(LC_NUMERIC, "C"), "numeric locale setup failed");
  configure(FaultRetirementBarrier, 0);
  resources.consumer = dlopen(MPVQML_UNLOAD_CONSUMER, RTLD_NOW | RTLD_LOCAL);
  CHECK(resources.consumer, dlerror());
  CHECK(bindScope(resources.consumer) == 0, "cannot bind verified local libmpv forwarding scope");
  auto start = reinterpret_cast<int (*)(MpvQmlUnloadReceipt *)>(dlsym(resources.consumer, "mpvqml_test_start_blocked_retirement"));
  CHECK(start, "test consumer entrypoint missing");
  Dl_info consumer{}, backend{}, mpv{};
  CHECK(dladdr(reinterpret_cast<void *>(start), &consumer), "consumer module identity unavailable");
  MpvQmlUnloadReceipt receipt{};
  CHECK(start(&receipt) == 0, "production retirement setup failed");
  CHECK(receipt.outstanding && receipt.mpvAddress, "production receipt missing");
  CHECK(dladdr(reinterpret_cast<void *>(receipt.outstanding), &backend), "backend identity unavailable");
  CHECK(dladdr(receipt.mpvAddress, &mpv), "libmpv identity unavailable");
  CHECK(consumer.dli_fbase != backend.dli_fbase && backend.dli_fbase != mpv.dli_fbase,
        "test consumer/backend/libmpv identities must be distinct");
  const QString backendPath = QString::fromLocal8Bit(backend.dli_fname);
  const QString mpvPath = QString::fromLocal8Bit(mpv.dli_fname);
  QElapsedTimer deadline;
  deadline.start();
  while (counter(FaultRetirementEntered) != 2 && deadline.elapsed() < 5000)
    QTest::qWait(5);
  CHECK(counter(FaultRetirementEntered) == 2 && counter(FaultRetirementCompleted) == 0,
        "both native workers must be blocked before unload");
  CHECK(receipt.outstanding() == 2, "both handles must remain leased before unload");
  CHECK(dlclose(resources.consumer) == 0, "consumer dlclose failed");
  resources.consumer = nullptr;
  CHECK(!mapped(consumer.dli_fbase), "disposable consumer did not unload; NOT_EXERCISED");
  CHECK(mapped(backend.dli_fbase) && mapped(mpv.dli_fbase), "active retirement modules were unloaded");
  CHECK(receipt.outstanding() == 2, "unload dropped live retirement ownership");
  int heartbeat = 0;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&heartbeat]() { ++heartbeat; });
  timer.start(5);
  CHECK(QTest::qWaitFor([&heartbeat]() { return heartbeat >= 3; }, 1000),
        "GUI heartbeat did not progress within the ownership-fixture deadline");
  CHECK(heartbeat >= 3 && counter(FaultRetirementCompleted) == 0, "GUI heartbeat failed while unload retirement was blocked");
  resources.release();
  deadline.restart();
  while ((counter(FaultRetirementCompleted) != 2 || receipt.outstanding() != 0)
         && deadline.elapsed() < 5000)
    QTest::qWait(5);
  CHECK(counter(FaultRetirementCompleted) == 2 && receipt.outstanding() == 0,
        "unloaded-consumer retirements were not reclaimed");
  qInfo() << "consumer unmapped; both native retirements reclaimed; heartbeat ticks" << heartbeat
          << "backend" << backendPath << "libmpv" << mpvPath;
  qInfo().noquote() << "FAULT_CASE_EXERCISED moduleUnloadWhileBlocked";
  return 0;
}
