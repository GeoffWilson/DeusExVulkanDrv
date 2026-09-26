# Deus Ex Render Devices: PathTracerDrv and VulkanDrv

> **This is a modified version of [OldUnreal/UT99VulkanDrv](https://github.com/OldUnreal/UT99VulkanDrv),
> which is itself a fork of [dpjudas/UT99VulkanDrv](https://github.com/dpjudas/UT99VulkanDrv)
> by Magnus Norddahl. It is not the original software and is not supported by
> its authors.** This fork, [GeoffWilson/DeusExVulkanDrv](https://github.com/GeoffWilson/DeusExVulkanDrv),
> targets **Deus Ex** (engine 1112fm) rather than Unreal Tournament. VulkanDrv is
> upstream's work made to build and run there; PathTracerDrv is new.

Two render devices for Deus Ex:

- **PathTracerDrv** draws the game by path tracing it with hardware ray tracing:
  real shadows, bounced light, mirrors that reflect, surfaces that shine or stay
  matte according to what they are made of, and the game's own lights, light
  effects and fog, denoised with NVIDIA's NRD or, on an RTX GPU, NVIDIA DLSS
  Ray Reconstruction. On a wide screen it shows more of the world rather than
  less, and can keep the HUD to a 16:9 or 4:3 box in the middle.
- **VulkanDrv** is a conventional rasteriser, the one to play the game on. It
  adds supersampling, HDR, anisotropic filtering and a frame limiter to what the
  game shipped with.

D3D11Drv and D3D12Drv from upstream also build and run for Deus Ex. They are kept
for comparison - D3D11 is the reference the path tracer is checked against - and
are not described further here.

## Installing

Copy the device's `.dll` and `.int` into the game's `System` folder, then set
`GameRenderDevice` in the `[Engine.Engine]` section of the ini to
`PathTracerDrv.PathTracerRenderDevice` or `VulkanDrv.VulkanRenderDevice`.
PathTracerDrv also needs `PathTracerHelper.exe` beside it: the tracing happens
there (see below), and the device will not start without it.

UE1 names its ini after the executable: `DeusEx.exe` reads `DeusEx.ini` and the
retail `DeusExRetail.exe` reads `DeusExRetail.ini`. Change the one the game you
run actually reads, or both. On Linux, `cmake/deploy-deusex.sh` copies every
built device into the Steam install and sets both:

```sh
cmake/deploy-deusex.sh "" pathtracer      # or vulkan, d3d11, d3d12
```

## PathTracerDrv

### Running it

It needs hardware ray tracing - `VK_KHR_ray_query` and
`VK_KHR_acceleration_structure` - and Deus Ex is a 32 bit process, which almost
nothing offers them to. No Proton build passes them through to a 32 bit client,
and NVIDIA's own 32 bit Windows driver does not offer them at all: on an RTX
4090 on driver 32.0.16.1692 a 32 bit client is offered 270 device extensions
and none of the four ray tracing ones, while a 64 bit client on the same driver
is offered 289 including all of them. Every environment offers them to a 64
bit process.

So the tracing runs in one. `PathTracerDrv.dll` keeps the engine's side in the
game's process, and starts `PathTracerHelper.exe`, a 64 bit program beside it,
on the same GPU. The helper has the ray tracing; the two share the traced frame
on the GPU, so it never crosses to the CPU. Everything else - the level, the
actors, the textures, the HUD, the window, the `PT` commands - stays in the
DLL. The helper writes `PathTracerHelper.log` beside the game's log, and exits
with the game.

That makes it run:

- **from Steam, under Proton** - played with Proton-CachyOS, and the image
  sharing it depends on was checked under Proton Experimental, GE-Proton and
  DW-Proton too;
- **under upstream wine**, with the retail `DeusEx.exe` from the 1112fm patch,
  through `cmake/run-deusex-wine.sh`;
- **on Windows**, natively: NVIDIA's 32 bit driver takes the frame from its
  64 bit one just as winevulkan does.

`spike/README.md` has the measurements. Handing the frame over costs about a
tenth of a millisecond, whatever its size.

It has been developed on an RTX 4090 with a 3440x1440 display, first with the
game at 1920x1440 and, since 1.2, at the display's own 3440x1440 with the HUD
pinned. At 1920x1440 the Hong Kong market, one of the heaviest scenes, runs at
about 145 frames a second uncapped with materials on and NRD, with a 1% low of
about 130, and at about 190 with DLSS Ray Reconstruction at Quality, 1% low
160; the frame limiter holds it to 120 by default. `LogTimings` says where a
frame's time goes, the GPU's side included, and what frame rate that came to.

Fullscreen is a borderless window over the whole screen. Alt-tabbing away
leaves it fullscreen, behind whatever was switched to and tracing at 20 frames
a second, rather than letting the engine drop to a window and minimise it: on
the way back the engine destroys the device and makes a new one, and under wine
the restyle that follows took the focus away again, so the game could not be
brought back at all. `PathTracerEvents.log`, beside the game's log, records the
window's focus and size changes, mode switches, swap chain rebuilds and any
crash, flushed as they happen.

### New in 1.2

- **DLSS Ray Reconstruction**, NVIDIA's denoiser and upscaler, on an RTX GPU,
  and the default there at Quality (`DLSS`, `PT DLSS`); see Noise below.
- **Faster frames.** The CPU gathers and records the next frame while the GPU
  traces the last: 106 frames a second became 146 in the Hong Kong market.
- **Wide screens.** A Hor+ field of view, cinematics and conversations framed
  whole on a wide screen, the HUD's markers on what they mark, and the HUD pinned
  to a 4:3 box in the middle of the screen by default, or 16:9; see Wide
  screens below.
- **Shadows from lights with size**, sharp where they start and softer with
  distance (`LightSize`).
- **Rounded meshes.** Characters and objects are shaded with normals smoothed
  across their faces, as the engine does, rather than showing every facet.
- **The player in mirrors**, and in cutscenes whose camera looks on from
  outside.
- **Particles traced with the level** - steam, smoke, sparks - so walls hide
  them, rather than drawn over the picture.
- **A sharp HUD**, sampled the way the other devices sample it.
- **Materials off by default** until they have been checked by hand.
- **Fixes:** a crash loading a save of the map already being played; a crash
  starting the game in a mode narrower than the screen under Proton's Wayland
  driver, in every device here; and bars beside a narrower mode showing the
  last picture shown there.

### How it works

**The scene is read from the level, not from the render calls.** A render device
is only handed what survived the engine's frustum and BSP culling, and a path
tracer needs what is behind the camera as much as what is in front - that is
where bounced light and reflections come from. So the device reads the level
straight out of `UModel` and the actor list:

- the BSP, with its textures, panning, masked, translucent, modulated, unlit,
  mirrored and backdrop surfaces;
- movers, meshes and characters, posed by the engine itself so animation blends,
  facial animation and attachments come out as the game draws them, and shaded
  with normals smoothed across their faces the way the engine smooths them, so
  a low polygon mesh looks as rounded as it does in the original;
- sprites, particles (steam, smoke and sparks, and the laser beams, which the
  engine draws through a render iterator), decals, environment mapped meshes,
  the first person weapon, and animated and procedural textures (fire, water,
  and the "wet" textures that ripple another texture);
- the player: left out of the view from their own eyes, as the engine leaves
  it out, but seen in mirrors and anything else that reflects, and drawn in
  full when a conversation's camera looks on from outside;
- the skybox, seen through the sky zone's own viewpoint as the engine draws it.

The static world is built into a bottom level acceleration structure once per
level; everything that moves is rebuilt only when its shape changes, and placed
each frame in the top level structure. The trace is a compute shader using ray
queries, with no ray tracing pipeline and no shader binding table.

**The lighting is the game's.** Lights are the level's light actors, coloured
with the engine's own `FGetHSV` and reaching `LightRadius * 25`, and everything
the engine does to them is reproduced from `Render.dll` - which has no public
source and was read by disassembly:

- light types: pulse, subtle pulse, blink, flicker and strobe, on the engine's
  clock and with its exact formulas;
- light effects: spotlights (a pawn's following where it looks), static spots,
  non incidence, cylinder, disco, searchlight and rotor;
- special lighting, zone ambient light, and volumetric fog lights in fog zones,
  integrated per pixel along the view ray;
- unlit meshes at the engine's own brightness, and the screen flash for damage
  and water.

Each shaded point samples one light, chosen in proportion to its contribution
from the lights listed for its cell of a uniform grid over the level, and fires
one shadow ray at it - at a random point on a disc around the light rather than
its centre (`LightSize`, a small lamp's size by default), so a shadow starts sharp
where something meets it and softens with distance, as a real one does. The
engine's lights are points, and cast from one a shadow is hard all the way
out. Paths bounce off surfaces with cosine weighted directions, three bounces
by default with Russian roulette after the second. Glass and water
are lit by every light at once and without shadows, so the layer they add never
flickers.

**Materials.** Off by default for now (the `Materials` setting): guessed as
below and not yet checked by hand, some come out wrong - Liberty Island's brick
path is as shiny as glass. Deus Ex has no material data for its renderer, but
it does for its footsteps: every level texture is filed in a group named for what it is -
Metal, Wood, Stone, Tiles, Textile - so the player sounds right walking on it.
The path tracer reads that group as the surface's material. Mesh skins have no
such group, so they fall back on their name ("ChairLeatherTex1") and on what
their decoration breaks into when destroyed (a `WoodFragment`, a
`MetalFragment`). Each material is a roughness, a metalness and a reflectance,
shaded with a GGX microfacet reflection: wood, tiles, polished stone, metal and
leather catch the lights and reflect their surroundings, while concrete, brick,
fabric and anything unrecognised stay matte exactly as before. Glass, decals,
mirrors, self lit and environment mapped surfaces keep their own rules.

A first surface smooth enough to show its surroundings - roughness under 0.5,
so tiles, metal and marble - has its reflection traced in a second, short pass
of its own and denoised by ReLAX as specular, blurred by its roughness. Rougher
glossy surfaces such as wood and leather take highlights from the lights and a
sheen of the zone's ambient, but no traced reflection: at that roughness it is
mostly blur, and tracing it cost a second path for every pixel of them. Anywhere
further along a path, a glossy surface chooses between its matte and its glossy
half in proportion to what each reflects.

The built in choices can be overridden in the game's ini, in a
`[PathTracerDrv.Materials]` section: a texture's own name, or `Group.<name>` for
a whole group, set to `roughness, metalness` with an optional third value for
the reflectance face on. `PT LOOK` names the texture under the crosshair and
what it counts as.

	[PathTracerDrv.Materials]
	Group.Metal=0.3,1
	ChairLeatherTex1=0.35,0

**Noise.** One of two denoisers takes it out:

- **NRD's ReLAX denoiser**, wherever DLSS cannot run. The trace splits each
  pixel into the first solid surface it sees - through any glass or decals - and
  records that surface's normal, depth and motion, its lighting apart from its
  colour, what the ray picked up before reaching it, and the colours that put it
  back together. ReLAX denoises the lighting and a final pass puts the picture
  back together, with fog and the screen flash over it. What a mirror shows is
  treated as a surface in its own right, where it appears to be behind the
  glass, and denoised by a second ReLAX pass: ReLAX will not blur a perfect
  mirror itself. Glossy reflections go through ReLAX's specular half alongside
  the diffuse lighting. With it, **per pixel accumulation** while the view is
  still: each pixel checks that it is looking at the same instance in the same
  place as last frame, and keeps a short history where a moving shadow or an
  animated light crosses it.
- **DLSS Ray Reconstruction**, NVIDIA's network that denoises and upscales in
  one pass, by default at Quality, on an RTX GPU. It wants the noisy picture as it
  was traced, not the lighting split from its colour, and works the lighting
  back out from the albedos, normals, roughness, depth and motion it is given
  with it; the trace writes that set in place of NRD's. The primary rays are
  jittered by one offset a frame, the one it is told, and the trace runs at
  the render size the network is trained for at the chosen quality - 1280x960
  for 1920x1440 at Quality - which it upscales. A last pass tonemaps what comes
  back and puts the fog and the screen flash over it. The jitter is told as
  how far the picture moved, the opposite of the rays' own offset; told it the
  other way round, as it was before 1.2, fine detail shimmered even at DLAA. It
  also keeps what NRD softens: measured against the converged picture in the
  test harness, its error across a shadow's edge is a half to two thirds of
  NRD's. NGX, which it runs on, is 64-bit only: it is possible because the
  tracing is in the helper. Where it cannot run - another GPU, an old driver,
  wine without NVIDIA's NGX core, dxvk-nvapi and DXVK, which Proton provides
  (see `spike/README.md`) - NRD stands in and `PathTracerHelper.log` says why.

  In the Hong Kong market on an RTX 4090 at 1920x1440: NRD takes 6.9 ms of GPU
  time a frame, Ray Reconstruction at Quality 3.8 (tracing 2.0, the network
  1.4) and at DLAA, which does not upscale, 7.4. At Quality the frame is then
  bound by the game's own CPU time rather than the GPU.

**The CPU overlaps the GPU.** Gathering the next frame's actors runs while the
previous frame is still tracing, and the helper records the next frame while
the GPU traces the last, then queues it behind it. Everything a frame writes
for the GPU is staged per frame and copied in on the GPU, so the frame still
in flight is never written under. Measured against v1.1, which recorded a
frame only once the last was done, in the Hong Kong market: 106 frames a
second became 146 and the 1% low 78 became 118, and a frame now takes as long
as the GPU does.

**2D.** The HUD, menus and console are rasterised over the traced picture, so
the game is fully playable. Each piece is sampled as the other devices sample
it - nearest where its art asks for no smoothing, clamped at the edges of art
that is not meant to repeat - so the HUD is as sharp as it is in D3D.

**Wide screens.** The engine's field of view is horizontal: on a screen wider
than 4:3 it keeps the width and crops the top and bottom, so at 21:9 the game's
75 degrees shows under 60% of the height it does at 4:3, and conversations and
cinematics framed for 4:3 lose heads and feet. The raster devices cannot help
it, since they only draw what the engine has projected. The trace builds the
view from the level, so it keeps the height the same field of view gives at 4:3
and widens the view to fill the screen ("Hor+", `WidescreenFOV`). Everything
that frames the view follows from that:

- **Cinematics and conversations.** A cinematic camera's field of view is taken
  from the frame the engine renders with, not the player's. A conversation
  narrows the 3D view to the strip between its black bars; the trace builds
  the view for the whole screen and the bars cover what is outside the strip,
  rather than the strip's view being stretched over the whole screen, so they
  keep their framing on a wide screen - the intro at 21:9 included, its
  subtitles in their bar.
- **What the HUD marks in the world** - the brackets around what can be used,
  the augmentations' target boxes - is placed by the game through the same
  view, so it lands on what the trace shows rather than where the engine's
  narrower view would have put it.
- **A pinned HUD** (`PinnedUI`, `PT PINNEDUI 16:9`). The game is given a 16:9 or
  4:3 mode at the height of the mode chosen, lays its HUD, menus and
  conversations out in it as it was designed to, and the trace fills the rest
  of the mode chosen around them; what spans the whole of the game's width - a
  conversation's bars, a fade - is carried on to the picture's edges.
- **A mode narrower than the screen** is traced at that mode and letterboxed,
  pinned or not, with the bars cleared rather than showing the last picture
  shown there. A 4:3 mode costs what a 4:3 mode costs.

### Settings

In the `[PathTracerDrv.PathTracerRenderDevice]` section:

	Bounces=3
	Exposure=128
	SkyIntensity=128
	MaxAccumulatedFrames=256
	LightScale=100
	Denoise=True
	DLSS=True
	DLSSQuality=1
	UseVSync=True
	VkDeviceIndex=0
	VkDebug=False
	DebugMode=0
	LogTimings=False
	FPSLimit=120
	Materials=False
	GlossBounces=1
	WidescreenFOV=True
	PinnedUI=1.333333
	LightSize=4

- `Bounces`: how many times a path may bounce. Where most of the cost is.
- `Exposure`: overall brightness, a byte around a midpoint of 128.
- `SkyIntensity`: the stand-in sky used only where a level has no sky zone.
- `MaxAccumulatedFrames`: how long a still picture keeps refining.
- `LightScale`: a percentage applied to every light's brightness.
- `Denoise`: denoising from the start. `PT DENOISE` switches it for the
  session.
- `DLSS`: denoise with DLSS Ray Reconstruction rather than NRD, where it can
  run; on by default, at Quality. `DLSSQuality` is how far it upscales: 0 DLAA
  (not at all), 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance.
  `PT DLSS` switches it for the session.
- `LogTimings`: logs where each frame's time goes, averaged every few hundred
  frames: the CPU's side and the helper's, the GPU's own time on the scene
  build, the trace, the denoiser and the pass that puts the picture back
  together, from timestamps, and the frame rate and 1% low all that came to.
  Written to `PathTracerTimings.log` as well as the game's log, which loses its
  last few lines when the game closes under wine.
- `DebugMode`: 1 shows only what moves, 2 shows plain albedo with no lighting.
- `Materials`: surfaces made of something, as above; off by default until the
  materials have been checked by hand. Off, everything is matte and the frame
  costs what it did before materials: in the Hong Kong market on
  an RTX 4090 at 1920x1440 they add about 2 ms of GPU time a frame. Half a
  millisecond of that is the denoiser's specular half,
  which is only built with materials on. `PT MATERIALS` switches them for the
  session.
- `WidescreenFOV`: on a screen wider than 4:3, keep the height of view the
  game's field of view gives at 4:3 and widen the view to the screen ("Hor+").
  The engine's field of view is horizontal, so at 21:9 it keeps the width and
  crops the top and bottom, and conversations and cinematics framed for 4:3
  lose heads and feet; the raster devices cannot help that, since they only
  draw what the engine has projected, but the trace builds the view from the
  level. Keep the game's own field of view at its 4:3 value (75 by default).
  The HUD's brackets around what can be used, and the augmentations' target
  boxes, are placed with the same view, so they stay on what they mark.
  `PT WIDESCREEN` switches it for the session.
- `PinnedUI`: in fullscreen, keep the HUD, menus and conversations inside a
  box of this aspect ratio in the middle of the screen, rather than spread to
  the edges of a 21:9 or 32:9 one, with the traced world still filling the
  whole screen: 1.333333 for 4:3 (the default), 1.777778 for 16:9, 0 for the
  whole width. The game is given a mode of that shape at the height chosen,
  so the resolution it lists is the narrower one, but the ini keeps the one
  chosen. The picture is always the mode chosen: at the screen's own
  resolution the world fills the screen around the box, and a narrower mode
  is letterboxed as on the other devices, with the box inside it when the mode
  is wider than the box. `PT PINNEDUI 16:9`, `PT PINNEDUI 4:3` and
  `PT PINNEDUI OFF` switch it for the session.
- `LightSize`: the radius, in world units, of the disc around each light that
  shadows are cast from; 4 by default, a small lamp's size. A shadow then
  starts sharp where something meets it and softens with distance, as a real
  one does. 0 casts from a point: hard all the way out, which NRD blurs
  evenly and DLSS keeps. `PT LIGHTSIZE n` changes it for the session.
- `GlossBounces`: how far a smooth surface's reflection is traced. 1 lights what
  it shows by the lights and the zone's ambient; more carries the reflection on
  bouncing, at a cost; 0 traces none and keeps only the highlights.
  `PT GLOSSBOUNCES n` changes it for the session.
- `FPSLimit`: frames per second to hold the game to, 120 by default and 0 for
  no limit. Deus Ex cuts conversation audio short when left to run at a few
  hundred frames a second, the intro included. Unlike VulkanDrv's it does not wait
  for each frame to reach the screen, which would stop the CPU overlapping the
  GPU.

### Console commands

`PT` on its own lists them. Most are diagnostics:

- `PT LIGHTS`: the nearest lights that change or have an effect, with their type,
  effect and where they are relative to the view.
- `PT HIGHLIGHT`: paints changing lights green, spotlights magenta and shaped
  lights cyan, at eight times their brightness.
- `PT WEAPON`, `PT LOOK`: log how the held weapon, or the actor under the
  crosshair, is drawn - its style, glow, skins, and every material's flags and
  texture, including what is in the texture and what it counts as being made
  of. For the level itself, `PT LOOK` names the surface's texture, its group and
  its material.
- `PT VIEW NORMALS | DEPTH | MOTION | DIFFUSE | SPECULAR | EMISSION | ALBEDO |
  HITDIST | HISTORY | MATERIAL`: shows one of the denoiser's inputs, how many
  frames each pixel has averaged, or each surface's material (red roughness,
  green metalness, blue where it is glossy), in place of the picture. `PT VIEW`
  alone goes back.
- `PT DENOISE`: denoising on or off. Says what denoises.
- `PT DLSS [DLAA | QUALITY | BALANCED | PERFORMANCE | ULTRAPERFORMANCE]`: DLSS
  Ray Reconstruction on or off, or on at that quality; turns denoising on with
  it. Says whether it is running, and why not when NRD stands in.
- `PT WIDESCREEN`: the widescreen field of view on or off.
- `PT LIGHTSIZE n`: the size lights cast shadows from, as `LightSize`.
- `PT PINNEDUI 16:9 | 4:3 | OFF`: the UI kept to a box of that shape in the
  middle of the screen, or across all of it. Any ratio or number works.
- `PT NOLIGHTS`, `PT NOSHADOWS`, `PT NOSKY`, `PT NOFOG`, `PT OPAQUE`: switch
  one thing off to see what it costs or what it is doing. `PT MATERIALS`
  switches materials on or off (`PT NOMATERIALS` still works).
- `PT BOUNCES n`, `PT GLOSSBOUNCES n`, `PT RESET`.

The game's own `ShowHud 0` (and `ShowHud 1`) hides the HUD, for screenshots.

### What it does not do yet

- Two mirrors facing each other show one reflection each rather than a corridor,
  while denoising.
- A character's motion vectors follow the whole character, not its animation.
  No ghosting has been seen, but it is not exact.
- The lighting is faithful to the engine's numbers, not yet tuned for how a
  path traced version of them should look. Some scenes are darker or flatter
  than the rasterised game.

### Building it on Windows

The CMake project builds natively with MSVC, twice: the DLL from an x86
developer prompt (`vcvarsall amd64_x86`), and the helper from an x64 one
(`vcvarsall amd64`), since a 64 bit configuration of the project builds only
the helper. The Visual Studio solution has no PathTracerDrv project and is
still Unreal Tournament's. The case-compat step is skipped on a Windows host,
which resolves those include spellings itself, so python is not needed there.

```sh
cmake -S . -B build-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release    # x86 prompt
cmake --build build-win32 --target PathTracerDrv
cmake -S . -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release    # x64 prompt
cmake --build build-win64 --target PathTracerHelper
```

Copy `build-win32/PathTracerDrv.dll`, `PathTracerDrv.int` and
`build-win64/PathTracerHelper.exe` into `System`. So far both halves have been
cross built from Linux, as in [cmake/README-crossbuild.md](cmake/README-crossbuild.md),
rather than with MSVC.

### Building the denoisers

Neither is in this repository - their licence does not allow them to be
redistributed here. `cmake/build-nrd.sh` fetches a pinned NRD release and builds
it as a 64 bit library for the helper (and a 32 bit one as well), and
`cmake/fetch-dlss.sh` fetches a pinned release of the DLSS SDK: NGX's loader,
which the helper links, and Ray Reconstruction's runtime, `nvngx_dlssd.dll`,
which goes beside it. Without either the path tracer still builds and runs,
just without that denoiser. See
[cmake/README-crossbuild.md](cmake/README-crossbuild.md).

## VulkanDrv

The rasteriser the game is best played on. Upstream had Deus Ex project files
but had evidently never been built or run for it; the fixes that took were:

- **Memory.** The SDK replaces global `new` and `delete` with the engine's
  allocator, but C++14 sized deallocation bypasses the replacement, so engine
  memory reached the CRT's `free()` and the game crashed after about 820 frames.
- **Field of view** from `FSceneNode::Proj.Z` rather than the player's
  `FovAngle`, which is only the same thing while the player is the camera -
  cinematics came out zoomed with their actors out of frame.
- **The near clip plane** is no longer applied on an engine that clips polygons
  itself; it was slicing characters out of cinematics.
- **Window and cursor handling.** Fullscreen state is captured before the
  viewport is resized, and the cursor clip follows the window. On Windows the
  stale clip read back as constant mouse movement, spinning the player left.
- **The launcher's splash** is hidden before the first mode change. Under
  Proton's Wayland driver, as Proton-GE and CachyOS patch it, the splash
  becomes a popup of the game's window, and starting in a fullscreen mode
  narrower than the monitor ended the process with the compositor's
  "destroyed popup not top most popup". The same goes for every device here.

And what was added:

- **Supersampling** (`RenderScale`), which anti-aliases the fences, grates,
  foliage and shimmering textures that Deus Ex is full of and multisampling
  cannot reach.
- **Alpha to coverage** on masked surfaces, so multisampling smooths their edges
  too.
- **HDR on Wayland** as well as Windows: HDR10 alongside scRGB. Under Proton it
  needs `PROTON_ENABLE_WAYLAND=1 PROTON_ENABLE_HDR=1`.
- **A frame limiter** (`FPSLimit`), paced against presentation where the device
  allows. Not optional in practice: uncapped, the engine cuts conversation audio
  short and its cinematic cameras drift.
- **Anisotropic filtering** (`MaxAnisotropy`), **sRGB textures**
  (`SRGBTextures`), and 4:3 and 16:9 modes letterboxed at the monitor's full
  height, which is the practical answer to cinematics on a wide screen.
- **Surviving a lost device**, rebuilding everything under the instance rather
  than taking the process with it.

Recommended in `[VulkanDrv.VulkanRenderDevice]`:

	RenderScale=2.000000
	AntialiasMode=Off
	FPSLimit=120
	MaxAnisotropy=16.000000

Raise `RenderScale` until the cost shows rather than turning on MSAA as well:
the game is bound by its own engine long before the GPU.

The rest of its settings, from upstream: `UseVSync`, `Hdr`, `HdrScale`, `Bloom`,
`BloomAmount`, `Saturation`, `Contrast`, `LinearBrightness`, `GammaMode`,
`GammaOffset`, `LightMode`, `LODBias`, `VkDeviceIndex`, `VkDebug`,
`VkExclusiveFullscreen`. `GetVkDevices` in the console lists the GPUs to choose
from.

## Building

On Linux, the CMake cross build in [cmake/README-crossbuild.md](cmake/README-crossbuild.md)
builds every device with clang-cl against Microsoft's CRT and SDK, and documents
the Deus Ex specific behaviour worth knowing before filing a bug against a
renderer. It is the only build of PathTracerDrv.

On Windows, the Visual Studio solution builds VulkanDrv, D3D11Drv and D3D12Drv
from their `DeusExRelease` configurations. It does not include PathTracerDrv
yet, and NRD is only built by the Linux script, so a release's PathTracerDrv
comes from the cross build.

## License

See [LICENSE.md](LICENSE.md): the render devices carry Magnus Norddahl's zlib
style licence, PathTracerDrv the same licence with Geoff Wilson as its copyright
holder (less the few files it copies from VulkanDrv, which stay Magnus
Norddahl's), and `Thirdparty/ut432pubsrc` is Epic Games' public source under
the terms of the Unreal retail licence. ZVulkan has its own
[licence](ZVulkan/LICENSE.md). NVIDIA's NRD and the DLSS SDK, which the path
tracer's helper links when they have been fetched, are under the NVIDIA RTX
SDKs licence and are not included here. This software uses NVIDIA DLSS; NVIDIA
and DLSS are trademarks of NVIDIA Corporation.
