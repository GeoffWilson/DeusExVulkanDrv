# Building VulkanDrv for Deus Ex from Linux

The Visual Studio solution stays the canonical build. This CMake project builds
the same `DeusExRelease` configuration with clang-cl, so the Deus Ex port can be
compiled and iterated on without Windows.

## One-time setup

The MSVC CRT and the Windows SDK are downloaded from Microsoft with
[`xwin`](https://github.com/Jake-Shadle/xwin):

```sh
cargo install xwin --locked
xwin --accept-license --arch x86 --cache-dir ../.xwin-cache splat --output ../.xwin
```

`../.xwin` (next to the repository) is where the toolchain file looks by
default; pass `-DXWIN_DIR=/some/path` to override. The path tracer's helper is
64-bit, so it needs the x64 libraries as well, in a folder of their own that
`xwin-clang-cl-x64.cmake` looks for:

```sh
xwin --accept-license --arch x86_64 --cache-dir ../.xwin-cache splat --output ../.xwin-x64
```

Also needed: `clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`, `cmake`, `ninja`
and `python3`.

## Building

```sh
cmake -S . -B build-deusex -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/xwin-clang-cl-x86.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-deusex -j8
```

The result is `build-deusex/VulkanDrv.dll`, `PathTracerDrv.dll`, `D3D11Drv.dll`
and `D3D12Drv.dll`, 32 bit DLLs linked against the Deus Ex 1112f import
libraries in `Thirdparty/DeusEx`. PathTracerDrv has no Visual Studio project,
so this is its only build.

PathTracerDrv traces in `PathTracerHelper.exe`, a 64-bit program it starts
beside itself (see the main README for why). A 64-bit configuration of the same
project builds that and nothing else:

```sh
cmake -S . -B build-x64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/xwin-clang-cl-x64.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-x64 --target PathTracerHelper
```

`cmake --build build-deusex --target PathTracerHelperTest` builds a 32-bit
program that drives the helper with a made up scene, as the device would, and
writes the frame it gets back to `helper-test.ppm`: a check of the helper and
the channel to it without the game. Put it beside `PathTracerHelper.exe` and
run `wine PathTracerHelperTest.exe [frames] [width] [height]`. `--dlss 0`
to `--dlss 4` denoises with DLSS Ray Reconstruction at that quality instead of
NRD, and `--still` holds the scene still and reports how much the picture
changes from frame to frame - noise and shimmer, which a wrong sub-pixel
jitter shows up as. `--reference` holds it still with no denoiser, so the frames
average towards what the trace converges on - what a denoiser should arrive
at - and `--backlight` puts the light behind the red box, so its shadow runs
towards the camera; `--lightsize n` casts shadows from a light of that
radius; `--detail` gives the floor a striped detail texture; `--bc1` sends
its checker as S3TC blocks, as New Vision's textures go; `--engine-lighting`
lights it as the device's default `Lighting=Engine` does, where the harness
otherwise keeps to the linear lighting its scene was made for; with it,
`--baked-mask` bakes the light into the floor's lightmap with a shadow mask
covering only the floor's left half, which the right half should go without.
`--inset` adds a second view, as a security camera's in a window of the HUD,
traced from the right of the box into the picture's top right corner.
`--decal` lays a scorch mark on the floor as the device lays a decal - a
modulated quad a quarter of a unit off it, mid grey round a dark blot - of
which only the blot should show; `--decal-lift n` raises it, so that the
light's shadow rays have to cross it. `--lights n` crowds the scene with n
more lights, each reaching all of it, as the Wan Chai canal's neon crowds a
cell of the light grid, and `--every-light` has the trace weigh every one of
them rather than drawing from them (`PT ALLLIGHTS`): with `--reference`, the
two converge on the same picture, and at 3440x1440 the difference in the
trace's time is what the drawing saves. The harness's own GPU times swing
with the GPU's clocks, which drop while it waits on wine: take the fastest of
many frames, not any one. `--hdr n` finishes the picture for an HDR display
whose peak is n times the SDR white and says how much went above it, and
`--neutral` takes the device's neutral tone curve rather than Reinhard's: with
both, everything below the curve's shoulder should come out exactly as
`--neutral` alone gives it. `--sprites n` hangs a plume of n overlapping
puffs of smoke before the box, drawn as the device draws sprites: however
many overlap, their edges should never show, and the scene behind should come
out as it does without them, with the smoke added. `--headset` traces for
the helper's simulated headset rather than the screen - two eyes 64 mm apart,
each seeing further out than in, the head turning slowly - and writes the two
eyes side by side, while a test HUD, a crosshair and a translucent bar, goes
back every frame as the device's does; the helper also writes
`headset-left.ppm`, `headset-right.ppm` and `headset-hud.ppm` beside itself,
what a real headset would have been handed, the HUD's colour over grey
beside its coverage. The Windows SDK that `xwin` fetches carries the Direct3D
headers and import libraries, so the two Direct3D devices need nothing extra;
their OpenXR support does, and is stubbed out (see `D3D11DRV_OPENXR`).

### The path tracer's denoiser

The path tracer's helper denoises with NVIDIA's NRD when it is there to link.
Build it once, before configuring:

```sh
cmake/build-nrd.sh          # into ../.nrd, beside .xwin
```

The script fetches a pinned NRD release (its licence keeps its source out of
this repository), compiles NRD's HLSL shaders to SPIR-V with a DXC it downloads
into `../.nrd`, and cross-builds `x64/NRD.lib`, which the helper links, with the
x64 xwin CRT (and `x86/NRD.lib` with the 32 bit one). Configure `build-x64`
afterwards and CMake reports `PathTracerHelper: denoising with NRD`; without it
the helper builds without the denoiser and `PT DENOISE` says it is missing.
`Denoise=False` in the device's ini section turns it off at startup.

### FSR 3.1

For GPUs Ray Reconstruction cannot run on, the helper can upscale NRD's
picture with AMD's FSR 3.1, built in from FidelityFX SDK v1.1.4 - the last
with a Vulkan backend; the FSR 4 SDKs are DirectX 12 only. Fetch it once,
before configuring:

```sh
cmake/fetch-fsr.sh          # into ../.fsr, beside .xwin; needs wine
```

The script takes the SDK's `sdk` folder alone, at the pinned tag, and runs
its shader compiler (`FidelityFX_SC.exe`, under wine) over FSR 3.1's ten
passes as the SDK's own build does, into `../.fsr/generated`. Configure
`build-x64` (and `build-ut469-x64`) afterwards and CMake reports
`PathTracerHelper: FSR 3.1`. The upscaler, the Vulkan backend and what they
share are built as `ffx_fsr3upscaler.lib`, against ZVulkan's Vulkan headers,
with `volk/volk.h` forced in so the backend's own Vulkan calls go through
volk. `PathTracerHelperTest --fsr quality` runs it on any GPU.

### DLSS Ray Reconstruction

The helper can also denoise with NVIDIA's DLSS Ray Reconstruction, when the
DLSS SDK is there to link. Fetch it once, before configuring:

```sh
cmake/fetch-dlss.sh         # into ../.dlss, beside .xwin
```

The script downloads a pinned release's headers, NGX's static loader
(`x64/nvsdk_ngx_s.lib`, static CRT like the helper) and Ray Reconstruction's
runtime (`nvngx_dlssd.dll`, 48 MB), and nothing else. Configure `build-x64`
afterwards and CMake reports `PathTracerHelper: DLSS Ray Reconstruction`; the
build copies `nvngx_dlssd.dll` beside `PathTracerHelper.exe` and
`deploy-deusex.sh` installs it with the helper, since NGX looks for it there.
`cmake --build build-x64 --target ngxcheck` builds the spike that asks whether
it runs at all on a given setup; see `spike/README.md`.

### Headsets

The helper shows the game in a headset through OpenXR (the device's `VR`
setting) when Khronos's loader and headers are there. Fetch them once,
before configuring:

```sh
cmake/fetch-openxr.sh       # into ../.openxr, beside .xwin
```

The script takes the pinned release's Windows loader, `openxr_loader.dll`
(64 bit, static CRT), and its headers, and nothing else. Configure `build-x64`
afterwards and CMake reports `PathTracerHelper: headsets through OpenXR`; the
build copies the loader beside the helper, which opens it only when a headset
is asked for. Without it the helper builds without headsets, and `PT VR`
says so.

### Unreal Tournament 469

`PATHTRACER_GAME=UT469` builds PathTracerDrv for Unreal Tournament with
OldUnreal's 469 patch instead, against the patch's SDK
(`OldUnreal-UTPatch469f-SDK-Windows.zip`, from the
[release](https://github.com/OldUnreal/UnrealTournamentPatches/releases)),
unpacked beside this repository or wherever `UT469_SDK` points. It is not
in this repository: its headers are Epic's, under the Unreal licence.

```sh
cmake -S . -B build-ut469-x64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/xwin-clang-cl-x64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DPATHTRACER_GAME=UT469 \
      -DUT469_SDK=../UT469fSDK
cmake --build build-ut469-x64 --target PathTracerDrv
```

The 64-bit game needs no helper: this builds the tracing, NRD and NGX into
`PathTracerDrv.dll` itself, and copies `nvngx_dlssd.dll` beside it.
`cmake/deploy-ut.sh "" pathtracer` installs both, and the `.int`, into the
Steam install and selects the device (`none` leaves the ini alone).
`--target PathTracerLocalTest` builds the harness above traced the same way,
in its own process on its own device, with the same options.

Two things clang-cl needs that MSVC does not: `__XNAMATHVECTOR_INL__`, the
guard 469's `UnX86.h` skips its operators on SSE vectors for, which clang
already has; and an `/alternatename` for the static that holds the name of
`UObject::operator delete`'s guard block, whose scope clang numbers one higher
than MSVC did when it built `Core.dll`.

## Installing

```sh
cmake/deploy-deusex.sh /path/to/DeusEx/System [vulkan|pathtracer|d3d11|d3d12|none]
```

That copies every driver that was built, with its `.int`, into the game's System
folder - and `build-x64/PathTracerHelper.exe` beside PathTracerDrv, or from
`HELPER_DIR` if set - and points `GameRenderDevice` at the one named (Vulkan by default),
keeping the previous ini as `DeusEx.ini.prevulkan`. To switch afterwards without
redeploying:

```sh
cmake/select-renderer.sh vulkan|d3d11|d3d12|d3d /path/to/DeusEx/System
```

## What the cross build has to work around

Deus Ex's SDK predates the compilers by two decades, so a few things differ from
an MSVC build. Each is handled in the build or in a commented source change:

- **Struct packing.** The engine DLLs use 4 byte packing (`/Zp4` in the VS
  project), but Windows and Vulkan structures need their natural alignment - the
  Vulkan ABI on 32 bit would otherwise break on every 64 bit field. MSVC gets
  this from `#pragma pack(push, 8)` around those includes, which clang-cl will
  not honour above a command line `/Zp`. `Precomp.h` therefore states the
  engine's packing explicitly and the cross build passes no `/Zp` at all.
- **Inline assembly.** `appRound`, `appFloor`, `appCycles`, `appSeconds`,
  `appMemcpy`, `appMemzero` and `appDebugBreak` exist only as MASM blocks in
  `UnVcWin32.h` and are not exported from Core.dll. The build sets `ASM=0` and
  `VulkanDrv/UE1AsmShims.h` supplies portable equivalents.
- **Header case.** The SDK headers include each other with Windows' casing
  rules (`Core.h` asks for `UnCid.h`, the file is `UnCId.h`). `case-compat.py`
  generates a directory of symlinks for the alternate spellings.
- **`TCHAR` is `unsigned short`.** Core.dll exports the UNICODE flavour of the
  engine API, so the build defines `UNICODE`/`_UNICODE` and compiles with
  `/Zc:wchar_t-`, matching `CharacterSet=Unicode` in the VS project.
- **The engine's allocator.** `UnFile.h` replaces global `operator new` and
  `operator delete` with the engine's `GMalloc`. C++14 sized deallocation then
  calls `operator delete(void*, size_t)`, which the SDK does *not* replace, so
  memory from the engine's allocator reaches the CRT's `free()` - it took about
  820 frames for a `std::vector` reallocation to trip over it. The other SDKs
  bundled here let a package opt out with `UTGLR_NO_APP_MALLOC`, which
  `Precomp.h` already defines; the Deus Ex copy had no such guard, so one was
  added. This is not specific to the cross build: an MSVC build needs it too.
- **dllimport propagation.** clang extends the `dllimport` on `FString` to the
  members of its `TArray<TCHAR>` base, which Core.dll never exported;
  `UnTemplate.h` instantiates that specialization first so they are emitted
  locally. The same effect made `UObject::operator delete`'s `guard()` ask for a
  `__FUNC_NAME__` import whose scope number only VC6 produces, so that one
  `guard()` is gone.

The last three bullets touch `Thirdparty/DeusEx`; those edits are `__clang__`
guarded or inert, so the MSVC build sees the same code it did before.

## Deus Ex specifics worth knowing

Things found while bringing the port up that are the game's behaviour rather
than the render device's, so nobody has to rediscover them:

- **Frame rate.** The engine only enforces a tick rate for network play, so
  Deus Ex free-runs at whatever the GPU manages. Above a few hundred frames per
  second it truncates conversation audio and the intro's camera interpolation
  drifts, which makes cinematics look mis-framed. VulkanDrv's and
  PathTracerDrv's `FPSLimit` setting caps presentation; 120, PathTracerDrv's
  default, or 60 is a reasonable value. Once a frame is already late the
  limiter no longer waits at all: it used to add a whole interval to every frame
  of a game that could not reach the limit.
- **Field of view.** UE1 treats `FovAngle` as the *horizontal* field of view, so
  a wider display crops the top and bottom rather than showing more at the
  sides. A render device cannot widen this on its own - the engine culls and
  clips against its own frustum, so a wider frustum in the device just yields
  empty wedges at the edges. Raise the game's FOV instead (`DesiredFOV` and
  `DefaultFOV` in User.ini). To keep the 4:3 vertical view from the default 75
  degrees: about 91 at 16:9, about 108 at 21:9.
- **Cinematic letterbox and subtitles.** `CinematicWindow.SetRootViewport()` in
  DeusEx.u letterboxes cinematics to exactly 16:9 and draws the subtitles in the
  band below. On a 16:9 screen that band is zero high, and on anything wider the
  game takes its own "screen size is strange" branch and drops the letterbox, so
  cinematic subtitles disappear. Only cinematics are affected; ordinary dialogue
  sizes itself as a fraction of the height and is aspect independent. Playing at
  4:3 is the only way to get them without patching the game's script. The
  driver's resolution list offers 4:3 and 16:9 modes at the monitor's full
  height for exactly this, letterboxed by the present pass.
- **Switching between fullscreen and windowed under Proton.** The engine
  destroys and recreates the whole render device for every mode change - each
  toggle shows an `EndFullscreen`/`AttemptFullscreen` pair and a fresh Vulkan
  device in the log - and the device also asks the engine for a DirectDraw
  backed fullscreen mode. Under Proton the first toggle after startup can leave
  the desktop unpainted or the window unmapped, although the driver reports no
  error at all; toggling once more settles it and it then works indefinitely.
  The same build on native Windows switches cleanly every time, so this is the
  wine/compositor/driver handoff rather than the render device.
  `UseDirectDraw=False` in DeusEx.ini removes the mode switch, which a Vulkan
  device has no use for, should it help.
- **Alt-tab out of fullscreen.** The engine drops back to a window and
  minimises it when it loses focus, and on being restored destroys the render
  device and makes a new one in fullscreen (`AttemptFullscreen` in WinDrv.dll).
  Under wine the restyle that follows takes the focus away again, the engine
  sees another alt-tab, and the game can never be brought back. PathTracerDrv
  keeps the engine from seeing focus loss while fullscreen, so the window stays
  as it is and returning only raises it; its `PathTracerEvents.log` records the
  focus, size and mode changes if this needs looking at again. VulkanDrv
  destroys and recreates itself the same way and has not been changed.
- **Hiding the HUD.** The game has its own console command for it, `ShowHud 0`
  and `ShowHud 1`, on the player. Nothing in the render device is needed.
- **Test on Windows, not only on Proton.** Wine enforces the cursor clip
  loosely, which hid a fault that made the game unplayable on Windows: the
  engine's pointer clip was left describing the window as it stood before this
  device restyled it, and the engine read the clamped recentre back as a
  constant pull on the X axis every frame. Anything touching window or input
  handling wants checking on Windows before it is believed.
