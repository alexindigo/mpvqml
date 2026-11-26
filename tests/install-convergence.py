#!/usr/bin/env python3
"""Test a relocated installation with product source and build trees inaccessible."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

from termination_guard import unwind_on_termination


@unwind_on_termination()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--config", default="")
    parser.add_argument("--source-revision", help="Full commit ID of the delivered candidate")
    args = parser.parse_args()
    if args.source_revision and not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", args.source_revision):
        parser.error("--source-revision must be a full git commit ID")
    build = args.build.resolve(strict=True)
    source = Path(__file__).resolve().parent.parent
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(source) + "\n" not in cache:
        raise RuntimeError("build is not from this source tree")
    if build == source or build in source.parents \
            or any(path.stat().st_uid != os.getuid() for path in (source, build)):
        raise RuntimeError("requires a user-owned source tree and a separate owned build directory")

    def install_dir(name):
        match = re.search(r"^CMAKE_INSTALL_" + name + r":PATH=(.+)$", cache, re.MULTILINE)
        value = Path(match.group(1)) if match else None
        if value is None or value.is_absolute() or ".." in value.parts:
            raise RuntimeError("test requires a prefix-relative CMAKE_INSTALL_" + name)
        return value

    libdir = install_dir("LIBDIR")
    includedir = install_dir("INCLUDEDIR")
    # Stage in the product build, then move out of both product trees. This
    # also works for the conventional out-of-source build at source/build.
    staging = Path(tempfile.mkdtemp(prefix="install-destdir-", dir=build))
    parent = source.parent if source in build.parents else build.parent
    out = Path(tempfile.mkdtemp(prefix=build.name + "-installed-", dir=parent))
    prefix = out / "prefix"
    external = out / "external"
    external.mkdir()
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
                "QT_QPA_GENERIC_PLUGINS", "QT_PLUGIN_PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS",
                "DESTDIR"):
        env.pop(key, None)
    env.update(LIBGL_ALWAYS_SOFTWARE="1", QSG_RHI_BACKEND="opengl", QT_FORCE_STDERR_LOGGING="1")
    commands = []

    def run(argv, name, child_env=env):
        argv = [str(value) for value in argv]
        result = subprocess.run(argv, cwd=out, env=child_env, capture_output=True, text=True, timeout=120)
        commands.append({"argv": argv, "exit": result.returncode})
        (out / (name + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + " failed; see " + str(out / (name + ".log")))
        return result.stdout + result.stderr

    receipt = {"status": "INCOMPLETE", "prefix": str(prefix), "build": str(build),
               "destdir": str(staging), "source": str(source), "source_commit": args.source_revision}
    modes = {path: stat.S_IMODE(path.stat().st_mode) for path in (source, build)}
    denied = []
    try:
        config = ["--config", args.config] if args.config else []
        run(["cmake", "--install", build, "--prefix", "/mpvqml-test", *config], "install",
             {**env, "DESTDIR": str(staging)})
        shutil.move(str(staging / "mpvqml-test"), prefix)
        staging.rmdir()
        test_mode = re.search(r"^BUILD_TEST_MODE:BOOL=(.+)$", cache, re.MULTILINE)[1]
        if (prefix / libdir / "qt6/qml/MpvQml/TestSupport").exists() \
                or (prefix / includedir / "mpvqml/mpvframecapture.h").exists():
            raise RuntimeError("test support or the private C++ capture header leaked into the API")
        plugin = prefix / libdir / "qt6/qml/MpvQml/libmpvqmlplugin.so"
        dynamic = run(["readelf", "-d", plugin], "plugin-elf")
        runpaths = re.findall(r"\(RUNPATH\).*?\[(.*?)\]", dynamic)
        if runpaths != ["$ORIGIN/../../.."] or "libMpvQt" in dynamic:
            raise RuntimeError("installed plugin must have only the prefix-relative RUNPATH")
        backing = run(["readelf", "-d", prefix / libdir / "libmpvqml.so"], "backing-elf")
        if "libMpvQt" in backing or str(build) in backing or str(source) in backing:
            raise RuntimeError("installed backing-library dependency/path mismatch")
        for name in ("install-module-consumer.cpp", "installed-consumer.cpp", "installed-mappings.h"):
            shutil.copyfile(source / "tests" / name, external / name)
        shutil.copyfile(source / "tests/install-consumers.cmake", external / "CMakeLists.txt")
        video = out / "test_frame_seek.webm"
        shutil.copyfile(source / "tests/assets" / video.name, video)
        # Arm cleanup before each chmod. Reopen source first if it contains build.
        for path in (build, source):
            denied.append(path)
            path.chmod(0)
        if any(os.access(path, os.R_OK | os.X_OK) for path in denied):
            raise RuntimeError("product trees remain accessible; run as an unprivileged user")

        compiled = out / "compiled"
        run(["cmake", "-S", external, "-B", compiled, f"-DPROBE_PREFIX={prefix}"], "consumer-configure")
        run(["cmake", "--build", compiled, "--parallel", "4"], "consumer-build")
        lib = prefix / libdir
        module_consumer = compiled / "module-consumer"
        module_elf = run(["readelf", "-d", module_consumer], "consumer-elf")
        if "libmpvqml" in module_elf:
            raise RuntimeError("QML-only probe must not link to the backing library")
        module = run([module_consumer, lib, source, build], "installed-module")
        header = run([compiled / "header-consumer", lib, source, build], "installed-header")
        no_display = {**env, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software"}
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "QSG_RHI_BACKEND"):
            no_display.pop(key, None)
        capture = run([module_consumer, lib, source, build, video, out], "installed-capture", no_display)
        maps = ("INSTALLED_PLUGIN_MAP ", "INSTALLED_BACKING_MAP ")
        if not all(marker in module and marker in capture for marker in maps) \
                or "INSTALLED_CONSUMER_OWNED_READ " not in header or maps[1] not in header \
                or "INSTALLED_CAPTURE_PIXEL initial 123x69 " not in capture \
                or "INSTALLED_CAPTURE_PIXEL seek 123x69 " not in capture:
            raise RuntimeError("actual installed consumer outcomes missing")
        version = re.search(r"^INSTALLED_MODULE_OWNED_READ id=\d+ value=(.+)$", module, re.MULTILINE)
        if not version:
            raise RuntimeError("missing installed mpv-version read")
        qt = run(["pkg-config", "--modversion", "Qt6Core"], "qt-version").strip()
        mpv = run(["pkg-config", "--modversion", "mpv"], "mpv-pc-version").strip()
        receipt.update(status="PASS", build_test_mode=test_mode, config=args.config,
                       install_mode="DESTDIR then relocated prefix", platform=os.uname().sysname,
                       machine=os.uname().machine, qt_version=qt, libmpv_pkg_config_version=mpv,
                       mpv_version=version[1], relative_plugin_runpath=True,
                       source_inaccessible=True, build_inaccessible=True,
                        installed_plugin_backing_maps=True, allocated_async_read=True,
                        installed_public_header_read=True, exported_cmake_target="mpvqml::mpvqml",
                        clean_library_path_environment=True,
                        headless_timeline_pixels=re.findall(r"^INSTALLED_CAPTURE_PIXEL (.+)$", capture, re.MULTILINE),
                       libraries=[str(path.relative_to(prefix)) for path in sorted((prefix / libdir).glob("libmpvqml.so*"))])
    except BaseException as error:
        receipt.update(status="FAIL", error=str(error))
        raise
    finally:
        for path in reversed(denied):
            path.chmod(modes[path])
        for key, path in (("source", source), ("build", build)):
            receipt[key + "_mode_restored"] = stat.S_IMODE(path.stat().st_mode) == modes[path]
        receipt["commands"] = commands
        (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(json.dumps({"results": str(out), **receipt}, indent=2))


if __name__ == "__main__":
    main()
