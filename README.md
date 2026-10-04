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
  game shipped with, and on a wide screen, like the path tracer, shows more of
  the world rather than less with the HUD kept to a box in the middle.

PathTracerDrv also runs in **Unreal Tournament** with OldUnreal's 469f patch,
for single player and your own servers: see
[PathTracerDrv in Unreal Tournament](#pathtracerdrv-in-unreal-tournament).

D3D11Drv and D3D12Drv from upstream also build and run for Deus Ex. They are kept
for comparison - D3D11 is the reference the path tracer is checked against - and
are not described further here.

## Installing

Copy the device's `.dll` and `.int` into the game's `System` folder, then set
`GameRenderDevice` in the `[Engine.Engine]` section of the ini to
`PathTracerDrv.PathTracerRenderDevice` or `VulkanDrv.VulkanRenderDevice`.
PathTracerDrv also needs `PathTracerHelper.exe` beside it: the tracing happens
there (see below), and the device will not start without it. For DLSS Ray
Reconstruction, its default denoiser, it needs NVIDIA's `nvngx_dlssd.dll`
beside the helper as well; without it NRD denoises instead.

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
about 160 frames a second uncapped with DLSS Ray Reconstruction at Quality,
with a 1% low of about 120, and at about 95 with NRD, 1% low 85: standing
still, with the default settings, under Proton. The frame limiter holds it to
120 by default. `PT BENCH` measures it, and what each of the costlier features
takes out of it, and `LogTimings` says where a frame's time goes, the GPU's
side included, and what frame rate that came to.

**Native Windows costs nothing**, which was not a foregone conclusion given the
tracing happens in another process there as it does under Proton. The same
machine and the same settings, standing where the Proton runs stood, from the
save they were taken on:

| `PT BENCH`, Hong Kong market, 1920x1440 | Proton | Windows |
| --------------------------------------- | ------ | ------- |
| DLSS Ray Reconstruction at Quality       | about 160 fps | 171.7 fps |
| GPU a frame, DLSS                        | 5.8 ms | 5.79 ms |
| NRD                                      | about 95 fps | 97.1 fps |
| GPU a frame, NRD                         | 10.5 ms | 10.24 ms |
| Reading the scene out of the engine (CPU)| 2.2 / 2.3 ms | 2.12 / 2.03 ms |
| One bounce rather than three             | 3.3 ms less GPU | 3.24 ms less |

Every line agrees within the drift between two runs of the same bench, so the
handoff to the helper costs nothing measurable on either platform and the trace
is the trace wherever it runs. The 1% lows were if anything steadier on Windows:
about 155 against a 170 average with DLSS, where Proton's sits nearer 120, and
about 92 against 97 with NRD, where Proton's is about 85.

`PT BENCH` wants a settled frame rate: the row it measures first, straight
after a `PT DLSS` switch, came out 1.4 ms slower than the same settings
measured again at the end, and slower than every row in between. Read its
repeat of the settings as they are, at the foot of the table, as the baseline
the rest should be compared against.

Fullscreen is a borderless window over the whole screen. Alt-tabbing away
leaves it fullscreen, behind whatever was switched to and tracing at 20 frames
a second, rather than letting the engine drop to a window and minimise it: on
the way back the engine destroys the device and makes a new one, and under wine
the restyle that follows took the focus away again, so the game could not be
brought back at all. `PathTracerEvents.log`, beside the game's log, records the
window's focus and size changes, mode switches, swap chain rebuilds and any
crash, flushed as they happen.

### New in 1.3

- **A real flashlight.** The light augmentation shone a round patch of light
  wherever the view landed, its shadows falling away from the patch rather
  than from you. It is now a torch worn at the temple: a bright hotspot in a
  wider, dimmer spill, falling off with distance, with traced shadows and
  bounced light (`Flashlight`, `PT FLASHLIGHT`), and optionally its beam in
  the air (`FlashlightHaze`, `PT BEAM`).
- **Shafts of light in fog.** The engine's volumetric fog lights glow through
  everything, so a lamp's halo shows through a pillar and on both sides of a
  wall. Their glow is now held to their shadows: it falls through grates and
  fan blades in shafts, and a pillar casts a shadow through it
  (`FogShadows`, `PT FOGSHADOWS`).
- **Glowing surfaces sampled as lights.** Signs, light panels and screens lit
  what was around them only through the bounces that happened to reach them,
  so a small one left a noisy, flickering spill. Each surface shaded now also
  traces a shadow ray to a point on a glowing triangle nearby, weighed against
  the bounces so nothing is counted twice: the same light, with far less
  noise - and turned up tenfold by default, so that neon spills its colour
  onto the walls around it (`GlowLighting`, `PT GLOW n`).
- **Photo mode.** `PT PHOTO` holds the world still and lets the camera fly
  free through it, through walls and out of the level, with the usual
  movement keys and mouse. JC is left standing where he was, for the camera
  to look back at. Hold still and the picture refines: the denoiser gives way
  to every frame's samples averaged, up to 4095 a pixel, with more bounces
  than play can afford. Fire saves it as a PNG in `Photos` beside `System`,
  and `PT PHOTO APERTURE n` adds depth of field. See the commands below.
- **Coloured light through glass.** The engine's lightmaps let light through
  glass as if it were not there. A lamp shining through a tinted pane now
  takes its colour, and stained glass throws its pattern onto the floor in
  colour, softening with distance as shadows do (`ColouredGlass`,
  `PT GLASS`).
- **Wet streets.** Optional, since it never rains in the original: the ground
  open to the sky darkened and shining as after rain, with puddles standing
  on the flat, reflecting the neon across the street. What an awning, a
  balcony or a glass roof covers stays dry (`Wetness`, `PT WET n`).
- **Bump mapping.** Optional, and off unless asked for (`BumpMapping=100`,
  `PT BUMP 100`): walls and floors are otherwise lit as flat planes, as the
  engine draws them. Deus Ex has no height maps, but in what is built of
  pieces the gaps are dark - mortar between bricks, grout between tiles,
  cracks in concrete - so each texture's brightness is taken as its height,
  as deep as its material allows, and the surface is lit by its slope: by the
  lights, the flashlight and what it reflects. The detail texture adds its
  grain up close. Signs, glass, paper and characters stay as they were.
- **Fixes.** The skybox is turned by its sky zone's rotation, as the engine
  turns it: Liberty Island's skyline sat some 28 degrees round from where the
  other devices put it, with a purple line under it where a masked texture's
  edge was bled from the wrong side. Save games have their picture again.

### New in 1.2

- **The level lit as the engine lights it**: each light's falloff, its
  brightness and the zone's ambient added up the way the engine builds its
  lightmaps, held to the shadow masks the map was built with, and drawn at
  the brightness every device draws them at - but with traced shadows as
  well (`Lighting`). The pools of light, the saturated colour and the
  contrast of the original are back, where the lighting before was flatter,
  greyer and dimmer, and a room comes out as bright as the other devices
  draw it. See How it works below.
- **A neutral tone curve**, which leaves all but the brightest fifth of the
  picture as the other devices draw it (`NeutralToneMap`); and a new default
  `Exposure` of 90.
- **DLSS Ray Reconstruction**, NVIDIA's denoiser and upscaler, on an RTX GPU,
  and the default there at Quality (`DLSS`, `PT DLSS`); see Noise below.
- **Faster frames.** The CPU gathers and records the next frame while the GPU
  traces the last: 106 frames a second became 146 in the Hong Kong market with
  1.1's lighting and NRD. The engine's lighting costs some of that back.
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
- **The game's Brightness setting**, applied over the finished picture as
  VulkanDrv applies it; it did nothing before.
- **Torch and fire waver and watery shimmer**, which lit as steady lights,
  and the slow and fast waves that roll across a lit surface.
- **Searchlights and police lights** turning at the engine's speed with one
  beam, where they swept four beams round at four times the rate.
- **Wavy water**: surfaces the map marks as wavy drift about as the engine
  pans them, where they sat still.
- **Characters hold their weapons**, which were missing from every armed
  character's hands.
- **Characters and objects lit as the engine lights them**, brighter and
  flatter than a wall under the same lamp, with a rim from a light behind.
- **Animated textures on the engine's clock** - fire, water, screens - which
  could run at the wrong speed, and stopped behind the pause menu. A texture
  with no speed of its own, which the engine steps once a frame, runs at its
  animation's speed, or 30 frames a second, rather than at the frame rate: a
  radar screen ran ten times too fast. And textures the game redraws
  itself, such as the vision augmentation's static, keep moving, where they
  froze on their first picture.
- **Detail textures**, the fine grain over a wall or floor close up, as Deus
  Ex's own Direct3D renderer lays them on. The game's Detail Textures setting
  switches them.
- **Zone ambient light as the engine gives it**, where walls lit by nothing
  else were over twice as bright; and `AmbientGlow` on lit meshes.
- **Light fittings no longer block their own lamps**, which left the floor
  under a hanging lamp dark and the ceiling above it lit.
- **Mipmapped textures**: the packages' own mip chains, the levels the other
  devices draw, chosen per ray by how wide its footprint is where it lands,
  and filtered anisotropically on the level's surfaces (`MaxAnisotropy`), so
  distant floors and walls no longer sparkle and crawl, and a floor seen down
  a corridor stays sharp.
- **New Vision**'s high resolution textures (`UseS3TC`); see New Vision below.
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
  facial animation and attachments come out as the game draws them, the
  weapon a character holds fixed where the engine fixes it, to a hidden
  triangle in the character's hand, and shaded
  with normals smoothed across their faces the way the engine smooths them, so
  a low polygon mesh looks as rounded as it does in the original;
- sprites, particles (steam, smoke and sparks, and the laser beams, which the
  engine draws through a render iterator), decals, environment mapped meshes,
  the first person weapon or tool - a lockpick, a multitool, the key ring -
  and animated and procedural textures (fire, water, and the "wet" textures
  that ripple another texture);
- the player: left out of the view from their own eyes, as the engine leaves
  it out, but seen in mirrors and anything else that reflects, and drawn in
  full when a conversation's camera looks on from outside;
- the skybox, seen through the sky zone's own viewpoint and turned by its
  rotation, as the engine draws it;
- the vision augmentation: its tints and labels, and the people and bodies it
  shows through walls, which the HUD draws as meshes over the view;
- the views the HUD draws in windows of their own - a security computer's
  cameras, the spy drone's - each traced from its own camera after the
  player's view, at its window's size, and averaged over the frames it holds
  still rather than denoised, so a moving one stays grainy. The camera it is
  seen from, which the engine hides while it draws the view, is left out of
  it, and the player is in it.

The static world is built into a bottom level acceleration structure once per
level; everything that moves is rebuilt only when its shape changes, and placed
each frame in the top level structure. The trace is a compute shader using ray
queries, with no ray tracing pipeline and no shader binding table.

**The lighting is the game's.** Lights are the level's light actors, coloured
with the engine's own `FGetHSV` and reaching `LightRadius * 25`, and everything
the engine does to them is reproduced from `Render.dll` - which has no public
source and was read by disassembly:

- the level's surfaces as the engine builds their lightmaps (`Lighting=Engine`,
  the default): each light by 1 - 3x^2 + 2x^3 of the way x out to its radius
  and by the cosine, a light baked into the map at twice a dynamic one (the
  engine filters a shadow mask out to 255, where a light without one is
  filled at 127), added up with the zone's ambient as displayed colours,
  clamped, and drawn at twice the texture's brightness, as every device draws
  a lightmap. Adding lights as displayed colours is where the original's pools
  of light and its saturation come from: overlapping lights come out brighter
  together than light adds up. The trace takes that sum of the lights that
  actually reach the point - a shadow ray at each of the four strongest in
  reach and at one picked from the rest (among many lights, see Crowded
  lights) - so shadows are traced ones, and a light behind a wall adds
  nothing. Each light baked into a surface is also
  held to its shadow mask there, filtered as the engine filters it: the masks
  are coarse - a texel can be most of a metre across - and blurred, and on a
  small wall most of them can lie in shadow where the light in fact reaches
  it all, so a room traced alone came out several times as bright as the
  engine draws it. So a surface is no brighter than the engine's lightmap,
  and no brighter than the trace lets it be: a lamp the lightmap lets through
  a wall still adds nothing. This matches the engine's own lightmaps, which
  `PT LOOK` reads, to about 1.5%. The engine's lighting cost about 0.9 ms of
  GPU time a frame over the linear (a Vandenberg room, RTX 4090, traced at
  1280x960); the masks added nothing measurable, since only the lights that
  are traced look theirs up. `Lighting=Linear` lights them as before 1.2,
  each light linear to nothing at its radius in linear light: flatter, greyer
  and dimmer;

