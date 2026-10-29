#!/usr/bin/env python3
"""Test a relocated installation with the generated build tree inaccessible."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    source = Path(__file__).resolve().parent
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(source.parent) + "\n" not in cache:
        raise RuntimeError("build is not from this source tree")
    if build.stat().st_uid != os.getuid() or build == source.parent:
        raise RuntimeError("only a user-owned, out-of-source build may be made inaccessible")

    def install_dir(name):
        match = re.search(r"^CMAKE_INSTALL_" + name + r":PATH=(.+)$", cache, re.MULTILINE)
        value = Path(match.group(1)) if match else None
        if value is None or value.is_absolute() or ".." in value.parts:
            raise RuntimeError("test requires a prefix-relative CMAKE_INSTALL_" + name)
        return value

    libdir = install_dir("LIBDIR")
    includedir = install_dir("INCLUDEDIR")
    # Stage under the generated build, then relocate out before denying access
    # to the entire build directory. Consumers and logs must remain reachable.
    staging = Path(tempfile.mkdtemp(prefix="install-destdir-", dir=build))
    out = Path(tempfile.mkdtemp(prefix=build.name + "-installed-", dir=build.parent))
    prefix = out / "prefix"
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
                "QT_QPA_GENERIC_PLUGINS", "QT_PLUGIN_PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS",
                "DESTDIR"):
        env.pop(key, None)
    env.update(LIBGL_ALWAYS_SOFTWARE="1", QSG_RHI_BACKEND="opengl",
               QT_FORCE_STDERR_LOGGING="1", QML_IMPORT_PATH=str(prefix / libdir / "qt6/qml"))
    commands = []

    def run(argv, name, child_env=env):
        argv = [str(value) for value in argv]
        result = subprocess.run(argv, env=child_env, capture_output=True, text=True, timeout=90)
        commands.append({"argv": argv, "exit": result.returncode})
        (out / (name + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + " failed; see " + str(out / (name + ".log")))
        return result.stdout + result.stderr

    receipt = {"status": "INCOMPLETE", "prefix": str(prefix), "build": str(build),
               "destdir": str(staging), "source": str(source.parent)}
    original_mode = stat.S_IMODE(build.stat().st_mode)
    inaccessible = False
    try:
        run(["cmake", "--install", build, "--prefix", "/mpvqml-test"], "install",
            {**env, "DESTDIR": str(staging)})
        shutil.move(str(staging / "mpvqml-test"), prefix)
        staging.rmdir()
        plugin = prefix / libdir / "qt6/qml/MpvQml/libmpvqmlplugin.so"
        dynamic = run(["readelf", "-d", plugin], "plugin-elf")
        runpaths = re.findall(r"\(RUNPATH\).*?\[(.*?)\]", dynamic)
        if runpaths != ["$ORIGIN/../../.."] or "libMpvQt" in dynamic:
            raise RuntimeError("installed plugin must have only the prefix-relative RUNPATH")
        backing = run(["readelf", "-d", prefix / libdir / "libmpvqml.so"], "backing-elf")
        if "libMpvQt" in backing or str(build) in backing:
            raise RuntimeError("installed backing-library dependency/path mismatch")
        pkg = run(["pkg-config", "--cflags", "--libs", "Qt6Core", "Qt6Gui", "Qt6Qml", "Qt6Quick", "Qt6Test"], "qt-flags")
        flags = shlex.split(pkg)
        module_consumer = out / "module-consumer"
        header_consumer = out / "header-consumer"
        run(["c++", "-std=c++17", "-fPIC", source / "install-module-consumer.cpp", "-o", module_consumer,
             *flags], "module-consumer-build")
        run(["c++", "-std=c++17", "-fPIC", source / "installed-consumer.cpp", "-o", header_consumer,
             "-I" + str(prefix / includedir), "-L" + str(prefix / libdir),
             "-Wl,-rpath," + str(prefix / libdir), "-lmpvqml", *flags], "header-consumer-build")
        build.chmod(0)
        inaccessible = True
        if os.access(build, os.R_OK | os.X_OK):
            raise RuntimeError("build remains accessible; run as an unprivileged user")
        module = run([module_consumer, prefix, build, libdir], "installed-module")
        header = run([header_consumer], "installed-header")
        if "INSTALLED_MODULE_OWNED_READ" not in module or "INSTALLED_PLUGIN_MAP" not in module \
                or "INSTALLED_BACKING_MAP" not in module or "INSTALLED_CONSUMER_OWNED_READ" not in header:
            raise RuntimeError("actual installed consumer outcomes missing")
        receipt.update(status="PASS", relative_plugin_runpath=True, build_inaccessible=True,
                       installed_plugin_backing_maps=True, allocated_async_read=True,
                       installed_public_header_read=True, clean_library_path_environment=True,
                       libraries=[str(path.relative_to(prefix)) for path in sorted((prefix / libdir).glob("libmpvqml.so*"))])
    except Exception as error:
        receipt.update(status="FAIL", error=str(error))
        raise
    finally:
        if inaccessible:
            build.chmod(original_mode)
        receipt["build_mode_restored"] = stat.S_IMODE(build.stat().st_mode) == original_mode
        receipt["commands"] = commands
        (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(json.dumps({"results": str(out), **receipt}, indent=2))


if __name__ == "__main__":
    main()
