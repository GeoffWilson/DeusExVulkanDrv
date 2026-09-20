# Unreal Tournament Render Devices

> **This is a modified version of [OldUnreal/UT99VulkanDrv](https://github.com/OldUnreal/UT99VulkanDrv),
> which is itself a fork of [dpjudas/UT99VulkanDrv](https://github.com/dpjudas/UT99VulkanDrv)
> by Magnus Norddahl. It is not the original software and is not supported by
> its authors.** This branch makes VulkanDrv work in **Deus Ex** (engine 1112fm),
> a configuration the upstream project has project files for but which had
> evidently never been built or run. See [Deus Ex](#deus-ex) below for what
> changed; everything else is upstream's work.

This project implements Vulkan, Direct3D 12, and Direct3D 11 render devices for Unreal Tournament (UT99).

## Compiling the source

The project files were made for Visual Studio 2022. Open VulkanDrv.sln, select the release configuration and press build.

Note: This project requires the 469 SDK. It also requires 469c or newer to run.

## Using VulkanDrv, D3D11Drv or D3D12Drv as the render device

Copy the .dll and .int files files to the Unreal Tournament system folder.

In the [Engine.Engine] section of UnrealTournament.ini, change GameRenderDevice to VulkanDrv.VulkanRenderDevice, D3D12Drv.D3D12RenderDevice or D3D11Drv.D3D11RenderDevice. Then launch the game.

All the render devices supports the following renderdev specific settings in each their section of the UnrealTournament.ini file:

	LODBias=-0.500000
	GammaOffsetBlue=0.000000
	GammaOffsetGreen=0.000000
	GammaOffsetRed=0.000000
	GammaOffset=0.000000
	GammaMode=D3D9
	UseVSync=True
	LightMode=Normal
	OccludeLines=True
	Hdr=False
	AntialiasMode=MSAA_4x
	Saturation=255
	Contrast=128
	LinearBrightness=128

VulkanDrv specific settings:

	RenderScale=1.000000
	MaxAnisotropy=8.000000
	SRGBTextures=False
	FPSLimit=0
	VkDebug=False
	VkDeviceIndex=0
	VkExclusiveFullscreen=False

D3D12Drv specific settings:

	UseDebugLayer=False

D3D11Drv specific settings (OpenXR virtual reality):

	UseVR=False
	VRResScale=1.500000
	VRWorldScale=52.500000
	VRHeightOffset=0.000000
	VRLookMode=0
	VRHudDepth=150.000000
	VRHudDepthBottom=30.000000
	VRHudBottomY=0.700000
	VRUIDepth=150.000000
	VRCrosshairDepth=150.000000
	VRWeaponIPDScale=1.000000
	VRHudScaleX=1.000000
	VRHudScaleY=1.000000
	VRScaleHudMeshes=False
	VRClearZBeforeHud=True
	VRBrightnessScale=1.000000
	VRBrightnessOffset=0.000000
	VRMirrorMode=1
	VRSBSHalf=False

## Description of settings

- LightMode:
  - Normal: The default rendering of the Direct3D render devices that shipped with the game
  - OneXBlending: Halve the brightness of the light maps (all map surfaces are half as bright). This effectively makes the actors brighter relative to actors
  - BrighterActors: This doubles the brightness of the actors instead of making the light maps dimmer
- GammaOffset, GammaOffsetRed, GammaOffsetGreen, GammaOfsetBlue: Add additional gamma to all pixels. GammaOffset for all color channels, the others for specific ones
- GammaMode:
  - D3D9: Use the gamma calculations from the other Direct3D render devices
  - XOpenGL: Use the gamma calculations from the XOpenGL render device
- Hdr: Overbright pixels will use the high dynamic range of the monitor. Needs HDR enabled in the desktop's display settings and a display that can do it. Two surface types are accepted, in order of preference: scRGB (`VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`), which Windows offers and where the compositor does the display encode, and HDR10 (`VK_COLOR_SPACE_HDR10_ST2084_EXT`) at ten bits per channel, which is what Wayland compositors offer and where the present shader does the Rec.2020 and ST.2084 encode itself. If neither is offered the setting stays SDR; the log says which of the three happened. `HdrScale` sets what the game's white is worth in both paths, so the picture does not change brightness when the same machine moves between them
- AntialiasMode:
  - Off: No anti alias applied
  - MSAA_2x: 2x multisampling
  - MSAA_4x: 4x multisampling
- Saturation: Saturation color control. 255 is the default. 128 is black and white. Zero inversely saturates the colors.
- Contrast: Contrast color control. 128 is the default.
- LinearBrightness: True brightness control (UT's brightness control is actually gamma control). 128 is the default.
- OccludeLines: If true, lines are occluded by geometry in the Unreal editor.
- LODBias: Adjusts the level-of-detail bias for textures. A number greater than zero will bias it towards using lower detail mipmaps. A negative number will bias it towards using higher level mipmaps
- Bloom: Adds bloom to overbright pixels
- BloomAmount: Controls how strong the bloom blur should be. 128 is the default.

## Description of VulkanDrv specific settings

- VkDebug enables the vulkan debug layer and will make the render device output extra information into the UnrealTournament.log file. 'VkMemStats' can also be typed into the console.
- VkExclusiveFullscreen enables vulkan's exclusive full screen feature. It is off by default as some users have reported problems with it.
- VkDeviceIndex selects which vulkan device in the system the render device should use. Type 'GetVkDevices' in the system console to get the list of available devices.
- VkTestDeviceLoss fakes a lost device after this many presented frames, once, and then sets itself back to zero. It exists to exercise the recovery path deliberately rather than discovering it during a real fault. It raises the same error from the same place a real loss does, so the teardown and rebuild are genuine - but the device underneath is still healthy, so it says nothing about how a driver behaves once it has actually lost one. Zero, the default, disables it.
- RenderScale renders the scene at a multiple of the viewport size and scales the result back down when presenting it, which is supersampling: at 2.0 the game draws four samples for every pixel you see. It anti-aliases everything, including the alpha tested edges and the shimmer of high frequency textures at a distance, where multisampling only reaches geometry edges. The cost is quadratic - 2.0 is four times the pixels, and combining it with MSAA multiplies again - and the HUD and text are drawn into the same buffer, so they are softened along with the rest. Accepted values run from 0.25 to 4.0, and anything outside that is clamped with a note in the log - as is a scale the device cannot allocate, which falls back to 1.0 rather than failing to start. Four is not a hardware limit but the point where the buffers stop earning their size: at a 1440p viewport with multisampling that is already some three gigabytes of render targets, and going further mostly buys frame rate loss and memory pressure. Values below 1.0 render below the viewport size instead, trading sharpness for speed.
- MaxAnisotropy sets the anisotropic filtering level, clamped to whatever the device supports. Anything at or below 1.0 turns it off. Higher values sharpen textures viewed at a glancing angle, which on these maps is most of the floors and walls.
- SRGBTextures samples textures as sRGB so the shading runs on linear light, encoding the result back for display at the end. The brightness is unchanged by design: this engine's lighting is a multiplication, and a multiplication gives the same answer in either space, so everything that has to cross over - the light arriving as vertex colours, the fog, the doubling that compensates for half brightness light maps, the detail texture modulation - does, and the result lands where it started. What does change is everything that interpolates or blends rather than multiplies: texture filtering, mip transitions and translucent surfaces resolve in linear light, which reads as smoother gradients and cleaner glow around bright things. Against that, high contrast masked art picks up a visible fringe - the mouse cursor is the clearest example - because a hard white on black edge now filters to a lighter intermediate. Subtle either way, and a matter of taste, which is why it is off by default.
- FPSLimit caps how many frames per second are presented, or zero to leave the frame rate alone. The engine only enforces a tick rate for network play, so an old game on a modern GPU can run at a frame rate its own timing was never written for - Deus Ex cuts conversation audio short well before a 240Hz display's refresh rate, and 120 or 60 is a reasonable cap there. Where the device supports VK_KHR_present_wait, the limiter paces against when a frame actually reached the screen instead of when it was handed over, which keeps frame times even.

## Description of D3D12Drv specific settings

- UseDebugLayer enables the D3D12 debug layer and will make the render device output extra information into the UnrealTournament.log file for any errors or warnings.

## Description of D3D11Drv VR settings

D3D11Drv can render the game in stereo to an OpenXR headset (SteamVR, Oculus/Meta, WMR). The game is presented as a large virtual screen floating in front of you: the world renders once per eye with real stereo depth, the first person weapon, crosshair and HUD are placed at their own comfortable depths, and head tilt keeps the picture level. Requires the OpenXR runtime of your headset to be active; openxr_loader.dll must be next to the game executable or in the system folder.

- UseVR: Master switch. When enabled, the render device probes for an OpenXR runtime and headset at startup and falls back to normal mono rendering if none is found.
- VRResScale: Eye render resolution as a multiple of the resolution recommended by the runtime (supersampling). 1.0 = recommended, 1.5 = default. Values are clamped to what the runtime allows. Raising it sharpens the image at a GPU cost.
- VRWorldScale: World scale in Unreal units per metre, used to derive the stereo eye separation. Larger values make the world feel smaller. 52.5 is calibrated for UT's proportions.
- VRHeightOffset: Vertical comfort shift of the whole picture in the headset, in screen units. 0 = centred; negative values move the image down.
- VRLookMode: 0 = head look enabled: during gameplay head tilt (roll) is compensated so the horizon stays level, and while a menu is open you can look around the virtual screen to reach its corners. Any other value = the screen is fixed to your head.
- VRHudDepth: Distance (in Unreal units) at which flat HUD elements (bars, ammo counters, chat) converge in stereo during gameplay. Larger = further away.
- VRHudDepthBottom: A nearer convergence distance for the bottom strip of the screen (the weapon bar sits "under your nose", where a close depth is much easier on the eyes). Set to 0 or below to disable and use VRHudDepth everywhere.
- VRHudBottomY: Where the bottom strip begins, as a fraction of screen height (0.7 = bottom 30% of the screen uses VRHudDepthBottom).
- VRUIDepth: Convergence distance for the UI while the mouse is free (menus, console).
- VRCrosshairDepth: Convergence distance of the crosshair. The crosshair is detected as the tile drawn at the screen centre, so custom crosshairs work too.
- VRWeaponIPDScale: Stereo separation of the first person weapon relative to the world (1.0 corresponds to 10% of the eye distance). Lower values flatten the weapon and pull it visually out of your face; the default already reduces it, as the weapon model is rendered extremely close.
- VRHudScaleX, VRHudScaleY: Scale the gameplay HUD towards the screen centre so elements in the corners stay inside the headset's field of view. 1.0 = no scaling, 0.8 = HUD occupies 80% of the screen. Menus are never scaled.
- VRScaleHudMeshes: Also apply the HUD scale (and HUD depth) to 3D meshes that mods draw as part of the HUD after a depth clear (for example rotating radar icons). Off by default; enable if a mod's HUD meshes end up outside the visible area.
- VRClearZBeforeHud: Clear the depth buffer before the HUD is drawn so the HUD and crosshair are never occluded by world geometry. Leave on unless you are debugging.
- VRBrightnessScale, VRBrightnessOffset: Brightness correction applied to the headset image only (eye brightness = game brightness * scale + offset). The desktop window keeps the game's normal brightness. Useful because HMD panels often render the game darker or brighter than a monitor.
- VRMirrorMode: What the desktop window shows while VR is active:
  - 0: Nothing (best performance).
  - 1: The game view, but only while the headset is not being worn (default). Wear detection combines the OpenXR session state with the headset's presence sensor and is polled about once per second.
  - 2: Like 1, but the mirror is also active whenever a menu is open (mouse visible).
  - 3: Always mirror. The mirror renders the mono game view aligned with the window, so menus stay clickable with the mouse.
  - 4: Side-by-side recording mode. The window backbuffer is resized to hold both eye images at full eye resolution side by side, cropped vertically to the game screen. A game recorder that hooks the game itself (Bandicam game recording mode, OBS game capture) will capture a full resolution SBS 3D video; YouTube and 3D players accept it directly. The backbuffer size is locked for the whole session so the recording is never interrupted, and head roll is not applied so the recorded frames stay level. The window itself just shows a squeezed preview in this mode.
  - 5: Flat recording mode. An always-on mirror intended for recording a normal 2D video of a VR session: the backbuffer follows the window size like a regular viewport (resolution changes and windowed/fullscreen switches resize it normally), and the HUD is shown at its full size in the recording — the VRHudScaleX/Y scaling (including scaled HUD meshes) is not applied to the mirror, only to the headset view.
- VRSBSHalf: Only used in VRMirrorMode 4. When enabled, each eye is squeezed to half width so the whole frame is one eye wide instead of two (half-SBS / "half side-by-side"). This is the format YouTube and most 3D players expect for a normal-aspect video, and it halves the recorded width. Off by default (full side-by-side, one eye's full resolution per side).

Console commands available while D3D11Drv is active:

- `VRRECENTER`: Reset the head reference so the virtual screen re-centres on your current head pose. Bindable (`set input F12 VRRECENTER`).
- `VR GETPARAM <name>`: Prints the current value of any VR setting (for example `VR GETPARAM VRHudDepth`). Intended for mods: `PlayerPawn.ConsoleCommand("VR GETPARAM VRHudDepth")` returns the value as a string, so a mod can draw its own overlays at the depth the user configured, regardless of which VR render device is installed.
- `VR INGAME`: Prints 1 while the mouse is captured (playing) and 0 while a menu or the console is open — tells a mod whether gameplay HUD depth or UI depth currently applies.

## License

Please see LICENSE.md for the details.

## Deus Ex

VulkanDrv runs Deus Ex 1112fm. Copy `VulkanDrv.dll` and `VulkanDrv.int` into the
game's `System` folder and set `GameRenderDevice=VulkanDrv.VulkanRenderDevice` in
the `[Engine.Engine]` section of `DeusEx.ini`.

Build it either from the `DeusExRelease` configuration of the Visual Studio
solution, or - on Linux - with the CMake cross build described in
[cmake/README-crossbuild.md](cmake/README-crossbuild.md), which also documents
the Deus Ex specific behaviour worth knowing before filing a bug against the
renderer.

### What had to change

Six fixes, each commented where it lives:

- **The package's allocations.** The SDK replaces global `new`/`delete` with the
  engine's allocator, but C++14 sized deallocation bypasses the replacement, so
  engine memory reached the CRT's `free()`. It crashed after roughly 820 frames.
  The sibling UT432 SDK in this repository already had the `UTGLR_NO_APP_MALLOC`
  guard for this; the Deus Ex copy did not. **An MSVC build needs this too.**
- **`FPSLimit`.** The engine only enforces a tick rate for network play. Left
  uncapped, Deus Ex cuts conversation audio short and its cinematic camera
  interpolation drifts.
- **The field of view** now comes from `FSceneNode::Proj.Z` rather than the
  player pawn's `FovAngle`, which is only the same thing while the player is the
  camera. Deus Ex's cinematics have their own cameras, and the mismatch rendered
  them zoomed with the actors out of frame.
- **The near clip plane** is no longer applied on pre-469 engines, which clip
  polygons themselves and whose `NearClip` is a screen space plane. It was
  slicing characters out of cinematics.
- **Fullscreen state** is captured before `ResizeViewport` rather than after, so
  leaving fullscreen no longer restores the fullscreen geometry as the window to
  come back to.
- **The cursor clip** now follows the window. `SetRes` restyles and resizes the
  window after `ResizeViewport`, by which point the engine has already clipped
  the pointer to the window as it was - the small windowed frame. The engine goes
  on recentring the pointer in the middle of the window it now has, so that
  recentre is clamped to the stale rectangle's edge and the engine reads the
  difference back as mouse movement every frame. At 3440x1440 that is a constant
  -726 pixels of X per frame: the cursor sits pinned against one side of the
  menus and the player spins hard left in game, while Y, whose centre still falls
  inside the stale rectangle, behaves perfectly. Only Windows is affected, as
  Wine enforces the clip loosely.

The last four are engine-agnostic bugs in code shared with D3D11Drv and
D3D12Drv, which carry the same `FovAngle` line, the same unconditional clip
plane, and - in D3D12Drv - the same unfollowed cursor clip.

### What else was added

- **Alpha to coverage on masked geometry.** Masked surfaces are a hard alpha
  test, which multisampling cannot anti-alias: with `AntialiasMode` set, world
  edges come out smooth while every chain link fence, grate, railing and leaf
  keeps its jagged cutout - and Deus Ex leans on masked textures heavily. The
  masked opaque pipelines now let the alpha drive the coverage mask, with the
  alpha rescaled to cross the threshold over about a pixel so the edge is
  anti-aliased rather than smeared by mip filtering. It costs nothing when
  multisampling is off, and is left off where blending is in play.

- **Supersampling.** `RenderScale` draws the scene into buffers a multiple of
  the viewport size and scales the result down when presenting, which
  anti-aliases everything rather than only the geometry edges multisampling
  reaches - including the texture shimmer a 2000 game shows plenty of. The
  engine keeps working in viewport pixels throughout; only the Vulkan viewport
  rectangle, the hit test region and the present blit know about the scaled
  size, and the hit rectangle has to scale with it or clicking lands in the
  wrong place. Screenshots come out right for free, since `ReadPixels` already
  blits rather than copies.

- **sRGB textures.** `SRGBTextures` samples textures as sRGB so the shading runs
  on linear light, and the present pass encodes the result back for display. The
  catch is that every value the engine hands over is a display space figure -
  the vertex lighting, the fog, the light map doubling, the detail modulation -
  and each has to be converted or the picture is wrong in a way that looks like
  a lighting bug. Off by default; it is a matter of taste rather than an
  improvement.

- **HDR on Wayland.** `Hdr` asked only for scRGB, which is what Windows offers
  and what lets a render device hand over linear light and leave the display
  encode to the compositor. Wayland offers HDR10 instead, so the setting could
  not engage at all on Linux - and silently, since falling back to an SDR
  surface is not an error. The swap chain now takes either, preferring scRGB
  where both exist, and a second present variant does the work HDR10 does not
  do for you: Rec.2020 primaries, the ST.2084 curve, and a real luminance for
  white rather than "1.0". `HdrScale` keeps its meaning across both, because
  scRGB defines 1.0 as 80 nits and the HDR10 path multiplies by exactly that
  before encoding. Ten bits per channel is required rather than preferred - PQ
  spends its code values over 0 to 10000 nits, so at eight bits the banding
  lands in the dark end of the picture, which in this game is most of the
  picture. Changing the setting changes the swap chain format, which the present
  render pass declares, so that pass and its pipelines are now rebuilt when the
  format moves.

  Under Proton this needs `PROTON_ENABLE_WAYLAND=1 PROTON_ENABLE_HDR=1` in the
  game's launch options. Without the first of those the game runs on XWayland,
  which has no colour management to pass through, and the surface offers two
  8 bit sRGB formats and nothing else however the desktop is configured - so
  the setting cannot engage and, since falling back to SDR is not an error,
  says nothing about why. That is what the log lines are for: on a fallback the
  driver prints whether `VK_EXT_swapchain_colorspace` went in and every
  format/colour space pair the surface offered, which separates "the compositor
  has no HDR to give" from "the driver asked for a pairing it was never going
  to get".

  Worth knowing what it buys before spending an evening on it. Deus Ex has no
  HDR content: the scene buffer holds ordinary display referred values that
  rarely exceed white. What changes is that the values which do - coronas, a
  flashlight on a near wall, muzzle flashes - stop being clipped flat, and the
  dark end gets ten bits instead of eight. Measured against SDR on a 3440x1440
  HDR display, the difference is real but small. `HdrScale` is worth setting
  deliberately rather than left alone, because PQ is absolute: it is what
  declares the game's white in nits (128 is 184 nits, 255 is 304), and if the
  desktop maps SDR content brighter than that the game will simply look dimmer
  than it did before. It can be changed live from the console with
  `set VulkanDrv.VulkanRenderDevice HdrScale 255`.

- **Surviving a lost device.** `VK_ERROR_DEVICE_LOST` used to take the process
  with it, and hard enough that the log lost its tail, so there was not even a
  line saying what happened. It arrives from a driver timeout, a GPU reset, or
  memory pressure severe enough that an eviction cannot be satisfied - none of
  which are the game doing anything wrong, and all of which a user will read as
  the game crashing. It now raises a distinct error type rather than a generic
  one, which is what makes it possible to treat differently from an actual bug.
  Lost mid frame it costs that frame and sets a flag; the rebuild happens at the
  start of the next one, where nothing is half recorded and no render pass is
  open. Everything hanging off the device goes and is built again, while the
  instance and the surface - which a device loss does not invalidate - are kept,
  so the rebuild never has to go back to the window. The texture cache cannot be
  salvaged, since those images lived in memory that is gone, so `PrecacheOnFlip`
  asks the engine to fill it again in one go rather than a stutter at a time.
  Three attempts, then it stops: a device that keeps dying is not coming back,
  and retrying forever would trade a crash for a hang.

  Two things found while writing it that were worth more than the feature. The
  results of `vkWaitForFences` were being discarded, so a lost device would have
  hung on a fence that was never going to be signalled, with nothing in the log
  - a freeze rather than a crash, and far harder to diagnose. And the scene
  buffer allocation has a fallback that catches any exception and drops
  `RenderScale` to 1.0, on the reasoning that a scale the device cannot afford
  should not cost the session; device loss during that allocation would have
  been blamed on the setting, silently reset it, and never reached the recovery
  path at all.

### Recommended settings for Deus Ex

	RenderScale=2.000000
	AntialiasMode=Off
	FPSLimit=120
	MaxAnisotropy=16.000000

`FPSLimit` is not optional in practice: uncapped, the engine cuts conversation
audio short and its cinematic cameras drift.

`RenderScale` earns its cost here in a way multisampling does not. Deus Ex is
full of alpha tested surfaces - fences, grates, foliage, railings - which
`AntialiasMode` cannot touch on its own, and of high frequency textures that
shimmer at a distance, which it cannot touch at all. Supersampling handles both,
and the game is bound by its own engine long before the GPU, so the time is
there to spend. Raise the scale until the cost shows rather than turning on MSAA
as well; the two are worth roughly the same and supersampling is the better buy
in this game. The HUD and text share the scene buffer and are softened slightly
along with everything else, which is the one thing to judge for yourself.

### Possible future work

Ideas weighed against what the 1112 engine can actually supply. Worth noting
first what cannot be done at all: XOpenGL's `BumpMaps` and `ParallaxVersion`
need a bump texture per surface that only the 469 engine hands to a render
device. Deus Ex's `FTextureInfo` has no such field, so there is nothing to
render from short of an external texture pack keyed on texture name - a
different project, not a feature port. Performance work is equally moot: the
game is bound by its own engine long before the GPU, which is why it will run
at several hundred frames per second uncapped.

Everything that was on this list has since been done. The one piece of it that
remains unproven rather than unwritten is device loss against a real fault: the
recovery path has been exercised deliberately, with `VkTestDeviceLoss`, but
never by a driver that had actually lost the device. If it ever does happen to
you, the log will say so in as many words, which is the part that used to be
missing.


Not recommended: the OpenXR VR support in D3D11Drv is not a feature that ports
across. It is bound up with UT's weapon and HUD handling, and Deus Ex's HUD and
conversation system would need rethinking around it.