- light types: pulse, subtle pulse, blink, flicker and strobe, on the engine's
  clock and with its exact formulas;
- light effects: spotlights (a pawn's following where it looks), static spots,
  non incidence, cylinder, disco, searchlight and rotor, the torch and
  fire wavers and watery shimmer, which flicker across the surfaces they
  light a lightmap texel at a time, and the slow and fast waves, bands of
  light that roll outwards from the light;
- zone ambient light as the engine works it out: `FGetHSV` at the zone's
  brightness, which puts it through the engine's brightness curve, close to a
  square root. A lightmap starts every texel at half of that, which the
  devices draw doubled; a lit mesh gets all of it, with its own `AmbientGlow` (a pickup's pulse at 255), added to
  its lights before they are clamped. Few zones set one - most of the game has
  none - but where they do, ambient-lit walls were twice as bright as the
  engine draws them;
- special lighting, and volumetric fog lights in fog zones,
  integrated per pixel along the view ray, and drawn where the engine draws
  them: only while the player stands in a fog zone, and only over surfaces
  and actors in one, never over the sky - and held to their shadows: each
  fog light's glow is taken at points along the ray and scaled by the share
  of them it can see, from a cube around the light of how far it sees each
  way, traced every frame, so that walls, grates and fans cut shafts and
  shadows through it where the engine's glows through them;
- unlit meshes at the engine's own brightness, and the screen flash for damage
  and water;
- lit meshes as the engine lights them, not as a flat surface is: by
  (N.L + 1)^2 - 1.5 rather than N.L, which leaves a character brighter and
  flatter under a lamp and falling into shadow more sharply, with a sheen
  where a light lies along the surface beyond it - from behind the surface
  too - scaled by 1.4 times the actor's ScaleGlow. Summed in the colours as
  displayed, as the engine does its arithmetic, clamped so a mesh is never
  lit past its texture, then made linear. Unlike the engine's, the meshes
  keep their shadows from the lights in front of them. `PT MESHLIGHT` lights
  them as flat surfaces instead, to compare.

Detail textures are laid on as the `D3DDrv.dll` Deus Ex shipped with lays them,
which differs from the published UT source: three passes over the surface,
each multiplying it by twice the detail texture faded towards mid grey. The
first is at the detail texture's own scale and fades in from 380 units away to
full at 107; each after is 4.223 times finer and fades in a 4.223th as far out.
They multiply the displayed colour, so the linear one takes them to the power
2.2. The game's Detail Textures setting (`DetailTextures`) switches them, as it
does in the other devices.

Textures are sampled with their mips - the chains the packages store, which
are what the other devices upload - at a level chosen per ray from a cone
traced alongside it ("Texture Level of Detail Strategies for Real-Time Ray
Tracing", Ray Tracing Gems chapter 20): a pixel wide at the eye, or an output
pixel wide when DLSS upscales, as NVIDIA asks, widening with distance and more
steeply after a bounce, where fine detail no longer shows. Each triangle
carries how much texture is laid across it for this, and a flat one - the
level, its movers, decals, sprites - how its texture coordinates change
across it too: from those the footprint's two axes go to the texture unit,
which filters anisotropically along the longer (`MaxAnisotropy`), so a floor
seen at a glance down a corridor stays sharp and still. A mesh's is filtered
by the middle of its footprint's two axes. Masked textures are still tested
for holes at their full size, so a grille keeps its bars. A texture that
changes as it is drawn - fire, water, a screen - is sent without mips, as its
frames are.

Each shaded point samples one light, chosen in proportion to its contribution
from the lights listed for its cell of a uniform grid over the level, and fires
one shadow ray at it - at a random point on a disc around the light rather than
its centre (`LightSize`, a small lamp's size by default), so a shadow starts sharp
where something meets it and softens with distance, as a real one does. The
engine's lights are points, and cast from one a shadow is hard all the way
out. A mesh with a light inside it - a hanging lamp's trough, a desk lamp's
shade - casts no shadows: the engine never lets a mesh shadow its lightmaps,
so levels put lights inside their fittings, and traced, the fitting shut the
light in. Paths bounce off surfaces with cosine weighted directions, three bounces
by default with Russian roulette after the second. Glass and water
are lit by every light at once and without shadows, so the layer they add never
flickers.

Smoke, sparks, a lamp's light cone - whatever is drawn by adding an unlit
colour - is summed wherever a ray crosses it, in one pass along the ray to
the next thing the ray stops at, rather than stopped at one piece at a time
as glass is: its light does not depend on the order it is met in. One piece
at a time, a thick plume of steam used up the layers a ray may pass through,
and the floor and the fog behind it went missing in squares. Glass, decals
and the like are still passed one at a time, up to sixteen along a ray.

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
the reflectance face on and a fourth for the relief in world units (see
`BumpMapping`). `PT LOOK` names the texture under the crosshair and what it
counts as.

	[PathTracerDrv.Materials]
	Group.Metal=0.3,1
	ChairLeatherTex1=0.35,0
	Group.Stone=0.55,0,0.04,4

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

**Crowded lights.** Which lights a point weighs comes from a grid over the
level, each cell listing the lights that reach it. Weighing all of them, at
the first surface and at every bounce, was most of the frame where a level
crowds them: the Wan Chai canal's neon puts up to 78 in a cell, and with the
lights off the trace there took a quarter of the time. So the helper ranks
each cell's lights by what they could give it - how bright each gets at its
peak, by a falloff like the engine's across the cell - and a point weighs
only the heaviest few exactly, eight where the view meets the world and four
at a bounce, and draws a few more from the rest in proportion to their rank.
Each draw is weighed exactly too, and counted for the lights it stands for,
so the pick of what to trace stands for the whole cell (resampled importance
sampling), and the picture comes out on average as it did. In the canal at
3440x1440 with NRD, on an RTX 4090, a frame's GPU time went from 25.0 ms to
17.2 - 40 frames a second to 58 - by `PT BENCH`'s "every light weighed". In
the test harness, with 80 lights reaching everywhere at 3440x1440, the trace
took 3.9 ms rather than 12.2, and the converged pictures of the two agree to
within 0.1% (`--lights`, `--every-light`, `--reference`). A cell with no more lights
than that is weighed in full, as before, and so are glass and water, whose
light must not change from frame to frame. `PT ALLLIGHTS` weighs every one.

A flickering light keeps its place in the light list while it is dark, where
it used to drop out of it: the helper built its grid again whenever the list
changed, which with the canal's neon was every frame, 2 ms of it.

**HDR.** The trace works out real brightnesses, and SDR has to squeeze every
one above its white into the last fifth below it: a lamp, a neon tube and the
sunlit wall beside them come out nearly the same white. With `HDR` on, the
tone curve - the neutral one, whose part below its shoulder draws the scene
exactly as SDR does - levels off at the display's peak instead of at white,
so what was squeezed spreads out above white, with the SDR range and the HUD
at `HDRPaperWhite` nits. The fog, the screen flash, the HUD and the Brightness
setting are all put over the picture as they are in SDR, and only then is it
encoded for the display: linear light for scRGB, or Rec.2020 and the PQ curve
for HDR10, which is what a Wayland compositor offers. Photos, save games'
pictures and screenshots stay SDR: the HDR shoulder is undone and SDR's put
back, which the harness measured within a fifth of a level of the SDR curve.
In HDR the tone curve is always the neutral one; Reinhard's has no part that
SDR and HDR could share.

**Headsets.** With `VR` on, the helper opens a session with the OpenXR
runtime - SteamVR, or the headset's own software - and traces each frame
twice, once from each eye, each eye with its own history and its own
denoiser. The room the headset tracks is fitted to the player: the eyes are
where the player's eyes are, `VRUnitsPerMetre` world units to the metre, and
straight ahead is the way the player faces, but level. The mouse still turns
the player and aims, and the room turns with them; looking up and down with
it leaves the horizon where it is rather than tilting the world, which is the
surest way to make a headset's wearer ill. The head looks round inside the
room, and a lean moves the eyes. The seat is put where the head is as the
headset starts showing the game, since a runtime's own origin can be
anywhere - at the floor, or at standing height - and anything but the head
there would put the eyes above or below the player's; `PT VR RECENTER` puts
it there again. Each eye is traced from exactly where the
runtime says it will be when the frame is shown, and sees further to its own
side than across the nose, as a headset's eyes do, which the trace, the
motion it hands the denoiser and NRD's projection all follow. The HUD is drawn
on its own, with how much of the world it covers kept beside its colour, and
shown on a panel `VRHudDistance` metres out that follows the head,
`VRHudSize` degrees across. What it marks in the world - the brackets round
what can be used, the augmentations' target boxes - the game places through
a view the device turns to where the head looks, so they land on the world
behind them wherever the head turns, and the panel is put where the head
was when they were placed rather than where it is a moment later, so they
stay on it while it turns. What aims with the mouse - the crosshair, and the
accuracy reticle round it - is drawn into a layer of its own and shown on a
second panel along the aim, so it is where the shot goes. The lights'
coronas, which the engine draws over the world, go on a third panel, placed
through the head as the HUD is but 10 metres out, so the eyes meet on them
out where the lights are rather than at the HUD; the screen's copy of the
left eye leaves them out. The runtime sets the pace: the helper waits
for the moment to start each frame, which holds the game to the headset's
refresh, and where a frame is late the runtime turns the last one as the head
turns. The screen's VSync and `FPSLimit` step aside meanwhile. A menu with no
level behind it goes on the panel over the last picture, which the runtime
keeps turning as the head turns. The screen shows the left eye, cut to its
shape, under the HUD, which is drawn there as the headset's panel has it, so
it lines up with the picture only roughly. Each eye is traced at the size the
headset asks for, times `VRResolution`, with `VRDLSSQuality` and `VRBounces`
in place of the screen's `DLSSQuality` and `Bounces`. The
Brightness setting is put on the headset's pictures as it is on the screen's;
HDR is the screen's alone.

**2D.** The HUD, menus and console are rasterised over the traced picture, so
the game is fully playable. Each piece is sampled as the other devices sample
it - nearest where its art asks for no smoothing, clamped at the edges of art
that is not meant to repeat - so the HUD is as sharp as it is in D3D.

**Wide screens.** The engine's field of view is horizontal: on a screen wider
than 4:3 it keeps the width and crops the top and bottom, so at 21:9 the game's
75 degrees shows under 60% of the height it does at 4:3, and conversations and
cinematics framed for 4:3 lose heads and feet. The trace keeps the height the
same field of view gives at 4:3 and widens the view to fill the screen ("Hor+",
`WidescreenFOV`), building it from the level; VulkanDrv does the same by having
the engine compute the wider view itself. Everything that frames the view
follows from that:

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
  conversation's bars, a fade - is carried on to the picture's edges, and so
  is the black round a scope's or the binoculars' sight.
- **A mode narrower than the screen** is traced at that mode and letterboxed,
  pinned or not, with the bars cleared rather than showing the last picture
  shown there. A 4:3 mode costs what a 4:3 mode costs.

### Settings

In the `[PathTracerDrv.PathTracerRenderDevice]` section:

	Bounces=3
	Lighting=Engine
	Exposure=90
	NeutralToneMap=True
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
	MaxAnisotropy=16
	UseS3TC=True
	Flashlight=True
	FlashlightBrightness=100
	FlashlightHaze=0
	FogShadows=True
	GlowLighting=1000
	ColouredGlass=True
	Wetness=0
	BumpMapping=0

- `Bounces`: how many times a path may bounce. Where most of the cost is.
- `Lighting`: `Engine`, the default, lights the level's surfaces as the engine
  builds its lightmaps, with traced shadows; `Linear` as before 1.2. See How it
  works above. `PT LIGHTING` switches it for the session.
- `Exposure`: overall brightness, a byte: the light is scaled by
  0.2 + Exposure / 127.5, so 102 draws an unlit surface exactly as the other
  devices do. 90, the default, sits a little under that, since bounced light -
  which the rasterised game has none of - adds to every lit surface. An ini
  from before 1.2 keeps its old 128, which is now too bright: set it to 90.
  `PT EXPOSURE n` changes it for the session.
- `NeutralToneMap`: the tone curve that brings the traced light into the
  screen's range. On by default: all but the brightest fifth is left as the
  other devices draw it, and only that is rounded off towards white. Off,
  Reinhard's, as before 1.2, which compresses everything and greys bright
  colours. `PT TONEMAP` switches it for the session.
- `SkyIntensity`: the stand-in sky used only where a level has no sky zone.
- `MaxAccumulatedFrames`: how long a still picture keeps refining.
- `LightScale`: a percentage applied to every light's brightness.
- `Denoise`: denoising from the start. `PT DENOISE` switches it for the
  session.
- `DLSS`: denoise with DLSS Ray Reconstruction rather than NRD, where it can
  run; on by default, at Quality. `DLSSQuality` is how far it upscales: 0 DLAA
  (not at all), 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance.
  `PT DLSS` switches it for the session.
- `FSR`: AMD's FSR 3.1 upscaler. Where Ray Reconstruction cannot run - any AMD
  or Intel GPU - the trace and NRD run at the size `DLSSQuality` gives and FSR
  brings the picture up to the screen's, at the same ratios (0 native, 1
  Quality 1.5x, 2 Balanced 1.7x, 3 Performance 2x, 4 Ultra Performance 3x).
  0 never, 1 where Ray Reconstruction cannot run (the default), 2 always, in
  its place. `FSRSharpness`, 0 to 1, sharpens what it gives; 0.2 by default.
  On the Steam Machine (Navi 33) at 1080p it took the test scene from 20.2 ms
  a frame to 10.0 at Quality and 6.0 at Performance.
- `LogTimings`: logs where each frame's time goes, averaged every few hundred
  frames: the CPU's side and the helper's, the GPU's own time on the scene
  build, the trace, the denoiser and the pass that puts the picture back
  together, from timestamps, and the frame rate and 1% low all that came to.
  Written to `PathTracerTimings.log` as well as the game's log, which loses its
  last few lines when the game closes under wine. Gathering the scene is
  broken down there by what it spent the time on (lights, animated meshes,
  other actors, held weapons, particles, fittings, decals, the view model),
  and the helper's own time by stage in `PathTracerHelper.log`.
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
  lose heads and feet. Keep the game's own field of view at its 4:3 value
  (75 by default).
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
- `UseS3TC`: use the S3TC textures a package carries beside its originals,
  as OpenGLDrv's setting of the same name does - New Vision's (see New Vision
  below). On by default; a package without them is unaffected.
- `MaxAnisotropy`: the texture filter's anisotropy, as the other devices call
  it: how many samples it may take along a surface seen at a slant - a floor
  down a corridor - to keep it sharp without sparkling. 16 by default; 4 and 8
  are cheaper, and 0 or 1 filters trilinearly. `PT ANISOTROPY n` changes it
  for the session.
- `GlossBounces`: how far a smooth surface's reflection is traced. 1 lights what
  it shows by the lights and the zone's ambient; more carries the reflection on
  bouncing, at a cost; 0 traces none and keeps only the highlights.
  `PT GLOSSBOUNCES n` changes it for the session.
- `Flashlight`: the light augmentation as a real torch rather than the patch
  of light the game moves to wherever you look: shining from beside your eyes
  with a hotspot about 12 degrees across in a spill out to about 32, falling
  off with the square of the distance, with traced shadows and bounced light,
  and its beam in the air. `FlashlightBrightness` is its brightness and
  `FlashlightHaze` how much of its beam the air shows, both percentages. The
  beam is off by default: seen from beside the lamp it lights the air in the
  whole of the cone as a veil rather than showing as a beam. At 100 it is
  faint in clear air and ten times as strong in a fog zone. Off, the augmentation lights as
  the game has it. `PT FLASHLIGHT` switches it for the session,
  `PT FLASHLIGHT n` sets the brightness and `PT BEAM n` the haze.
- `FogShadows`: the glow of the engine's fog lights held to their shadows, so
  it falls through grates in shafts and a pillar shadows it, rather than
  glowing through everything as the engine draws it. `PT FOGSHADOWS` switches
  it for the session.
- `GlowLighting`: how much the level's glowing surfaces - signs, light
  panels, screens, anything the map marks unlit - light what is around them,
  in percent. 100 is as bright as they glow, which is what they had always
  given and hardly shows: a sign's texture is no brighter than a lit wall.
  1000, the default, spills a sign's colour onto the walls around it, as real
  neon would. What they look like is unchanged. In the Hong Kong market, with
  2,068 glowing triangles, sampling them costs 0.36 ms of GPU a frame at 1000
  (DLSS Quality at 1920x1440 on an RTX 4090). The level's own glowing surfaces are sampled as lights
  where they would add at least a fiftieth to what the lights give; glass and
  meshes that glow are found only by the bounces that reach them.
  `PT GLOW n` sets it for the session.
- `ColouredGlass`: light through glass takes its colour, on by default.
  Glass drawn by adding its colour - most of Deus Ex's windows - passes its
  texture's hue as far as it is coloured at all, so clear and grey panes pass
  light as it is and deep red stained glass passes red; glass drawn
  modulated passes what it darkens the view by. Read texel by texel, so a
  stained window's pattern lands on the floor. Sprites and what glows - a
  lamp's light cone - tint nothing. `PT GLASS` switches it for the session.
- `Wetness`: wet streets, in percent, 0 (dry) by default. The ground in zones
  with a window onto the sky, facing up, with nothing but the sky above it,
  is darkened, damp; towards the puddles a film of water gathers, its
  reflection traced and sharpening as it deepens, and the puddles on the flat
  are mirrors. The wetter it is, the further the film spreads. Awnings,
  balconies, glass roofs and anything else overhead keep what is under them
  dry; a grate's holes let the rain through. 100 is soaked. Only the first
  surface the view meets is wet: what it reflects is shaded dry. `PT WET n`
  sets it for the session.
- `BumpMapping`: how deep walls' and floors' relief is drawn, in percent of
  what their materials give: 0, the default, for flat surfaces as the engine
  draws them, and 100 for the materials' relief as given. A texture's
  brightness is taken as its height - the dark gaps between bricks, stones
  and tiles sunk, their faces standing out - and the surface lit by its
  slope, measured a texel apart at the mip its footprint calls for, so it
  flattens with distance rather than sparkling. The materials give it by
  what a surface is made of: 3 world units for brick and stone, 1.5 for
  concrete, tiles and foliage, about 1 for metal and wood, none for glass,
  water, paper or skin, and none for anything the materials cannot place,
  so signs and posters stay flat. A fourth value in a material override
  sets a texture's own (see Materials). The detail texture adds a quarter
  unit of grain up close. Only flat surfaces, which carry their texture's
  direction across them: characters keep their own smooth normals. Standing
  water smooths it over. `PT BUMP n` sets it for the session.
- `HDR`: HDR output, False by default. On a display and a compositor that take
  it - scRGB on Windows with HDR switched on in its display settings, HDR10
  under a Wayland compositor; under Proton that needs
  `PROTON_ENABLE_WAYLAND=1 PROTON_ENABLE_HDR=1` in the game's launch options -
  the picture is drawn as SDR draws it up to the tone curve's shoulder, the
  HUD and menus with it, at `HDRPaperWhite` nits, and what SDR had to squeeze
  in below white spreads out above it towards `HDRPeakNits` (see HDR). Where
  neither is offered it stays SDR, and the log says what the surface did
  offer. `PT HDR` switches it for the session.
- `HDRPeakNits`: the display's peak brightness, 1000 by default: where the
  tone curve levels off. `PT HDRPEAK n`.
- `HDRPaperWhite`: how bright the SDR picture's white is drawn in HDR, 200
  nits by default - the HUD, a white wall in daylight. `PT HDRWHITE n`.
- `VR`: the game in a headset, through OpenXR, False by default (see
  Headsets). The runtime has to be running and the headset connected when
  the game starts: on Windows SteamVR, with a Quest over Link or Air Link,
  or the headset's own OpenXR runtime; under Proton the Linux one, reached
  through Proton's wineopenxr - played with WiVRn and a Quest 2 under
  Proton-CachyOS, with `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` in the
  game's launch options so Steam's container can see it, and with
  `PROTON_ENABLE_WAYLAND=1` for HDR on the screen as well - where Proton
  leaves the runtime's Vulkan extensions for the helper to look up itself,
  since winevulkan does not expand its stand-in for them under its Wayland
  driver. Proton asks the
  runtime about the headset once, as the game starts, so connect the
  headset first; `PT VR` can then switch it on and off. Where there is none,
  or it cannot draw on the game's GPU, the log says why and the game carries
  on on the screen. `openxr_loader.dll` goes beside the helper.
- `VRUnitsPerMetre`: the world's scale, 52.5 by default - Deus Ex's, in which
  JC is 94 units tall. Smaller makes the world feel bigger. `PT VR SCALE n`.
- `VRResolution`: each eye's picture as a percentage of the size the headset
  asks for, 100 by default. `PT VR RES n`.
- `VRHudDistance`: how far ahead the HUD's panels are, in metres, 1.5 by
  default. Their size in the view is the same whatever the distance; nearer
  they are sharper, but further in depth from what they mark. `PT VR HUD n`.
- `VRHudSize`: how wide the HUD is in the headset, in degrees across the
  box it is laid out in, 60 by default. `PT VR HUDSIZE n`.
- `VRShowHud`: the HUD in the headset, False by default: the parts the game
  can hide itself - the object belt, the health display, the ammo, the
  augmentations' icons and the compass, as its own Toggle commands do - are
  hidden while the headset shows the game, leaving the crosshair, the
  messages, conversations and menus, and shown again as it stops. Only the
  HUD's windows are hidden: the game's own settings for them, which it
  saves, are left as they are. `PT VR SHOWHUD`
  switches it, and can be bound to a key: `set input H PT VR SHOWHUD`.
- `VRDLSSQuality`, `VRBounces`: what the headset's pictures are traced with
  instead of `DLSSQuality` and `Bounces`: Ray Reconstruction at 3,
  performance (0 DLAA to 4 ultra performance), and one bounce by default.
  Two eyes at the headset's size and refresh are several times the work of
  the screen. The screen's own come back as VR goes off; while it is on,
  `PT DLSS quality` and `PT BOUNCES n` set these.
- `FPSLimit`: frames per second to hold the game to, 120 by default and 0 for
  no limit. Deus Ex cuts conversation audio short when left to run at a few
  hundred frames a second, the intro included. Unlike VulkanDrv's it does not wait
  for each frame to reach the screen, which would stop the CPU overlapping the
  GPU.

### Console commands

`PT` on its own lists them. Most are diagnostics:

- `PT LIGHTS`: the nearest lights that change or have an effect, with their type,
  effect and where they are relative to the view.
- `PT FOG`: whether the player's zone and the surface under the crosshair
  are fog zones, which the engine needs to draw any fog, then the nearest
  lights that glow in fog: where each is, how big its glow is, its zone,
  whether it can be seen from the eye, and how far it sees out itself along
  26 directions - which is what `FogShadows` holds its glow to.
- `PT HIGHLIGHT`: paints changing lights green, spotlights magenta and shaped
  lights cyan, at eight times their brightness.
- `PT WEAPON`, `PT LOOK`: log how the held weapon, or the actor under the
  crosshair, is drawn - its style, glow, skins, and every material's flags and
  texture, including what is in the texture and what it counts as being made
  of. For the level itself, `PT LOOK` names the surface's texture, its group,
  its material, its detail texture and its zone's ambient, and logs every light
  that reaches the spot: what the engine's lightmap takes from it and what the
  trace does, whether the level's own geometry blocks it, each baked light's
  shadow mask there, and which lights the map's build baked into the surface
  with how much of each it left lit, and reads the engine's own lightmap
  there, as the engine built it. Where
  the trace is darker than the other devices, this says which lights the
  engine lets through walls - a map lit before its geometry was finished. For
  an actor it lists the lights in its reach, with their own values and the
  trace's, and what a surface of it facing them would get. Looked at twice,
  it says how fast the texture there animates: its frames, its speed, and how
  many frames the trace took in between. `PT LOOK TIME` also logs the
  engine's lightmap at the spot once a frame for the next 480 frames, to time
  a light's effect against the engine's own.
- `PT VIEW NORMALS | DEPTH | MOTION | DIFFUSE | SPECULAR | EMISSION | ALBEDO |
  HITDIST | HISTORY | MATERIAL`: shows one of the denoiser's inputs, how many
  frames each pixel has averaged, or each surface's material (red roughness,
  green metalness, blue where it is glossy), in place of the picture. `PT VIEW`
  alone goes back.
- `PT DENOISE`: denoising on or off. Says what denoises.
- `PT DLSS [DLAA | QUALITY | BALANCED | PERFORMANCE | ULTRAPERFORMANCE]`: DLSS
  Ray Reconstruction on or off, or on at that quality; turns denoising on with
  it. Says whether it is running, and why not when NRD stands in. With VR on,
  the quality is the headset's (`VRDLSSQuality`).
- `PT FSR [OFF | AUTO | ON]`: FSR never, where Ray Reconstruction cannot run,
  or always; on its own, from one to the next. Its quality is `PT DLSS`'s.
- `PT LIGHTING [ENGINE | LINEAR]`: the level lit as `Lighting` says, or the
  other way.
- `PT EXPOSURE n`, `PT TONEMAP`: the exposure, as `Exposure`, and the tone
  curve, as `NeutralToneMap`.
- `PT BAKEDSHADOWS`: with the engine's lighting, the baked lights held to the
  map's shadow masks as well as traced (the default), or traced alone.
- `PT NOGLOW`: glowing surfaces - unlit ones, a sign, a light panel - still
  seen but lighting nothing, to see what they add to a room.
- `PT MESHLIGHT`: meshes lit as the engine lights them, or as flat surfaces are.
- `PT DETAIL`: detail textures on or off, as the game's setting.
- `PT MIPS`: mipmaps off and on, to compare: off, every texture is read at
  its full size however far away it is.
- `PT ANISOTROPY n`: the texture filter's anisotropy, as `MaxAnisotropy`.
- `PT WIDESCREEN`: the widescreen field of view on or off.
- `PT LIGHTSIZE n`: the size lights cast shadows from, as `LightSize`.
- `PT PINNEDUI 16:9 | 4:3 | OFF`: the UI kept to a box of that shape in the
  middle of the screen, or across all of it. Any ratio or number works.
- `PT FLASHLIGHT [n]`, `PT BEAM n`, `PT FOGSHADOWS`, `PT GLOW n`: the
  flashlight on or off or at a brightness, its beam's haze, the fog's
  shadows, and how much glowing surfaces light, as the settings of the same
  names.
- `PT GLASS`, `PT WET n`, `PT BUMP n`: light through glass tinted or not, how
  wet the streets are, and how deep the bump mapping, as `ColouredGlass`,
  `Wetness` and `BumpMapping`.
- `PT GLOWSAMPLING`: glowing surfaces found only by the bounces that reach
  them, as before, or sampled as lights as well, to compare. The two should
  settle to the same picture.
- `PT PHOTO`: photo mode on or off. The world is held still - the level's own
  players only switch, so nothing else moves, fires, talks or triggers, and
  every character and animated thing is held in the pose it had, since that
  switch does not stop the engine animating them - and the camera flies free
  with the movement keys and the mouse: jump and crouch rise and sink, and the
  walk key goes slowly. The HUD, the weapon and
  the screen flashes are left out, and JC stands where he was. While the
  camera moves the picture is denoised; once it has held still for a moment
  it refines instead, every frame's samples averaged, up to 4095 a pixel.
  Fire (or `PT PHOTO SAVE`) saves the picture as a PNG in `Photos`, beside
  `System`, named for the map and the time; the console says where, and how
  many samples it had. Every key but moving, looking, the console, the menu,
  pause and screenshots is set aside while it is on - QuickSave takes the
  photo - and `PT PHOTO`, or opening the menu or any screen, ends it and puts
  JC back as he was. It starts only while JC walks or swims, not in a
  conversation or a cutscene.
- `PT PHOTO APERTURE n`, `PT PHOTO FOCUS n | AUTO`: depth of field for photo
  mode. The aperture is the lens's radius in world units - 0, the default, is
  a pinhole with everything sharp, and 2 to 10 blur a room's background - and
  the focus is how far ahead is sharp, or by default whatever the middle of
  the view shows, JC included - looking through glass, smoke and grates as
  the view does, and far off at the sky.
- `PT NOLIGHTS`, `PT NOSHADOWS`, `PT NOSKY`, `PT NOFOG`, `PT OPAQUE`: switch
  one thing off to see what it costs or what it is doing. `PT MATERIALS`
  switches materials on or off (`PT NOMATERIALS` still works).
- `PT HDR`, `PT HDRPEAK n`, `PT HDRWHITE n`: HDR output on or off, and its
  peak and its white in nits, for the session (see `HDR`).
- `PT VR`: the headset on or off for the session, which starts the helper
  again and sends it the level anew. `PT VR SHOWHUD` shows or hides the HUD
  in it (see `VRShowHud`). `PT VR RECENTER` makes wherever the head
  is now straight ahead; `PT VR STATUS` says what the helper found; `PT VR
  SCALE n`, `PT VR RES n`, `PT VR HUD n` and `PT VR HUDSIZE n` set
  `VRUnitsPerMetre`, `VRResolution`, `VRHudDistance` and `VRHudSize` for the
  session.
- `PT ALLLIGHTS`: weighs every light in reach at every point, as it used to,
  rather than drawing from a crowded cell's lights (see Crowded lights), to
  compare the two by eye or by frame rate.
- `PT BOUNCES n`, `PT GLOSSBOUNCES n`, `PT RESET`. With VR on, `PT BOUNCES`
  sets the headset's (`VRBounces`).
- `PT BENCH`: hold still for about half a minute while it switches the
  costlier features off one at a time - the engine's lighting, the baked
  shadow masks, mipmaps, anisotropic filtering, detail textures, the engine's
  mesh lighting, soft shadows, glowing surfaces lighting the room and being
  sampled as lights, the fog's shadows, the flashlight (against the game's
  own light augmentation), glass's colour, wet streets, bump mapping, the
  extra bounces, shadows - and then two that are there as bounds rather than
  features: every see-through surface made solid (`PT OPAQUE`) and no lights
  at all (`PT NOLIGHTS`), the most that making either cheaper could give
  back - and last every light weighed (`PT ALLLIGHTS`), what drawing from
  crowded cells saves. The frame limit and VSync are off meanwhile. It then logs a table to
  `PathTracerTimings.log`: the GPU's time, the scene gathering and the frame
  with each, and how each compares with the settings as they are. `PT BENCH`
  again stops it.
- `PT TILES`: logs every 2D draw of the next frame - each tile and mesh the
  HUD and the menus hand the device, and each view - to find what it leaves
  out.

The game's own `ShowHud 0` (and `ShowHud 1`) hides the HUD, for screenshots.

### What it does not do yet

- Two mirrors facing each other show one reflection each rather than a corridor,
  while denoising.
- A character's motion vectors follow the whole character, not its animation.
  No ghosting has been seen, but it is not exact.
- Where a map's lightmaps take light from lamps behind its walls, the trace
  keeps those shadowed, and the spot is darker; `PT LOOK` at a surface lists
  which.
- Some lockers come out about three times as bright as in the rasterised
  game; not yet looked into.
- In a headset: no motion controllers - the mouse and keyboard play it, and
  the head only looks. A scope or the binoculars do not magnify, since the
  headset's view is the headset's. A conversation's camera cuts move the whole
  room. Photo mode is for the screen. Under upstream wine there is no
  headset: wineopenxr is Proton's.

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

Build this way to test behaviour, not to measure it. This configuration
carries `/Zi`, links with `/DEBUG` and passes no `/GL`, and it read the scene
out of the engine about a millisecond a frame slower than the shipped build -
3.1 ms against 2.1 for the same frame - though the trace, the helper's work,
was the same. The path tracer's releases are cross built from Linux, as
[cmake/README-crossbuild.md](cmake/README-crossbuild.md) describes, and
VulkanDrv's and the Direct3D devices' come from the solution's `DeusExRelease`
configuration. Benchmark the binaries that ship.

### Building the denoisers

Neither is in this repository - their licence does not allow them to be
redistributed here. `cmake/build-nrd.sh` fetches a pinned NRD release and builds
it as a 64 bit library for the helper (and a 32 bit one as well), and
`cmake/fetch-dlss.sh` fetches a pinned release of the DLSS SDK: NGX's loader,
which the helper links, and Ray Reconstruction's runtime, `nvngx_dlssd.dll`,
which goes beside it. `cmake/fetch-fsr.sh` fetches AMD's FidelityFX SDK v1.1.4
and compiles FSR 3.1's shaders, which the helper builds in. Without any of
them the path tracer still builds and runs, just without that denoiser or
upscaler. See
[cmake/README-crossbuild.md](cmake/README-crossbuild.md).

## PathTracerDrv in Unreal Tournament

The same device, built for Unreal Tournament with OldUnreal's
[469 patch](https://github.com/OldUnreal/UnrealTournamentPatches), and played
from Steam under Proton on the patch's 64 bit build of 469f, its fifth release
candidate.

> **Single player, and your own servers, only.** Joining a server that runs
> ACE, the anti-cheat most public servers use, with this device gets you
> kicked, and on some servers banned. Play against bots, or on a server you
> run without ACE.

### Installing it in UT

Copy `PathTracerDrv.dll`, `PathTracerDrv.int` and `nvngx_dlssd.dll` into the
game's `System` folder and choose PathTracer as the renderer in the game's
video preferences, or set `GameRenderDevice=PathTracerDrv.PathTracerRenderDevice`
in the `[Engine.Engine]` section of `UnrealTournament.ini`. On Linux,
`cmake/deploy-ut.sh "" pathtracer` copies them into the Steam install and sets
it. There is no `PathTracerHelper.exe` to copy: the game is 64 bit, which is
offered ray tracing everywhere, so the tracing runs in its own process, on the
device's own Vulkan device - the helper's work, done in place
(`TraceHost`). It needs the same GPU as in Deus Ex, and DLSS Ray
Reconstruction and NRD denoise as they do there.

### What is different from Deus Ex

- **The view.** 469 widens the view for a wide screen itself, and the trace
  follows the engine's projection, so `WidescreenFOV` is off by default here
  rather than widening it twice. The HUD is pinned to 4:3 as in Deus Ex;
  `PT PINNEDUI OFF` gives 469's own HUD, which is laid out for any width.
- **No frame limiter.** Deus Ex needs one to keep its conversations whole; UT
  does not, and `FPSLimit` is 0.
- **The weapon in your hands** is put where UT's `RenderOverlays` puts it -
  469's `CalcDrawOffset`, with its field of view corrections, the bob and the
  hand it is held in - and only once the engine has placed it for the frame
  being traced. Taken earlier, a client of a server has the server's idea of
  where it is, at the body's middle, which put it out of sight below the view.
- **Meshes with an empty skin slot** are drawn as 469 draws them, with what it
  would environment map them with or else the mesh's last texture. UT's traffic
  cones, toolboxes, bins and tyres need it; they came out white without it.
- **469's own render device interface.** The device is one of OldUnreal's
  `URenderDeviceOldUnreal469` devices rather than an old one: the engine wraps
  an old device in a proxy, whose `Exit` called on into a device already gone
  as the game shut down. Its text comes premultiplied (`PF_Highlighted`), and is
  drawn so.
- **Photo mode** is for a game played alone: in a network game the server has
  the world, and the player, its own way.
- What is Deus Ex's alone does nothing: the light augmentation's flashlight,
  the vision augmentation, and views in windows of the HUD.

### What it does not do yet in UT

- The menus' model previews - the player setup's turning character - draw
  nothing. The engine draws them from an actor in its entry level, which the
  trace of the level being played does not have.
- A texture in one of 469's newer formats (BC2 to BC7, 16 bit) is drawn white,
  and the log names it; the stock game's are palettised, BGRA or BC1.
- It has not been benchmarked yet, nor tried on 32 bit 469, where it would
  start the helper as Deus Ex does.

It is built by the Unreal Tournament configuration in
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
- **S3TC textures** - New Vision's - were uploaded without the Vulkan device's
  BC texture compression enabled, which NVIDIA's driver tolerates and the
  Vulkan specification does not allow. ZVulkan enables it now.

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
- **A widescreen field of view** (`WidescreenFOV`, on by default; `VK WIDESCREEN`
  switches it for the session). On a screen wider than 4:3 the engine keeps
  the width and crops the top and bottom, so at 21:9 conversations and
  cinematics framed for 4:3 lose heads and feet. A device cannot widen the view
  by itself: the engine culls the level, and lights it, for its own view before
  handing anything over. So the engine is made to compute the wider view
  itself: every view's projection and the edges it culls against come from one
  function in `Engine.dll`, `FSceneNode::ComputeRenderSize`, which VulkanDrv
  redirects to call with the field of view that keeps 4:3's height, handing the
  player's own angle back afterwards. The engine then culls, lights and draws
  the wider view as it would any other; the game's scripts never see the wider
  angle, so zooming and mouse sensitivity work as before. It only redirects the
  jump the 1112fm `Engine.dll` has there, and leaves any other alone. Keep the
  game's own field of view at its 4:3 value (75 by default). The engine does
  more work for the wider view.
- **A pinned HUD** (`PinnedUI`, 4:3 by default; `VK PINNEDUI 16:9`,
  `VK PINNEDUI 4:3` and `VK PINNEDUI OFF` switch it for the session), as the
  path tracer has it. In fullscreen the game is given a mode of that shape at
  the height chosen and lays its HUD, menus and conversations out in it as it
  was designed to, while the world fills the rest of the mode chosen around
  them; what spans the game's whole width - a conversation's bars, a fade, the
  darkening behind a menu - is carried on to the screen's edges, and so is the
  black round a scope's or the binoculars' sight. The world
  either side comes from the engine too: `URender::DrawWorld` in `Render.dll`
  is redirected the same way, and for the player's view alone the frame is
  widened to the screen at the focal length it already had, given a culling
  buffer of that width, drawn, and put back as it was for the HUD. The sky,
  mirrors and warp zones drawn inside it follow the same focal length; the
  cameras' and the spy drone's insets stay in the box. The game lists the
  narrower mode as its resolution, but the ini keeps the one chosen. 0 lays
  the UI across the whole width.
- **Anisotropic filtering** (`MaxAnisotropy`), **sRGB textures**
  (`SRGBTextures`), and 4:3 and 16:9 modes letterboxed at the monitor's full
  height.
- **Surviving a lost device**, rebuilding everything under the instance rather
  than taking the process with it.

Recommended in `[VulkanDrv.VulkanRenderDevice]`:

	RenderScale=2.000000
	AntialiasMode=Off
	FPSLimit=120
	MaxAnisotropy=16.000000

Raise `RenderScale` until the cost shows rather than turning on MSAA as well:
the game is bound by its own engine long before the GPU. The scene buffers
also cost the game address space, of which a 32 bit process has 4 GB at most:
at 3440x1440 with 4x MSAA, `RenderScale=4` left some 430 MB less free than
`RenderScale=2`. `VulkanDrvEvents.log` records how much is free at each change
of buffer size, and any fault or C++ exception with the stack that raised it,
since the engine's own report of a crash leaves out what went wrong.

The rest of its settings, from upstream: `UseVSync`, `Hdr`, `HdrScale`, `Bloom`,
`BloomAmount`, `Saturation`, `Contrast`, `LinearBrightness`, `GammaMode`,
`GammaOffset`, `LightMode`, `LODBias`, `VkDeviceIndex`, `VkDebug`,
`VkExclusiveFullscreen`. `GetVkDevices` in the console lists the GPUs to choose
from.

## New Vision

[New Vision](https://www.moddb.com/mods/new-vision) replaces the game's world
textures with versions at up to eight times their size, and fourteen of its maps
with its own. Its packages keep each original texture and carry an S3TC
(DXT1) version beside it, which the engine hands only to a device that asks
for S3TC. Every device here does: VulkanDrv, D3D11Drv and D3D12Drv with no
setting, PathTracerDrv with `UseS3TC`, on by default.

To install it, copy its `NewVision` folder into the game's folder, beside
`System`, and add its two paths to the `[Core.System]` section of the ini -
**before** the stock `Maps` and `Textures` lines:

	Paths=..\System\*.u
	Paths=..\NewVision\Maps\*.dx
	Paths=..\NewVision\Textures\*.utx
	Paths=..\Maps\*.dx
	Paths=..\Textures\*.utx

The engine loads a package from the first path that has one, so added after
the stock lines, as New Vision's own readme says, the stock packages are
found first and nothing changes, in any device. Removing the two lines puts
the game back as it was.

In the path tracer the S3TC textures go to the GPU still compressed - a
level's worth unpacked runs to gigabytes - and are read off disk only long
enough to be sent, so the game's 32 bit process is not left holding them.
Masked ones are unpacked, so their holes work as they did. A level takes a
little longer to load. `PT LOOK` at a wall says which file its texture came
from and the size of its S3TC version, and the log's `PathTracer textures:`
line counts how many went as S3TC. `UseS3TC=False` goes back to the original
textures while leaving the paths alone.

## Building

On Linux, the CMake cross build in [cmake/README-crossbuild.md](cmake/README-crossbuild.md)
builds every device with clang-cl against Microsoft's CRT and SDK, and documents
the Deus Ex specific behaviour worth knowing before filing a bug against a
renderer. It is the only build of PathTracerDrv, for Unreal Tournament as well
(`PATHTRACER_GAME=UT469`, against OldUnreal's 469 SDK, which is not included
here).

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
