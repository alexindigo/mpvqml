# Verification lanes

Build with `-DBUILD_TEST_MODE=ON` (Qt Test, Network, QML tools and Python 3
are needed). In the owned project feature VM, set `TMPDIR` to disk-backed
workspace scratch and set `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` to its live
Wayland session. Run `scripts/run-tests <build>`. For coverage only, configure
`-DENABLE_COVERAGE=ON` and add `--coverage` to the runner (lcov and genhtml).
Ordinary CTest execution does not require coverage tools.

| CTest label | What the lane observes | Environment |
|---|---|---|
| `headless` | Core/retirement ownership, registered capture type, real first/seek frames | No display, offscreen Qt, software Qt Quick |
| `native` | Real renderer/fault/desktop GL tests, helper and facade tests | Wayland, Qt Quick OpenGL, software GL in the VM |
| `mock` | C++ facade seam suite; this suite still needs the native Qt window environment | Subset of `native`, not rendering evidence |
| `qml` | Standalone declarative API suite using the configured Qt qmltestrunner | Subset of `native` |
| `installed` | Relocated QML and C++ consumers; capture runs separately with no display | Wayland for the legacy property read; see production test-off recipe in the root README |

Missing required cases, QtTest skips and a missing desktop GL readback are
not passing results. The crash suite launches the non-C locale proof in an
isolated child; provide a generated non-C numeric locale (e.g. `de_DE.UTF-8`).
CTest reports elapsed times by lane. Expect seconds for the standalone QML
and relocated consumer cases; configure/build times are separate.

## Fixtures

Checked-in synthetic assets need no generation during testing. To regenerate
their semantics, use `scripts/generate-assets <scratch-dir>` with FFmpeg
(H.264, VP9, MP3, Vorbis, PNG and JPEG encoders); encoder versions may change
bytes. `test_frame_seek.webm` is 320x180 VP9 at 10 fps, red from 0 to 1 s and
blue from 1 to 2 s. The real and installed capture tests sample the center
at start and 75% (about 1.5 s), require 123x69 output for a 123x88 bound, and
use a dominant color threshold greater than 210 with the other channels below
30 in the installed probe rather than assuming bit-identical conversion.

`test_frame_rotated_{90,180,270}.mp4` have the same 160x90 H.264 stream
(red/blue/lime/yellow quadrants) but distinct display matrices. To regenerate
in the VM's scratch directory:

```sh
ffmpeg -f lavfi -i 'color=blue:s=160x90:r=5:d=1,drawbox=x=0:y=0:w=80:h=45:color=red:t=fill,drawbox=x=0:y=45:w=80:h=45:color=lime:t=fill,drawbox=x=80:y=45:w=80:h=45:color=yellow:t=fill' \
  -c:v libx264 -pix_fmt yuv420p rotation-landscape.mp4
for angle in 90 180 270; do
  ffmpeg -display_rotation "$angle" -i rotation-landscape.mp4 -map 0:v -c:v copy "test_frame_rotated_$angle.mp4"
done
```

The standalone QML scans stage their own explicit media set and modification
times, so adding a file to the shared asset catalog cannot change the playlist
under test. The fallback case checks exact identities in newest-first order,
including WebM and excluding Ogg and PNG.
