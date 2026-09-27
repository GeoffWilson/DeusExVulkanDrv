# Testing on Windows before a release

Notes for a session on the Windows install, whether it is Geoff alone or a
Claude Code session in the Windows checkout, to test PathTracerDrv 1.2 and the
other devices' 2.2 before they are published. Written 27 September 2026, at
the end of the 1.2 work on Linux.

## Why Windows

Everything here has been developed and played on Linux, under Proton and under
wine. That is not enough on its own: wine enforces the cursor clip loosely and
handles the switch between fullscreen and windowed modes differently, and both
once hid faults that made the game unplayable on Windows while it ran fine on
Linux. Anything touching windows, display modes or input is believed only once
it has run on Windows.

The last build tested on Windows was `be99b9a` (26 September). Since then:

- the level lit as the engine's lightmaps light it, held to the map's baked
  shadow masks;
- mipmapped, anisotropically filtered textures, detail textures, and New
  Vision's S3TC textures;
- zone ambient light, the engine's mesh lighting, light fittings no longer
  shadowing their own lamps;
- the vision augmentation, the tool in the player's hand, and the HUD's
  windows - a security computer's cameras, the spy drone - traced as views of
  their own;
- the weapons characters hold, wavy water, slow and fast wave lights,
  searchlights (police lights) at the engine's speed, animated textures at
  their own speed (a radar screen ran ten times too fast), the vision
  augmentation's static moving;
- performance: the light grid rebuilt only when a light changes, the
  market's characters posed in 1.7 ms a frame rather than 2.7, and
  `PT BENCH`;
- in VulkanDrv, D3D11Drv and D3D12Drv too: the launcher's splash hidden before
  the first mode change, which fixed a crash starting in a mode narrower than
  the screen under Proton's Wayland driver.

## Ground rules for a Claude Code session

- The repository is https://github.com/GeoffWilson/DeusExVulkanDrv, branch
  `deusex`. Check `git remote -v`: push only to that fork's `deusex` branch,
  never to OldUnreal's upstream.
- The Windows checkout has no repository email set. Before any commit, run
  `git config user.email geoff@kernite.co.uk`; no other address may appear in
  a commit.
- Commit or push only when Geoff asks. Commit messages end with the
  attribution lines the session's own instructions give.
- Claude cannot start the game: Geoff runs it, so each round trip costs him a
  launch. Put everything a test needs into one build.
- D3D11 is a reference for how the game should look, not a pixel target:
  traced shadows, bounced light and no light through walls are meant to
  differ.
- Leave the question of an upstream pull request alone unless Geoff raises it.

## Getting the binaries

The simplest and the one to test first: the test zip built on Linux,
`PathTracerDrv-test-<commit>.zip`, beside the checkout on the Linux side. It
holds exactly what the release will ship:

- `PathTracerDrv.dll`, `PathTracerDrv.int`, `PathTracerHelper.exe`,
  `nvngx_dlssd.dll` and `PathTracerHelperTest.exe`;
- `VulkanDrv.dll`, `D3D11Drv.dll`, `D3D12Drv.dll` and their `.int` files.

Building natively with MSVC, as the README's "Building it on Windows"
describes, has never been tried: every build so far has been cross built from
Linux. It is worth trying once, and comparing against the zip's binaries,
but it is not needed for the test. The NRD and DLSS scripts in `cmake/` are
bash; without them the helper builds without that denoiser.

## Installing

Into the game's `System` folder:

1. `PathTracerDrv.dll`, `PathTracerDrv.int`, `PathTracerHelper.exe`, and
   `nvngx_dlssd.dll` beside the helper. Without that DLL DLSS, the default,
   falls back to NRD without saying much.
2. In the ini the executable reads (`DeusEx.exe` reads `DeusEx.ini`): in
   `[Engine.Engine]`, `GameRenderDevice=PathTracerDrv.PathTracerRenderDevice`;
   in `[WinDrv.WindowsClient]`, `UseDirectDraw=False`.
3. In `[PathTracerDrv.PathTracerRenderDevice]`: `LogTimings=True`, and if the
   ini came from 1.1, `Exposure=90` - 1.1's 128 is now too bright.
4. Change only those keys. The game writes its ini with CRLF line endings;
   keep them.

Then run `PathTracerHelperTest.exe` from `System`: it should print PASS.

## What to test, in order

1. **Starting and the window.** Fullscreen at the monitor's own size; a 4:3
   mode on the ultrawide, which the splash fix is about; alt-tab away and
   back; the fullscreen toggle; windowed. Mouse look must not drift, which is
   how the cursor clip fault showed before.
2. **The Hong Kong market** (`06_HongKong_WanChai_Market`). The picture as on
   Linux; armed characters holding their weapons; the radar screen at about 8
   frames a second; the police car's lights turning at D3D11's speed.
3. **`PT BENCH`** in the market, standing where the Linux runs stood: once as
   it is (DLSS Quality), then `PT DLSS` to switch to NRD and `PT BENCH` again.
   Each takes about half a minute; the tables go to `PathTracerTimings.log`.
4. **The HUD.** The vision augmentation; a security computer's cameras; the
   spy drone. With `set DeusEx.JCDentonMale bCheatsEnabled True`, then
   `AugAdd AugVision`, `AugAdd AugDrone` and `AllEnergy`. A save made with the
   vision augmentation on loads with it off, in every device: the game's own
   bug, fixed by changing map.
5. **Lighting in general**, for example the Vandenberg computer room against
   D3D11. The lockers there are about three times too bright: known, parked.
6. **The other devices.** VulkanDrv and D3D11Drv each started in a 4:3 mode
   and at the monitor's size, since the splash fix is in all three.
7. Optionally **New Vision**: its two `Paths` lines go before the stock
   ones, not after as its readme says.

## Linux figures to compare with

`PT BENCH` in the market, 27 September, RTX 4090 under Proton, 1920x1440,
default settings (materials off, 3 bounces), standing still:

| | DLSS Quality | NRD |
|---|---|---|
| Frame rate | about 160 fps, 1% low 120 | about 95 fps, 1% low 85 |
| GPU per frame | 5.8 ms | 10.5 ms |
| Scene gathering (CPU) | 2.2 ms | 2.3 ms |
| Engine lighting's cost (GPU) | about 0.3-1.2 ms, depending on the view | 2.5 ms |
| One bounce instead of three | 3.3 ms less GPU | 6.6 ms less GPU |

The frame was waiting on the GPU in both. Windows may well differ, which is
worth knowing for the release notes either way.

## What to bring back

From `System`: `DeusEx.log`, `PathTracerTimings.log`,
`PathTracerHelper.log` and `PathTracerEvents.log`, plus screenshots of
anything that looks wrong, ideally with D3D11 at the same spot.

## After the Windows pass

Still to do for the release:

- the README's installing steps should mention `nvngx_dlssd.dll`;
- release notes for PathTracerDrv 1.2, leading with the `Exposure=90` upgrade
  note, and for the devices' 2.2;
- the zips, with NVIDIA's DLSS licence text beside `nvngx_dlssd.dll`, and the
  README's line that the software uses NVIDIA DLSS;
- the Windows figures, if they differ, in the README.
