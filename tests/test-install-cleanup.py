#!/usr/bin/env python3
"""Interrupt the real install-test permission transaction at a controlled boundary."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time

from termination_guard import unwind_on_termination


def worker(build, ready):
    source = Path(__file__).with_name("install-convergence.py")
    spec = importlib.util.spec_from_file_location("mpvqml_install_test", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    real_run = module.subprocess.run

    def controlled_consumer(argv, **kwargs):
        if Path(argv[0]).name == "module-consumer":
            # Install, readelf and compilation are real. Pause only the consumer
            # boundary so the parent can signal while build access is denied.
            ready.write_text("build access denied\n")
            time.sleep(60)
        return real_run(argv, **kwargs)

    module.subprocess.run = controlled_consumer
    sys.argv = [str(source), "--build", str(build)]
    module.main()


def interrupted_install(build, signum):
    original_mode = stat.S_IMODE(build.stat().st_mode)
    if original_mode == 0 or build.stat().st_uid != os.getuid():
        raise RuntimeError("requires an accessible user-owned generated build")
    process = None
    with tempfile.TemporaryDirectory(prefix="install-signal-", dir=build.parent) as scratch:
        ready = Path(scratch) / "ready"
        try:
            process = subprocess.Popen(
                [sys.executable, "-B", str(Path(__file__).resolve()), "--build", str(build),
                 "--worker-ready", str(ready)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.monotonic() + 40
            while not ready.exists():
                if process.poll() is not None:
                    raise RuntimeError("worker exited before the protected phase: " + process.communicate()[0])
                if time.monotonic() >= deadline:
                    raise RuntimeError("worker did not reach the protected phase")
                time.sleep(0.02)
            denied = stat.S_IMODE(build.stat().st_mode)
            if denied != 0:
                raise RuntimeError("consumer boundary did not observe an inaccessible build")
            os.kill(process.pid, signum)
            output = process.communicate(timeout=10)[0]
            after_mode = stat.S_IMODE(build.stat().st_mode)
            try:
                receipt = json.loads(output)
            except ValueError:
                receipt = {}
            passed = (process.returncode == 128 + signum and after_mode == original_mode
                      and receipt.get("status") == "FAIL" and receipt.get("build_mode_restored") is True)
            return {"signal": signal.Signals(signum).name, "exit": process.returncode,
                    "denied_mode": denied, "original_mode": original_mode, "after_mode": after_mode,
                    "passed": passed, "install_receipt": receipt}
        finally:
            # A negative run intentionally exercises a worker without handlers.
            # Repair only this test's owned build after recording its failure.
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
            build.chmod(original_mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--worker-ready", type=Path)
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    if args.worker_ready:
        # The worker must use the install script's own handlers, not the
        # harness's guard, or the pre-fix negative run would falsely pass.
        worker(build, args.worker_ready)
        return 0
    signals = (signal.SIGTERM, signal.SIGHUP)
    previous = {signum: signal.getsignal(signum) for signum in signals}
    with unwind_on_termination():
        results = [interrupted_install(build, signum) for signum in signals]
    restored = all(signal.getsignal(signum) == previous[signum] for signum in signals)
    print(json.dumps({"cases": results, "handlers_restored": restored}, indent=2))
    return 0 if restored and all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
