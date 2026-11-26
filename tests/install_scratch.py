"""Shared scratch boundary for the install permission probes."""

from pathlib import Path
import tempfile


def scratch_parent(source, build):
    parent = Path(tempfile.gettempdir()).resolve()
    if any(parent == path or path in parent.parents for path in (source, build)):
        raise RuntimeError("set TMPDIR to a writable directory outside the source and build trees")
    return parent
