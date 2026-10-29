#!/usr/bin/env bash
set -euo pipefail

root=""
lane=debug
cases=""
while (($#)); do
    case "$1" in
        --root) root=$2; shift 2 ;;
        --lane) lane=$2; shift 2 ;;
        --cases) cases=$2; shift 2 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ -n "$root" && -n "$cases" && ( "$lane" == debug || "$lane" == asan-ubsan ) ]] || exit 2
systemd-detect-virt --quiet --vm || { printf 'VM-only driver: NOT_EXERCISED\n' >&2; exit 2; }
[[ -d "$root/results" && -f "$root/run/guest-session.env" ]] || exit 2
# shellcheck disable=SC1091
source "$root/run/guest-session.env"
unset LD_PRELOAD ASAN_OPTIONS UBSAN_OPTIONS TSAN_OPTIONS QT_QUICK_BACKEND
unset QT_QPA_GENERIC_PLUGINS AERIAL_LOCK_CONNECTION_GUARD_PID AERIAL_LOCK_CONNECTION_GUARD_OWNER_PID QT_WAYLAND_RECONNECT
export LIBGL_ALWAYS_SOFTWARE=1 QSG_RHI_BACKEND=opengl QT_FORCE_STDERR_LOGGING=1
build="$root/build/$lane/mpvqml"
export LD_LIBRARY_PATH="$build" QML_IMPORT_PATH="$build"
if [[ "$lane" == asan-ubsan ]]; then
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
fi
ulimit -c 0
out="$root/results/native-$lane-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir "$out"
readelf -d "$build/libmpvqml.so" > "$out/linkage.txt"
ninja -C "$build" -t deps > "$out/compile-dependencies.txt"
python - "$out" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
assert "libMpvQt" not in (root / "linkage.txt").read_text(), "external MpvQt linkage"
assert "/MpvQt/" not in (root / "compile-dependencies.txt").read_text(), "external MpvQt header dependency"
print("OWNED_BACKEND_LINK_AND_HEADER_PROVENANCE")
PY

run_case() {
    case "$1" in
        core-failure)
            "$build/mpvqml_crash_tests" nativeFaults:coreCreationFailure nativeFaults:coreInitializationFailure \
                nativeFaults:partialRetirementBootstrapFailure nativeFaults:workerStartupFailure \
                nativeFaults:publicCoreCreationFailure nativeFaults:publicCoreInitializationFailure \
                nativeFaults:publicWorkerStartupFailure -v1 ;;
        render-failure)
            "$build/mpvqml_crash_tests" nativeFaults:publicRenderFailure nativeFaults:unsupportedGraphicsBackend -v1 ;;
        node-ownership|flags)
            "$build/mpvqml_controller_tests" -v1 ;;
        log-reentry|helper-lifetime)
            "$build/mpvqml_helpers_tests" -v1 ;;
        callback-lifetime)
            "$build/mpvqml_retirement_tests" redrawPostingCoalescesWhileGuiIsBlocked -v1
            "$build/mpvqml_crash_tests" nativeFaults:synchronousHelperRegistration \
                nativeFaults:recycledHelperAddressRejectsOldWakeups -v1 ;;
        screenshot-retirement)
            "$build/mpvqml_crash_tests" nativeFaults:ordinaryFileScreenshotRetirement \
                nativeFaults:fifoScreenshotRetirement nativeFaults:bothWorkersBlockedAndAllLiveLeasesReclaimed \
                nativeFaults:workerTlsCleanupOutlivesFacade nativeFaults:moduleUnloadWhileBlocked -v1 ;;
        early-stop)
            "$build/mpvqml_facade_tests" earlyStopCancelsDeferredSourceAndShutdownNeverFlushesIt \
                shutdownSettlesAdmissionsWithoutLateRevival -v1 ;;
        renderer-recreation)
            "$build/mpvqml_renderer_tests" -v1 ;;
        *) printf 'Unknown case %s: NOT_EXERCISED\n' "$1" >&2; return 2 ;;
    esac
}
IFS=, read -r -a selected <<< "$cases"
((${#selected[@]})) || exit 2
for case_name in "${selected[@]}"; do
    run_case "$case_name" 2>&1 | tee "$out/$case_name.log"
    printf 'EXERCISED %s\n' "$case_name" | tee -a "$out/summary.txt"
done
printf 'Results: %s\n' "$out"
