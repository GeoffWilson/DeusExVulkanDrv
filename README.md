# Deus Ex Render Devices: PathTracerDrv and VulkanDrv

> **This is a modified version of [OldUnreal/UT99VulkanDrv](https://github.com/OldUnreal/UT99VulkanDrv),
> which is itself a fork of [dpjudas/UT99VulkanDrv](https://github.com/dpjudas/UT99VulkanDrv)
> by Magnus Norddahl. It is not the original software and is not supported by
> its authors.** This fork, [GeoffWilson/DeusExVulkanDrv](https://github.com/GeoffWilson/DeusExVulkanDrv),
> targets **Deus Ex** (engine 1112fm) rather than Unreal Tournament. VulkanDrv is
> upstream's work made to build and run there; PathTracerDrv is new.

Two render devices for Deus Ex:

- **PathTracerDrv** draws the game by path tracing it with hardware ray tracing:
  real shadows, bounced light, mirrors that reflect, and the game's own lights,
  light effects and fog, denoised with NVIDIA's NRD.
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

UE1 names its ini after the executable: `DeusEx.exe` reads `DeusEx.ini` and the
retail `DeusExRetail.exe` reads `DeusExRetail.ini`. Change the one the game you
run actually reads, or both. On Linux, `cmake/deploy-deusex.sh` copies every
built device into the Steam install and sets both:

```sh
cmake/deploy-deusex.sh "" pathtracer      # or vulkan, d3d11, d3d12
```

## PathTracerDrv

### Running it

It needs `VK_KHR_ray_query` and `VK_KHR_acceleration_structure` **in a 32 bit
process**, which is a property of the environment rather than of the GPU. No
Proton build tested passes them through to a 32 bit client; upstream wine does.
So on Linux it runs under the distribution's wine, with the retail `DeusEx.exe`
from the 1112fm patch rather than Steam's launcher:

```sh
cmake/run-deusex-wine.sh
```

`spike/README.md` has the measurements, and `spike/vkrtcheck` answers the
question for any other setup. Native Windows has no translation layer in the way
and should simply work, but it has not been tried.

It has been developed on an RTX 4090 at 3440x1440, where it measured 150 to 175
frames per second before the denoiser was added, which costs some of that back.

Fullscreen is a borderless window over the whole screen. Alt-tabbing away
leaves it fullscreen, behind whatever was switched to and tracing at 20 frames
a second, rather than letting the engine drop to a window and minimise it: on
the way back the engine destroys the device and makes a new one, and under wine
the restyle that follows took the focus away again, so the game could not be
brought back at all. `PathTracerEvents.log`, beside the game's log, records the
window's focus and size changes, mode switches, swap chain rebuilds and any
crash, flushed as they happen.

### How it works

**The scene is read from the level, not from the render calls.** A render device
is only handed what survived the engine's frustum and BSP culling, and a path
tracer needs what is behind the camera as much as what is in front - that is
where bounced light and reflections come from. So the device reads the level
straight out of `UModel` and the actor list:

- the BSP, with its textures, panning, masked, translucent, modulated, unlit,
  mirrored and backdrop surfaces;
- movers, meshes and characters, posed by the engine itself so animation blends,
  facial animation and attachments come out as the game draws them;
- sprites, decals, environment mapped meshes, the first person weapon, and
  animated and procedural textures (fire, water, and the "wet" textures that
  ripple another texture);
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
one shadow ray at it. Paths bounce off surfaces with cosine weighted directions,
three bounces by default with Russian roulette after the second. Glass and water
are lit by every light at once and without shadows, so the layer they add never
flickers.

**Materials.** Deus Ex has no material data for its renderer, but it does for
its footsteps: every level texture is filed in a group named for what it is -
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

**Noise.** Two things take it out:

- **NRD's ReLAX denoiser**, on by default. The trace splits each pixel into the
  first solid surface it sees - through any glass or decals - and records that
  surface's normal, depth and motion, its lighting apart from its colour, what
  the ray picked up before reaching it, and the colours that put it back
  together. ReLAX denoises the lighting and a final pass puts the picture back
  together, with fog and the screen flash over it. What a mirror shows is
  treated as a surface in its own right, where it appears to be behind the glass,
  and denoised by a second ReLAX pass: ReLAX will not blur a perfect mirror
  itself. Glossy reflections go through ReLAX's specular half alongside the
  diffuse lighting.
- **Per pixel accumulation** while the view is still. Each pixel checks that it
  is looking at the same instance in the same place as last frame, and keeps a
  short history where a moving shadow or an animated light crosses it.

**The CPU overlaps the GPU.** Gathering the next frame's actors runs while the
previous frame is still tracing; only the upload waits for it.

**2D.** The HUD, menus and console are rasterised over the traced picture, so
the game is fully playable.

### Settings

In the `[PathTracerDrv.PathTracerRenderDevice]` section:

	Bounces=3
	Exposure=128
	SkyIntensity=128
	MaxAccumulatedFrames=256
	LightScale=100
	Denoise=True
	UseVSync=True
	VkDeviceIndex=0
	VkDebug=False
	DebugMode=0
	LogTimings=False
	FPSLimit=120
	Materials=True
	GlossBounces=1

- `Bounces`: how many times a path may bounce. Where most of the cost is.
- `Exposure`: overall brightness, a byte around a midpoint of 128.
- `SkyIntensity`: the stand-in sky used only where a level has no sky zone.
- `MaxAccumulatedFrames`: how long a still picture keeps refining.
- `LightScale`: a percentage applied to every light's brightness.
- `Denoise`: NRD from the start. `PT DENOISE` switches it for the session.
- `LogTimings`: logs where each frame's time goes, averaged every few hundred
  frames: the CPU's side, and the GPU's own time on the scene build, the trace,
  the denoiser, the pass that puts the picture back together and the 2D, from
  timestamps. Written to `PathTracerTimings.log` as well as the game's log,
  which loses its last few lines when the game closes under wine.
- `DebugMode`: 1 shows only what moves, 2 shows plain albedo with no lighting.
- `Materials`: surfaces made of something, as above. Off, everything is matte
  and the frame costs what it did before materials: in the Hong Kong market on
  an RTX 4090 at 1920x1440 they add about 2 ms of GPU time a frame, 104 frames a
  second against 85. Half a millisecond of that is the denoiser's specular half,
  which is only built with materials on. `PT NOMATERIALS` switches them for the
  session.
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

`PT` on its own lists them. All of them are diagnostics:

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
- `PT DENOISE`: denoiser on or off.
- `PT NOLIGHTS`, `PT NOSHADOWS`, `PT NOSKY`, `PT NOFOG`, `PT NOMATERIALS`,
  `PT OPAQUE`: switch one thing off to see what it costs or what it is doing.
- `PT BOUNCES n`, `PT GLOSSBOUNCES n`, `PT RESET`.

### What it does not do yet

- **Windows is untested.** Everything above was built and played under wine.
- Two mirrors facing each other show one reflection each rather than a corridor,
  while denoising.
- A character's motion vectors follow the whole character, not its animation.
  No ghosting has been seen, but it is not exact.
- The lighting is faithful to the engine's numbers, not yet tuned for how a
  path traced version of them should look. Some scenes are darker or flatter
  than the rasterised game.

### Building the denoiser

NRD is not in this repository - its licence does not allow its source to be
redistributed here - so `cmake/build-nrd.sh` fetches a pinned release and builds
it as a 32 bit library. Without it the path tracer still builds and runs, just
without the denoiser. See [cmake/README-crossbuild.md](cmake/README-crossbuild.md).

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
from their `DeusExRelease` configurations. It does not include PathTracerDrv yet.

## License

See [LICENSE.md](LICENSE.md): the render devices carry Magnus Norddahl's zlib
style licence, and `Thirdparty/ut432pubsrc` is Epic Games' public source under
the terms of the Unreal retail licence. ZVulkan has its own
[licence](ZVulkan/LICENSE.md). NVIDIA's NRD, which the path tracer links when it
has been built, is under the NVIDIA RTX SDKs licence and is not included here.
