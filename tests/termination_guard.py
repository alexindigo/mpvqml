"""Scoped signal unwinding for tests that temporarily change external state."""
from contextlib import contextmanager
import signal


@contextmanager
def unwind_on_termination():
    previous = {}

    def terminate(signum, _frame):
        raise SystemExit(128 + signum)

    try:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            previous[signum] = signal.signal(signum, terminate)
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
