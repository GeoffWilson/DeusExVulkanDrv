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
run `wine PathTracerHelperTest.exe [frames] [width] [height]`. The Windows SDK that `xwin` fetches carries the Direct3D
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
