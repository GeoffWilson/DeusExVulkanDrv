#include "TracePrecomp.h"
#include "Shaders.h"

// The curve that brings the linear picture into the display's range, shared
// by every pass that can finish it. Reinhard's compresses everything, the
// darks least; the neutral one leaves all but the brightest fifth as it is,
// as the engine's own devices draw it, and bends only that fifth towards white
// - a shoulder rather than the devices' hard clip.
//
// ceiling is where the neutral curve levels off, in the white the SDR picture
// is drawn at: 1 for SDR, and on an HDR display its peak over that white. The
// part below the shoulder is the same either way, so HDR changes nothing but
// what SDR had to squeeze in below white - the lamps, the neon, the sun on a
// wall - which spreads out above it towards the display's peak. Reinhard's
// has no such part, compressing the darks as well, so HDR takes the neutral
// curve whichever is chosen.
static std::string ToneMapGlsl()
{
	return R"(
		vec3 toneMap(vec3 c, bool neutral, float ceiling)
		{
			c = max(c, vec3(0.0));
			if (!neutral && ceiling <= 1.0)
				return c / (c + vec3(1.0));
			ceiling = max(ceiling, 1.0);
			const float start = 0.8;
			float peak = max(c.r, max(c.g, c.b));
			if (peak < start)
				return c;
			float d = ceiling - start;
			float newPeak = ceiling - d * d / (peak + d - start);
			c *= newPeak / peak;
			float g = 1.0 - 1.0 / (0.15 * (peak - newPeak) + 1.0);
			return mix(c, vec3(newPeak), g);
		}
	)";
}

// Everything the trace and the fog's shadow pass share: the bindings, the
// push constants and the functions. Each adds its own main.
static std::string TraceCommon()
{
	// MSVC caps a single string literal at 16384 bytes - clang-cl does not -
	// and this shader is four times that, so the GLSL is carried in pieces and
	// joined here. The splits are only there to stay under the cap and mean
	// nothing to the shader; each one falls on a blank line between statements,
	// and a new one is needed whenever a piece grows past the cap.
	std::string source = R"(
		#version 460
		#extension GL_EXT_ray_query : enable
		// Each ray lands on whatever triangle it lands on, so the texture index
		// differs between neighbouring invocations.
		#extension GL_EXT_nonuniform_qualifier : enable

		layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

		layout(binding = 0) uniform accelerationStructureEXT topLevel;
		// The view's own images, a set of their own so that the views the HUD
		// draws in windows are traced with the same scene: the average so far,
		// the picture, and - xyz - the world position each pixel hit last
		// frame, w which instance owned it, or -1 for the sky.
		layout(set = 1, binding = 0, rgba32f) uniform image2D accumImage;
		layout(set = 1, binding = 1, rgba16f) uniform image2D outImage;
		layout(set = 1, binding = 2, rgba32f) uniform image2D historyImage;

		struct TriangleAttributes
		{
			vec4 Normal;
			vec4 Albedo;
			vec4 Emission;
			vec4 Ambient;
			vec4 UV01;      // u0 v0 u1 v1
			vec4 UV2Tex;    // u2 v2 texture unused
			uvec4 CornerNormals;   // a mesh's smoothed normal at each corner, packed; w 1 when there are any
			uvec4 CornerOffsets;   // each corner's neighbours off its tangent plane, half pairs; on a flat surface, its detail texture
		};

		struct SceneLight
		{
			vec4 PositionRadius;
			vec4 ColorBrightness;
			vec4 DirectionCone;   // xyz spot direction, w cosine of its edge or -1
			vec4 Flags;           // x no incidence, y cylinder, z brightness changes, w pattern: 0 disco 1 searchlight 2 rotor, -1 none
			vec4 Peak;            // x the most its brightness gets, which the light grid ranks it by
		};

		layout(binding = 3, std430) readonly buffer Attributes { TriangleAttributes tris[]; };
		layout(binding = 4, std430) readonly buffer Lights { SceneLight lights[]; };
		// Per instance, indexed by the intersection's instance id: what varies
		// by where a shape is rather than by what it is.
		layout(binding = 5, std430) readonly buffer InstanceData { vec4 instanceAmbient[]; };
		// Which lights reach which cell of the level: see WriteLightGrid.
		layout(binding = 8, std430) readonly buffer LightGrid { uint lightGrid[]; };
		// The engine's shadow masks on the level's lightmapped surfaces: see
		// SceneData's Lightmaps.
		layout(binding = 25, std430) readonly buffer Lightmaps { uint lightmapData[]; };
		// What each texture is made of, indexed as the texture array is: x
		// roughness, y metalness, z reflectance face on. See Materials.h.
		layout(binding = 20, std430) readonly buffer MaterialData { vec4 materials[1024]; };

		// What a denoiser needs, written each frame when asked for (Disable bit
		// 64): the first solid surface's normal and roughness, its depth and
		// motion, the lighting on it split from its colour, what the path
		// picked up before reaching it, and the colours that put the lighting
		// back together:
		//   picture = emission + diffuseAlbedo * diffuse + specularAlbedo * specular
		layout(binding = 9, rgba16f) uniform writeonly image2D guideNormalImage;       // normal and roughness, packed as NRD packs them
		layout(binding = 10, rgba32f) uniform writeonly image2D guideDepthMotionImage; // view z (huge where there is no surface), motion in uv, surface found
		layout(binding = 11, rgba16f) uniform writeonly image2D diffuseImage;          // demodulated, hit distance
		layout(binding = 12, rgba16f) uniform writeonly image2D specularImage;         // what a mirror shows: demodulated, hit distance
		layout(binding = 13, rgba16f) uniform writeonly image2D emissionImage;
		layout(binding = 14, rgba16f) uniform writeonly image2D diffuseAlbedoImage;
		layout(binding = 15, rgba16f) uniform writeonly image2D specularAlbedoImage;
		// The volumetric fog over this pixel, blended on after the denoiser.
		layout(binding = 17, rgba16f) uniform writeonly image2D fogImage;
		// What a mirror shows, described to the denoiser as a surface of its
		// own, where it appears to be behind the glass.
		layout(binding = 18, rgba16f) uniform writeonly image2D reflectionNormalImage;
		layout(binding = 19, rgba32f) uniform writeonly image2D reflectionDepthMotionImage;
		// The glossy reflection off the first solid surface, demodulated, with
		// its hit distance, and the colour that puts it back:
		//   picture += glossAlbedo * gloss
		layout(binding = 21, rgba16f) uniform writeonly image2D glossImage;
		layout(binding = 22, rgba16f) uniform writeonly image2D glossAlbedoImage;
		// Last frame's camera, as the push constants carry it, this frame's
		// fixed jitter in xy (see Disable bit 256), GlowLighting in z and
		// Wetness in w, the flashlight (see
		// flashlightAt), which way the sky zone faces (TraceCommand's
		// SkyAxes), photo mode's lens (PhotoLens), the relief's depth in x
		// (BumpMapping), where the middle of the view is (viewShift), then
		// each instance's last placement as three rows.
		layout(binding = 16, std430) readonly buffer Motion { vec4 previousCamera[4]; vec4 frameJitter; vec4 flashlight[3]; vec4 skyAxes[3]; vec4 photoLens; vec4 surfaceRelief; vec4 frustumShift; vec4 previousRows[]; };
		// Each fog light's shadow cube, written by the pass before the trace
		// (Shaders::FogShadows) and read by volumetricFog.
		layout(binding = 26, std430) buffer FogShadows { float fogShadow[]; };
		// The level's glowing surfaces as lights, and the grid of which to
		// sample where: see EmitterGrid.h and glowLight.
		layout(binding = 27, std430) readonly buffer Emitters { uint emitterData[]; };
		// DLSS Ray Reconstruction's own inputs, written in place of NRD's when
		// asked for (Disable bit 256): the depth and the motion, each on its
		// own. The rest share NRD's images, written differently for it.
		layout(binding = 23, r32f) uniform writeonly image2D rrDepthImage;
		layout(binding = 24, rg16f) uniform writeonly image2D rrMotionImage;
		// Sized to match the layout rather than left open: an unsized array needs
		// the runtime descriptor array capability, and a device without it would
		// fail to create the pipeline at all rather than simply not texture.
		layout(binding = 6) uniform sampler2D sceneTextures[1024];

		layout(push_constant) uniform PushConstants
		{
			vec4 CameraOrigin;    // xyz world position of the eye
			vec4 CameraRight;     // xyz, already scaled by the horizontal half extent
			vec4 CameraUp;        // xyz, already scaled by the vertical half extent
			vec4 CameraForward;   // xyz unit vector down the middle of the view
			uvec4 Counts;         // x frame, y light count, z bounces (glossy bounces << 8, light radius << 16, mip bias in signed sixteenths << 24), w accumulated frames
			vec4 Params;          // x exposure, y sky intensity, z 1 for the engine's lighting (see directLight), w debug mode
			uint TextureCount;    // 0 when the device cannot index the array
			uint MaxSamples;      // ceiling on samples averaged into one pixel
			float Time;           // the level's clock, for panning textures
			uint Disable;         // diagnostic switches: 2097152 every light in a cell weighed, 1 lights, 2 shadows, 4 sky, 8 per-triangle checks, 32 fog, 128 materials, 1024 meshes lit as flat surfaces, 2048 detail textures, 4096 mipmaps, 8192 the neutral tone curve, 512 glowing surfaces lighting nothing, 16384 the engine's shadow masks, 32768 a view in a window of the HUD's, 65536 fog's shadows, 131072 the flashlight, 262144 glowing surfaces sampled as lights; 64 write NRD's inputs, 256 Ray Reconstruction's, 524288 photo mode's accumulation, 1048576 light untinted by glass, 4194304 the frame's jitter for an upscaler
			vec4 SkyOrigin;       // xyz the sky zone's viewpoint, w 1 when there is one
		};
	)";

	source += R"(

		#define GlossBounces ((Counts.z >> 8u) & 255u)
		// How much a glowing surface gives what it lights: GlowLighting.
		#define GlowScale frameJitter.z
		#define Wetness frameJitter.w
		#define BumpStrength surfaceRelief.x
		// Where the tone curve levels off: 1, or an HDR display's peak over
		// the SDR picture's white (toneMap).
		#define ToneCeiling surfaceRelief.y
		// Where the middle of this view is, off the axis CameraForward points
		// along, in the view's half widths and heights: 0 but for a headset's
		// eye, whose view reaches further to one side than the other. xy this
		// frame's, zw last frame's. A view in a window of the HUD's has none.
		#define viewShift ((Disable & 32768u) != 0u ? vec4(0.0) : frustumShift)
		#define LightRadius float((Counts.z >> 16u) & 255u)
		#define MipBias (float(int(Counts.z) >> 24) / 16.0)
		// Whether the level's surfaces take their lights as the engine's
		// lightmaps do, or each by a straight line out to its radius in
		// linear light: see directLight.
		#define EngineLighting (Params.z > 0.5)

		// How far a ray starts from the surface it leaves, in world units:
		// these levels are big.
		const float RayEpsilon = 0.5;
		// How far along it a ray lifted RayEpsilon off the surface it leaves
		// starts looking. The lift already keeps it off that surface - the
		// ray leaves above it - so this is only a hair. Starting RayEpsilon
		// along as well stepped through whatever else was that close: from
		// the floor beside a wall, a bounce towards the wall went through it,
		// and brought the light behind back as a bright line along every
		// corner and every foot of a wall.
		const float LiftedRayMin = 0.01;

		// Which instances each kind of ray sees, against SceneInstance::Mask.
		// The view's own rays and shadows miss the viewer's body while the
		// camera is inside it (0x12); whatever has bounced - a mirror's view
		// among them - misses the first person weapon (0x04); shadows miss a
		// light fitting with its lamp inside it (0x18). A view in a window of
		// the HUD's sees only what carries 0x10: not the weapon, and not the
		// camera it is seen from (SceneData's InstanceMask).
		const uint ViewRays = 0xEDu;
		const uint BouncedRays = 0xFBu;
		const uint ShadowRays = 0xE5u;
		const uint WindowRays = 0x10u;
		// The flashlight's shadows miss the player's body and the weapon in
		// the player's hands, which it shines from among, but not a light
		// fitting: there is no lamp of the flashlight's inside one (0xE9).
		const uint FlashlightRays = 0xE9u;
		// Rain, looked for straight up from wet ground (wetnessAt): stopped
		// by what shadows are, but not by the player's body or the weapon at
		// the player's eyes, which would leave a dry patch wherever the
		// player stood.
		const uint RainRays = 0xE9u;

		// Does this point on the triangle actually exist? UE1 masked art keys
		// transparency to palette index zero, which the upload turns into an
		// alpha of zero. Rendering those texels rather than seeing through them
		// is what put magenta - the key colour in Deus Ex's packages - across
		// every window and railing.
		// The triangle's own colour, textured where there is a texture to read.
		// UE1 surface coordinates run well outside 0..1 - one texture tiles
		// across a whole wall - which the sampler's repeat mode handles.
		// Where on its texture a hit lands.
		//
		// Normal.w marks an environment mapped surface - a camera lens, a pair
		// of glasses - whose texture is a picture of surroundings looked up by
		// the reflected view direction rather than painted on. The engine's own
		// mapping: the world space reflection's X and Y, from -1..1 to 0..1.
		vec2 surfaceUV(TriangleAttributes attr, vec2 bary, vec3 dir, vec3 worldNormal)
		{
			if (attr.Normal.w > 0.5)
			{
				vec3 r = reflect(normalize(dir), worldNormal);
				return (r.xy + 1.0) * 0.5;
			}

			// Barycentrics from a ray query are the weights of the second and
			// third vertices; the first takes up the remainder.
			// Emission.xy is how fast an auto panning surface slides, in
			// texture widths a second, and zero for everything else.
			vec2 uv = attr.UV01.xy * (1.0 - bary.x - bary.y)
			        + attr.UV01.zw * bary.x
			        + attr.UV2Tex.xy * bary.y
			        + attr.Emission.xy * Time;

			// The engine's small wave (PF_SmallWavy, which marks the surface
			// with an Albedo.w of 2): its texture swaying on the level's clock
			// by 8 sin t + 4 cos 2.3t texels across and 8 cos t + 4 sin 2.3t
			// down, as Render.dll pans it, in widths of the texture's own
			// size - the material's w, its width plus 4096 times its height.
			if (attr.Albedo.w > 1.5)
			{
				int index = int(attr.UV2Tex.z);
				if (index >= 0 && index < 1024)
				{
					float packed = materials[index].w;
					vec2 size = vec2(mod(packed, 4096.0), floor(packed / 4096.0));
					if (size.x > 0.0 && size.y > 0.0)
						uv += vec2(8.0 * sin(Time) + 4.0 * cos(2.3 * Time), 8.0 * cos(Time) + 4.0 * sin(2.3 * Time)) / size;
				}
			}
			return uv;
		}

		// Which mip level a hit samples its texture at, from how wide the ray's
		// footprint is where it lands (Akenine-Moller et al., "Texture Level
		// of Detail Strategies for Real-Time Ray Tracing", Ray Tracing Gems
		// chapter 20). footprint is log2 of that width (footprintOf); the
		// triangle says how much of its texture lies across a unit of it
		// (SetUvDensity), and the texture's size turns that into texels. An
		// environment map is looked up by direction rather than laid on, and
		// keeps its top level.
		float surfaceLod(TriangleAttributes attr, int index, float footprint)
		{
			if (attr.CornerOffsets.w == 0u || attr.Normal.w > 0.5)
				return 0.0;
			ivec2 size = textureSize(sceneTextures[nonuniformEXT(index)], 0);
			return max(uintBitsToFloat(attr.CornerOffsets.w) + 0.5 * log2(float(size.x) * float(size.y)) + footprint, 0.0);
		}

		// log2 of the width of a ray's footprint where it meets a surface, as
		// surfaceLod wants it: the cone's width there, widened for the slant by
		// the square root of the stretch - between the two axes of the ellipse
		// it really covers, which keeps a floor seen at a glance from blurring
		// without leaving it to shimmer - and measured in the geometry's own
		// space, where the triangle's density was: an instance scaled up
		// spreads its texture over more of the world.
		float footprintOf(float width, vec3 faceNormal, vec3 direction, mat4x3 objectToWorld, vec3 objectNormal)
		{
			mat3 m = mat3(objectToWorld);
			float areaScale = length(cross(m[1], m[2]) * objectNormal.x + cross(m[2], m[0]) * objectNormal.y + cross(m[0], m[1]) * objectNormal.z);
			float slant = max(abs(dot(faceNormal, direction)), 0.05);
			return log2(max(width, 1.0e-4)) - 0.5 * log2(slant) - 0.5 * log2(max(areaScale, 1.0e-8));
		}

		vec3 unpackUnitVector(uint packed);

		// A ray's footprint where it lands, for reading textures. logWidth is
		// footprintOf's, for surfaceLod; on a flat surface, dx and dy are the
		// footprint's two axes in texture coordinates, for textureGrad.
		struct Footprint
		{
			float logWidth;
			bool graded;
			vec2 dx;
			vec2 dy;
		};

		// The footprint's axes in texture coordinates, on a flat surface -
		// one that carries its texture's gradients (SetUvDensity): across the
		// ray it is the cone's width, along it the width stretched by the
		// slant, and the sampler filters anisotropically along the longer, at
		// the level the shorter calls for. A floor seen down a corridor is a
		// pixel wide one way and twenty texels the other; one level for both
		// either blurred it or left it sparkling.
		bool footprintAxes(TriangleAttributes attr, float width, vec3 faceNormal, vec3 direction, mat3 worldToObject, out vec2 dx, out vec2 dy)
		{
			dx = vec2(0.0);
			dy = vec2(0.0);
			if (attr.CornerNormals.w != 0u || attr.CornerNormals.z == 0u)
				return false;
			vec2 lengths = unpackHalf2x16(attr.CornerNormals.z);
			vec3 tu = unpackUnitVector(attr.CornerNormals.x) * lengths.x;
			vec3 tv = unpackUnitVector(attr.CornerNormals.y) * lengths.y;
			vec3 along = direction - dot(direction, faceNormal) * faceNormal;
			float l = length(along);
			along = l > 1.0e-4 ? along / l : normalize(cross(faceNormal, abs(faceNormal.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
			vec3 across = cross(faceNormal, along);
			float slant = max(abs(dot(faceNormal, direction)), 0.01);
			vec3 a0 = worldToObject * (along * (width / slant));
			vec3 a1 = worldToObject * (across * width);
			dx = vec2(dot(tu, a0), dot(tv, a0));
			dy = vec2(dot(tu, a1), dot(tv, a1));
			return true;
		}
	)";

	source += R"(
		// A texel of a hit's texture at the footprint's size: filtered along
		// it where the surface is flat, at one level where it is not, at the
		// top level with mipmaps switched off (Disable bit 4096). scale
		// stretches the texture's coordinates, for a detail texture laid on
		// finer than the surface's own.
		vec4 sampleFootprint(TriangleAttributes attr, int index, vec2 uv, Footprint footprint, vec2 scale)
		{
			if ((Disable & 4096u) != 0u || attr.Normal.w > 0.5)
				return textureLod(sceneTextures[nonuniformEXT(index)], uv, 0.0);
			if (footprint.graded)
				return textureGrad(sceneTextures[nonuniformEXT(index)], uv, footprint.dx * scale, footprint.dy * scale);
			return textureLod(sceneTextures[nonuniformEXT(index)], uv, surfaceLod(attr, index, footprint.logWidth + 0.5 * log2(max(scale.x * scale.y, 1.0e-8))));
		}

		vec3 surfaceAlbedo(TriangleAttributes attr, vec2 bary, vec3 dir, vec3 worldNormal, Footprint footprint)
		{
			int index = int(attr.UV2Tex.z);
			if (index < 0 || uint(index) >= TextureCount)
				return attr.Albedo.rgb;

			vec2 uv = surfaceUV(attr, bary, dir, worldNormal);

			vec4 texel = sampleFootprint(attr, index, uv, footprint, vec2(1.0));

			// The engine's art is authored in sRGB; the trace works in linear.
			vec3 linearRgb = pow(max(texel.rgb, vec3(0.0)), vec3(2.2));
			return linearRgb;
		}

		// What a surface's detail texture makes of its colour, depth units in
		// front of the eye: the fine grain that stops a wall going to mush when
		// you stand at it. As Deus Ex's Direct3D renderer lays it on: up to
		// three passes over the finished surface, each modulating it by twice
		// the detail texture blended towards mid grey, the first at the
		// texture's own scale out to 380 units, each after at 4.223 times the
		// scale and out to a 4.223th of the distance. A pass fades in from its
		// edge, 100 * (edge / depth - 1) out of 255 at a vertex. A flat
		// surface's CornerOffsets name the texture (LevelScene::SetDetail).
		// The passes are over the displayed colour, so the linear one takes
		// them to the power 2.2.
		vec3 detailFactor(TriangleAttributes attr, vec2 uv, float depth, Footprint footprint)
		{
			if (attr.CornerNormals.w != 0u || attr.CornerOffsets.x == 0u || (Disable & 2048u) != 0u)
				return vec3(1.0);
			uint index = attr.CornerOffsets.x - 1u;
			if (index >= TextureCount)
				return vec3(1.0);
			// Read over the surface's own footprint, stretched as the detail
			// texture is laid on finer than the surface's, and each pass
			// finer again.
			vec2 scale = uintBitsToFloat(attr.CornerOffsets.yz);
			vec3 factor = vec3(1.0);
			float edge = 380.0;
			for (int pass = 0; pass < 3 && depth < edge; pass++)
			{
				float a = clamp(100.0 / 255.0 * (edge / max(depth, 1.0) - 1.0), 0.0, 1.0);
				vec3 detail = sampleFootprint(attr, int(index), uv * scale, footprint, scale).rgb;
				factor *= 2.0 * mix(vec3(128.0 / 255.0), detail, a);
				scale *= 4.223;
				edge *= 0.2368;
			}
			return pow(factor, vec3(2.2));
		}

		// A unit vector as LevelScene packs it: octahedral, 16 bits a component.
		vec3 unpackUnitVector(uint packed)
		{
			vec2 e = unpackSnorm2x16(packed);
			vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
			if (n.z < 0.0)
				n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
			return normalize(n);
		}

		// The normal to shade a hit with. The engine lights a mesh at its
		// vertices, each with the average of the faces around it, and blends
		// the light across each face, so its meshes look rounded where the
		// triangles are flat; the corners' normals are blended the same way
		// here. Anything else is as flat as its face. faceNormal is the face's
		// own, already turned towards the ray, and side is which way it was
		// turned; the corners are turned with it.
		//
		// lifted is where rays leaving the hit should start: the point moved
		// off the flat triangle onto the rounded surface the corner normals
		// describe (Hanika, "Hacking the Shadow Terminator"). A ray started on
		// the flat triangle, towards a light the smoothed normal says it faces
		// but the face itself does not, runs into the mesh's own neighbouring
		// faces, and the shadow's edge comes out as square as the faces were.
		vec3 smoothNormal(TriangleAttributes attr, vec2 bary, mat3 toWorld, vec3 faceNormal, float side, vec3 position, out vec3 lifted)
		{
			lifted = position;
			if (attr.CornerNormals.w == 0u)
				return faceNormal;

			float w0 = 1.0 - bary.x - bary.y;
			vec3 n0 = unpackUnitVector(attr.CornerNormals.x) * side;
			vec3 n1 = unpackUnitVector(attr.CornerNormals.y) * side;
			vec3 n2 = unpackUnitVector(attr.CornerNormals.z) * side;
			vec3 shading = toWorld * (n0 * w0 + n1 * bary.x + n2 * bary.y);
			if (dot(shading, faceNormal) <= 1.0e-6)
				return faceNormal;
			shading = normalize(shading);

			// How far below each corner's tangent plane the hit is, from how
			// far its neighbours are: the hit is the corners' weighted sum.
			vec2 o0 = unpackHalf2x16(attr.CornerOffsets.x) * side;
			vec2 o1 = unpackHalf2x16(attr.CornerOffsets.y) * side;
			vec2 o2 = unpackHalf2x16(attr.CornerOffsets.z) * side;
			float below0 = min(o0.x * bary.x + o0.y * bary.y, 0.0);
			float below1 = min(o1.x * w0 + o1.y * bary.y, 0.0);
			float below2 = min(o2.x * w0 + o2.y * bary.x, 0.0);
			// Only straight out of the face. The rounded surface also runs
			// sideways past the face's edges, and following it there put the
			// start of a ray from the bottom of a box on the floor under the
			// floor, in the dark.
			vec3 lift = -(toWorld * (n0 * (w0 * below0) + n1 * (bary.x * below1) + n2 * (bary.y * below2)));
			lifted = position + faceNormal * max(dot(lift, faceNormal), 0.0);
			return shading;
		}

		// A direction chosen about a smoothed normal can point into the face
		// it was chosen on; mirrored back out of it, it leaves as it should.
		vec3 aboveFace(vec3 direction, vec3 faceNormal)
		{
			return dot(direction, faceNormal) < 0.0 ? reflect(direction, faceNormal) : direction;
		}

		// A small hash based generator. Path tracing needs a different stream per
		// pixel, per frame and per bounce, and wants it cheap rather than good:
		// the error a weak generator leaves shows up as correlation between
		// neighbouring pixels, which the accumulation then averages away.
		uint pcgHash(uint v)
		{
			uint state = v * 747796405u + 2891336453u;
			uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
			return (word >> 22u) ^ word;
		}

		uint rngState;
		float randomFloat()
		{
			rngState = pcgHash(rngState);
			return float(rngState) * (1.0 / 4294967296.0);
		}

		// Build an orthonormal basis around a normal without a branch, so a
		// cosine weighted direction can be rotated into world space.
		void basisFrom(vec3 n, out vec3 t, out vec3 b)
		{
			float s = n.z >= 0.0 ? 1.0 : -1.0;
			float a = -1.0 / (s + n.z);
			float c = n.x * n.y * a;
			t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
			b = vec3(c, s + n.y * n.y * a, -n.y);
		}

		vec3 cosineDirection(vec3 n)
		{
			float r1 = randomFloat();
			float r2 = randomFloat();
			float phi = 6.2831853 * r1;
			float sinTheta = sqrt(r2);
			vec3 t, b;
			basisFrom(n, t, b);
			return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * sqrt(max(0.0, 1.0 - r2)));
		}

		// What a solid surface is made of. Anything without a material shades
		// exactly as every surface did before there were any: matte, its
		// texture's colour and nothing else.
		struct Material
		{
			float roughness;     // 0 a mirror, 1 fully matte
			float metalness;
			float reflectance;   // face on, for the part that is not metal
			bool glossy;         // has a glossy half at all
			bool traced;         // smooth enough for its reflection to be traced
		};

		Material matte()
		{
			Material m;
			m.roughness = 1.0;
			m.metalness = 0.0;
			m.reflectance = 0.04;
			m.glossy = false;
			m.traced = false;
			return m;
		}

		// Only for what is shaded as a solid, lit surface. Glass, decals,
		// mirrors and the sky keep their own rules; a self lit surface has no
		// lighting to reflect, and an environment mapped one is a reflection
		// already.
		Material surfaceMaterial(TriangleAttributes attr)
		{
			Material m = matte();
			int index = int(attr.UV2Tex.z);
			if ((Disable & 128u) != 0u || index < 0 || index >= 1024)
				return m;
			if (attr.UV2Tex.w > 1.5 || attr.Emission.w > 0.5 || attr.Normal.w > 0.5)
				return m;
			vec4 v = materials[index];
			m.roughness = clamp(v.x, 0.02, 1.0);
			m.metalness = clamp(v.y, 0.0, 1.0);
			// The fraction: the whole part is the relief (reliefNormal).
			m.reflectance = fract(max(v.z, 0.0));
			// Past this, a surface's sheen is too broad and too faint to be
			// worth a ray of its own; it is shaded matte, as it always was.
			m.glossy = m.roughness < 0.8 || m.metalness > 0.0;
			// Rougher than this, the reflection of the surroundings is a blur
			// that the zone's ambient stands in for well enough, and tracing
			// it cost a second path for every pixel of wood and stone. Its
			// highlights from the lights are kept.
			m.traced = m.roughness < 0.5 && GlossBounces > 0u;
			return m;
		}

		float luminance(vec3 c)
		{
			return dot(c, vec3(0.2126, 0.7152, 0.0722));
		}

		// The GGX microfacet model, with Smith's height correlated shadowing.
		// alpha is the roughness squared, as the model's own parameter.
		float ggxD(float NoH, float alpha2)
		{
			float d = NoH * NoH * (alpha2 - 1.0) + 1.0;
			return alpha2 / (3.14159265 * d * d);
		}

		float smithLambda(float NoX, float alpha2)
		{
			float c2 = max(NoX * NoX, 1.0e-6);
			return 0.5 * (sqrt(1.0 + alpha2 * max(1.0 - c2, 0.0) / c2) - 1.0);
		}

		vec3 fresnel(vec3 f0, float VoH)
		{
			return f0 + (1.0 - f0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
		}

		// How much of the light arriving from every direction a glossy
		// surface reflects towards the eye: Karis's fit to the integral. It
		// is only the colour the reflection is divided by for the denoiser
		// and multiplied by again afterwards, so the fit's error cancels.
		vec3 reflectedAlbedo(vec3 f0, float roughness, float NoV)
		{
			const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
			const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
			vec4 r = roughness * c0 + c1;
			float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
			vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
			return f0 * ab.x + ab.y;
		}

		// What a material makes of a surface's colour, seen at NoV: its
		// reflectance face on, the colour of its glossy half averaged over
		// every direction light comes from, and what is left for the matte
		// half. A matte material leaves the colour exactly as it was.
		void splitColour(Material m, vec3 albedo, float NoV, out vec3 f0, out vec3 shineAlbedo, out vec3 diffuseColour)
		{
			f0 = mix(vec3(m.reflectance), albedo, m.metalness);
			shineAlbedo = vec3(0.0);
			diffuseColour = albedo;
			if (m.glossy)
			{
				shineAlbedo = reflectedAlbedo(f0, m.roughness, NoV);
				float dielectric = reflectedAlbedo(vec3(m.reflectance), m.roughness, NoV).x;
				diffuseColour = albedo * (1.0 - m.metalness) * (1.0 - dielectric);
			}
		}

		// The glossy reflection of a light from direction L, per unit of the
		// light's brightness at the surface: the BRDF times the cosine, and
		// times pi, since the engine's lights are scaled so that a matte
		// surface reflects its albedo times the light rather than over pi.
		// Point lights make an infinitely sharp highlight on a very smooth
		// surface, so their roughness is held above a floor.
		vec3 glossyLight(Material m, vec3 f0, vec3 N, vec3 V, vec3 L)
		{
			float NoL = dot(N, L);
			float NoV = max(dot(N, V), 1.0e-4);
			if (NoL <= 0.0)
				return vec3(0.0);
			vec3 H = normalize(V + L);
			float alpha = max(m.roughness * m.roughness, 0.02);
			float alpha2 = alpha * alpha;
			float G2 = 1.0 / (1.0 + smithLambda(NoV, alpha2) + smithLambda(NoL, alpha2));
			return fresnel(f0, max(dot(V, H), 0.0)) * (3.14159265 * ggxD(max(dot(N, H), 0.0), alpha2) * G2 / (4.0 * NoV));
		}

		// A direction off a glossy surface drawn from the microfacets the eye
		// can see (Dupuy and Benyoub's spherical cap form of Heitz's method),
		// and the weight the path carries for it: Fresnel times the shadowing
		// the visible normals do not already account for. Returns false when
		// the reflection points into the surface and the path ends.
		bool sampleGlossy(Material m, vec3 f0, vec3 N, vec3 V, out vec3 L, out vec3 weight)
		{
			vec3 t, b;
			basisFrom(N, t, b);
			vec3 v = vec3(dot(V, t), dot(V, b), max(dot(V, N), 1.0e-4));
			float alpha = m.roughness * m.roughness;
			float alpha2 = alpha * alpha;

			vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));
			float phi = 6.2831853 * randomFloat();
			float z = (1.0 - randomFloat()) * (1.0 + vh.z) - vh.z;
			float sinTheta = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
			vec3 nh = vec3(sinTheta * cos(phi), sinTheta * sin(phi), z) + vh;
			vec3 h = normalize(vec3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));

			vec3 l = reflect(-v, h);
			if (l.z <= 0.0)
			{
				L = N;
				weight = vec3(0.0);
				return false;
			}
			float lambdaV = smithLambda(v.z, alpha2);
			float G1 = 1.0 / (1.0 + lambdaV);
			float G2 = 1.0 / (1.0 + lambdaV + smithLambda(l.z, alpha2));
			weight = fresnel(f0, max(dot(v, h), 0.0)) * (G2 / G1);
			L = normalize(t * l.x + b * l.y + N * l.z);
			return true;
		}
	)";

	source += ToneMapGlsl();
	source += R"(
		// Should traversal accept this candidate triangle?
		//
		//   opaque      always.
		//   masked      only where the texture's alpha says the texel is there.
		//               UE1 keys transparency to palette index zero, and drawing
		//               those texels put the key colour across every grate.
		//   translucent always for a view ray, which then adds the surface's
		//               colour and carries on through it; never for a shadow
		//               ray, since glass does not stop light.
		//
		// Confirming rather than tinting during traversal. A ray query reports
		// candidates in whatever order traversal reaches them, including ones
		// beyond what turns out to be the closest opaque hit, so accumulating
		// anything as the ray passes let glass behind a wall colour the pixel in
		// front of it. Confirming shrinks the ray properly and keeps the hits in
		// order.
		bool confirmCandidate(int attributeBase, int primitive, vec2 bary, bool shadowRay, vec3 dir, mat3 toWorld)
		{
			TriangleAttributes attr = tris[attributeBase + primitive];

			// Sprites cast no shadows: the engine draws them flat onto the
			// screen, after the world, and a quad turned to face the camera
			// would throw a shadow that swings as the view turns.
			if (shadowRay && attr.Emission.w > 1.5)
				return false;

			float kind = attr.UV2Tex.w;
			if (kind < 0.5)
				return true;

			int index = int(attr.UV2Tex.z);
			bool textured = index >= 0 && uint(index) < TextureCount;

			vec2 uv = surfaceUV(attr, bary, dir, normalize(toWorld * attr.Normal.xyz));

			// Tested at the top mip level, however far off: each ray finds
			// a hole or a bar exactly, and the rays of a pixel between them
			// cover it by as much as the grille really does. A smaller mip's
			// blurred alpha thickened bars or closed holes.
			if (kind < 1.5)
				return textured ? textureLod(sceneTextures[nonuniformEXT(index)], uv, 0.0).a > 0.5 : true;

			// A mirror is solid; it reflects rather than letting anything past.
			// So is a window onto the sky zone (5) - but not modulated art (4),
			// which the test once let through with the mirrors: every decal
			// then shut out any light reaching the floor under it at a slant,
			// a darker square round every scorch mark and bloodstain.
			if (kind > 2.5 && (kind < 3.5 || kind > 4.5))
				return true;

			// Translucent adds and modulated multiplies; both are handled at the
			// hit and neither stops light. Either can be masked as well, and
			// then its holes are no part of it: a grille that is both showed
			// palette entry zero - magenta - added over whatever lay behind.
			if (shadowRay)
				return false;
			return textured ? textureLod(sceneTextures[nonuniformEXT(index)], uv, 0.0).a > 0.5 : true;
		}

		// Is anything between two points? Terminate on the first hit rather than
		// looking for the closest one: a shadow ray only asks whether, not what.
		// Set when a shadow ray was blocked by something that moved this frame.
		// The pixel's own surface has not changed, so nothing else would tell
		// the accumulation that the light on it has.
		bool shadowedByMover = false;
		// Set when the light sampled is one whose brightness is changing -
		// pulsing, blinking, flickering. The same short history applies.
		bool litByChangingLight = false;
		// With the engine's lighting, the light the ambient gives the point
		// directLight last lit, linear: the ambient is in the same sum as the
		// lights.
		vec3 lightmapAmbient = vec3(0.0);

	)";

	source += R"(
		// What light keeps passing through a surface it is not stopped by:
		// all of it, but for glass. Modulated glass passes what it multiplies
		// the view by, at most all of it. Translucent glass - drawn by adding
		// its colour, so it absorbs nothing the engine shows - passes its
		// texel's hue, as far as the texel is coloured at all: clear and grey
		// glass pass the light as it is, deep red stained glass passes red.
		// Read at the top mip level, so a stained window's pattern lands on
		// the floor, softening with distance as its shadows do. Sprites and
		// what glows - a lamp's light cone, a force field - tint nothing, and
		// neither does a masked texture's hole.
		vec3 glassTransmittance(int attributeBase, int primitive, vec2 bary, vec3 dir, mat3 toWorld)
		{
			if ((Disable & 1048576u) != 0u)
				return vec3(1.0);
			TriangleAttributes attr = tris[attributeBase + primitive];
			float kind = attr.UV2Tex.w;
			bool modulated = kind > 3.5 && kind < 4.5;
			if (!modulated && (kind < 1.5 || kind > 2.5))
				return vec3(1.0);
			if (attr.Emission.w > 0.5)
				return vec3(1.0);
			vec3 colour = attr.Albedo.rgb;
			int index = int(attr.UV2Tex.z);
			if (index >= 0 && uint(index) < TextureCount)
			{
				vec2 uv = surfaceUV(attr, bary, dir, normalize(toWorld * attr.Normal.xyz));
				vec4 texel = textureLod(sceneTextures[nonuniformEXT(index)], uv, 0.0);
				if (texel.a <= 0.5)
					return vec3(1.0);
				colour = pow(max(texel.rgb, vec3(0.0)), vec3(2.2));
			}
			if (modulated)
				return clamp(colour * 4.595, vec3(0.0), vec3(1.0));
			float top = max(colour.r, max(colour.g, colour.b));
			if (top < 0.01)
				return vec3(1.0);
			float saturation = 1.0 - min(colour.r, min(colour.g, colour.b)) / top;
			return mix(vec3(1.0), colour / top, saturation);
		}
	)";

	source += R"(
		// A glow: art drawn by adding an unlit colour - a sprite, or an unlit
		// surface or mesh drawn translucent: smoke, sparks, a lamp's light
		// cone. What it adds depends neither on what lies behind it nor on
		// the order the glows are met in, so rather than stop at each one as
		// at glass, the trace sums every glow before the next thing it does
		// stop at (glowAlong). One by one, a plume of smoke used up the
		// layers a ray may pass and the ray ended in the smoke: the floor
		// behind and the fog in front of it went missing in squares, the
		// edges of whichever puff was one too many. An environment mapped
		// one, rare as it is, is passed as glass is instead: its texel is
		// where the view reflects off its smoothed normal, which that path
		// works out anyway, and glowAlong is cheaper without it.
		bool additiveGlow(int attributeBase, int primitive)
		{
			float kind = tris[attributeBase + primitive].UV2Tex.w;
			return kind > 1.5 && kind < 2.5 && tris[attributeBase + primitive].Emission.w > 0.5 &&
				tris[attributeBase + primitive].Normal.w < 0.5;
		}

		// Every glow along a ray from tMin to tMax, each a texel of its
		// texture as bright as its glow - the same as passing them one by
		// one gave. bounced is a path gathering light rather than the view,
		// which takes a glow as the level's glowing surfaces (GlowScale).
		// Each triangle is offered once, as the geometry holding them is
		// built for (AccelStructure's NO_DUPLICATE_ANY_HIT). changed is set
		// when one of them moved since the last frame.
		vec3 glowAlong(vec3 origin, vec3 direction, float tMin, float tMax, uint cullMask, float coneWidth, float coneSpread, bool bounced, inout bool changed)
		{
			vec3 sum = vec3(0.0);
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel, gl_RayFlagsCullOpaqueEXT, cullMask, origin, tMin, direction, tMax);
			while (rayQueryProceedEXT(rq))
			{
				if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
					continue;
				int attributeBase = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
				int primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
				if (!additiveGlow(attributeBase, primitive))
					continue;
				vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
				mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rq, false);
				if (!confirmCandidate(attributeBase, primitive, bary, false, direction, mat3(objectToWorld)))
					continue;
				TriangleAttributes attr = tris[attributeBase + primitive];
				float t = rayQueryGetIntersectionTEXT(rq, false);
				vec3 faceNormal = normalize(mat3(objectToWorld) * attr.Normal.xyz);
				float side = dot(faceNormal, direction) > 0.0 ? -1.0 : 1.0;
				faceNormal *= side;
				float width = coneWidth + coneSpread * t;
				Footprint footprint;
				footprint.logWidth = footprintOf(width, faceNormal, direction, objectToWorld, attr.Normal.xyz);
				footprint.graded = footprintAxes(attr, width, faceNormal, direction,
					mat3(rayQueryGetIntersectionWorldToObjectEXT(rq, false)), footprint.dx, footprint.dy);
				int instance = rayQueryGetIntersectionInstanceIdEXT(rq, false);
				// A sprite's glow, or an unlit mesh's, is its actor's
				// ScaleGlow, carried by the instance (see the trace's glow).
				float glow = attr.Emission.w > 1.1 ? pow(max(instanceAmbient[instance].x, 0.0), 2.2) : 1.0;
				sum += surfaceAlbedo(attr, bary, direction, faceNormal, footprint) * glow;
				if (instanceAmbient[instance].w < 0.0)
					changed = true;
			}
			if (bounced)
				return (Disable & 512u) != 0u ? vec3(0.0) : sum * GlowScale;
			return sum;
		}
	)";

	source += R"(
		// What the glass a shadow ray passed through let through, when it
		// was not stopped: see occludedBy.
		vec3 shadowTint = vec3(1.0);

		// cullMask is which instances can be in the way: ShadowRays for the
		// level's lights (occluded), FlashlightRays for the flashlight. Glass
		// on the way tints the light (shadowTint), each pane once: the
		// geometry holding anything not opaque is built so that a ray query
		// offers each of its triangles once (AccelStructure's
		// NO_DUPLICATE_ANY_HIT).
		//
		// What a candidate does to the light is confirmCandidate's and
		// glassTransmittance's answers for a shadow ray, worked out here at
		// once from one read of the triangle and at most one texel: asked of
		// the two in turn, inside the traversal, with the triangle's
		// transform fetched for every candidate, they held enough at once
		// to overflow an AMD GPU's registers into memory at every light's
		// shadow ray. The transform is wanted only by environment mapped art.
		bool occludedBy(vec3 origin, vec3 dir, float dist, uint cullMask)
		{
			shadowTint = vec3(1.0);
			if ((Disable & 2u) != 0u)
				return false;
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel,
				gl_RayFlagsTerminateOnFirstHitEXT | ((Disable & 8u) != 0u ? gl_RayFlagsOpaqueEXT : 0u),
				cullMask, origin, RayEpsilon, dir, dist);
			while (rayQueryProceedEXT(rq))
			{
				if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
					continue;
				TriangleAttributes attr = tris[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false) + rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)];
				float kind = attr.UV2Tex.w;
				// Sprites cast no shadows: the engine draws them flat onto
				// the screen, after the world, and a quad turned to face the
				// camera would throw a shadow that swings as the view turns.
				// Nor do they tint the light.
				if (attr.Emission.w > 1.5)
					continue;
				// Solid; and a mirror, which reflects rather than letting
				// anything past, and a window onto the sky zone (5). Not
				// modulated art (4): every decal then shut out any light
				// reaching the floor under it at a slant, a darker square
				// round every scorch mark and bloodstain.
				if (kind < 0.5 || (kind > 2.5 && (kind < 3.5 || kind > 4.5)))
				{
					rayQueryConfirmIntersectionEXT(rq);
					continue;
				}
				// A hole in a grate lets light through, so masked art only
				// stops it where its texel is there. Glass - translucent or
				// modulated - never stops it, but tints it, unless it glows:
				// a lamp's light cone, a force field tint nothing.
				bool masked = kind < 1.5;
				if (!masked && ((Disable & 1048576u) != 0u || attr.Emission.w > 0.5))
					continue;
				int index = int(attr.UV2Tex.z);
				bool textured = index >= 0 && uint(index) < TextureCount;
				// Tested at the top mip level, however far off: each ray finds
				// a hole or a bar exactly, and a stained window's pattern
				// lands on the floor, softening with distance as its shadows
				// do. A smaller mip's blurred alpha thickened bars or closed
				// holes.
				vec4 texel = vec4(0.0, 0.0, 0.0, 1.0);
				if (textured)
				{
					vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
					vec3 normal = attr.Normal.w > 0.5 ? normalize(mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, false)) * attr.Normal.xyz) : vec3(0.0, 0.0, 1.0);
					texel = textureLod(sceneTextures[nonuniformEXT(index)], surfaceUV(attr, bary, dir, normal), 0.0);
				}
				if (masked)
				{
					if (texel.a > 0.5)
						rayQueryConfirmIntersectionEXT(rq);
					continue;
				}
				// What the glass lets through. Modulated glass passes what it
				// multiplies the view by, at most all of it. Translucent glass
				// - drawn by adding its colour, so it absorbs nothing the
				// engine shows - passes its texel's hue, as far as the texel
				// is coloured at all: clear and grey glass pass the light as
				// it is, deep red stained glass passes red. Its holes pass it
				// all.
				if (textured && texel.a <= 0.5)
					continue;
				vec3 colour = textured ? pow(max(texel.rgb, vec3(0.0)), vec3(2.2)) : attr.Albedo.rgb;
				if (kind > 3.5)
				{
					shadowTint *= clamp(colour * 4.595, vec3(0.0), vec3(1.0));
					continue;
				}
				float top = max(colour.r, max(colour.g, colour.b));
				if (top < 0.01)
					continue;
				float saturation = 1.0 - min(colour.r, min(colour.g, colour.b)) / top;
				shadowTint *= mix(vec3(1.0), colour / top, saturation);
			}
			if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
				return false;
			int blocker = rayQueryGetIntersectionInstanceIdEXT(rq, true);
			if (blocker > 0 && instanceAmbient[blocker].w < 0.0)
				shadowedByMover = true;
			return true;
		}

		bool occluded(vec3 origin, vec3 dir, float dist)
		{
			return occludedBy(origin, dir, dist, ShadowRays);
		}

		// A value from 0 to 1 for each 32 unit cell of the level - about a
		// lightmap texel - and each step, blended between neighbouring cells
		// the way a lightmap's texels are filtered.
		float cellHash(ivec3 cell, uint step)
		{
			uint h = pcgHash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u ^ pcgHash(step));
			return float(h) * (1.0 / 4294967296.0);
		}

		float cellNoise(vec3 position, uint step)
		{
			vec3 p = position / 32.0;
			ivec3 c = ivec3(floor(p));
			vec3 f = p - vec3(c);
			float x00 = mix(cellHash(c, step), cellHash(c + ivec3(1, 0, 0), step), f.x);
			float x10 = mix(cellHash(c + ivec3(0, 1, 0), step), cellHash(c + ivec3(1, 1, 0), step), f.x);
			float x01 = mix(cellHash(c + ivec3(0, 0, 1), step), cellHash(c + ivec3(1, 0, 1), step), f.x);
			float x11 = mix(cellHash(c + ivec3(0, 1, 1), step), cellHash(c + ivec3(1, 1, 1), step), f.x);
			return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
		}

		// Torch waver, fire waver and watery shimmer, as Render.dll applies
		// them: after a surface's lightmap is lit, each texel the light reaches
		// is scaled by its own entry of a table of randoms, 0.95 to 1 for a
		// torch and 0.8 to 1 for a fire, the table drawn afresh every frame;
		// watery shimmer scales by 0.6 to 1 from a second table whose entries
		// drift towards new randoms over about a second. So a torch or a fire
		// flickers across the surfaces it lights rather than as a whole, and
		// shimmer ripples slowly. The texel becomes a cell of the level.
		float waver(vec3 position, float pattern, vec3 lightPosition)
		{
			// The slow and fast waves (Render.dll's FUN_10b03de0 and
			// FUN_10b040b0): rings running out from the light at 35 units a
			// second, 64 units apart, or 32 for the fast one - 0.7 + 0.3 sin
			// of the whole units out, less 35 a second, in turns of that.
			if (pattern > 5.5)
			{
				float wavelength = pattern > 6.5 ? 32.0 : 64.0;
				float away = floor(distance(position, lightPosition));
				return 0.7 + 0.3 * sin((away - 35.0 * Time) * (6.2831853 / wavelength));
			}
			if (pattern < 3.5)
				return 0.95 + 0.05 * cellNoise(position, Counts.x);
			if (pattern < 4.5)
				return 0.8 + 0.2 * cellNoise(position, Counts.x);
			float t = Time;
			uint second = uint(floor(t));
			float drift = smoothstep(0.0, 1.0, fract(t));
			float s = mix(cellNoise(position, second + 0x9e3779b9u), cellNoise(position, second + 0x9e3779bau), drift);
			return 0.6 + 0.4 * s;
		}

		// How Render.dll lights a mesh, at its vertices (FLightManager::Light),
		// where a flat surface's lightmap takes plain N.L: by (N.L + 1)^2 - 1.5,
		// which is nothing below an N.L of about 0.22 and 2.5 facing the light,
		// so a lit character comes out brighter and flatter and falls into
		// shadow more sharply. Plus a sheen where the light lies along the
		// surface, away from the viewer - six times the square of how far the
		// light's direction along the surface points down the view - which is
		// the rim a character gets from a light behind them.
		float meshResponse(float c, vec3 dir, vec3 normal, vec3 viewDir)
		{
			float response = max((c + 1.0) * (c + 1.0) - 1.5, 0.0);
			float rim = dot(viewDir, dir - normal * dot(normal, dir));
			if (rim > 0.0)
				response += 6.0 * rim * rim;
			return response;
		}

		// One light at a point: what it gives there before anything gets in
		// its way, and which way and how far it is. False where it gives
		// nothing. meshGlow is 1.4 times the actor's ScaleGlow for a point on
		// a mesh, which the engine scales its mesh lighting by, and negative
		// anywhere else; viewDir is the way the ray that found the point was
		// going.
		// One texel of a lightmap's shadow mask, 0 to 255, a byte each.
		float maskTexel(uint start, int x, int y, int width)
		{
			uint b = start + uint(y * width + x);
			return float((lightmapData[b >> 2u] >> ((b & 3u) * 8u)) & 255u);
		}

		// Where a point lies on its surface's lightmap, worked out for each
		// light that asks (lightAt). present is false with no lightmap, or with the
		// masks left out (PT BAKEDSHADOWS, Disable bit 16384).
		struct LightmapPoint
		{
			bool present;
			uint listStart;
			uint listCount;
			uint start;
			uint texels;
			int width;
			ivec4 corners;   // x0 x1 y0 y1
			vec2 f;
		};

		// surface is the record's number plus one (Emission.z).
		LightmapPoint lightmapPoint(uint surface, vec3 position)
		{
			LightmapPoint p;
			p.present = surface != 0u && (Disable & 16384u) == 0u;
			p.listStart = 0u;
			p.listCount = 0u;
			p.start = 0u;
			p.texels = 0u;
			p.width = 1;
			p.corners = ivec4(0);
			p.f = vec2(0.0);
			if (!p.present)
				return p;
			uint record = 1u + (surface - 1u) * 12u;
			vec4 ua = uintBitsToFloat(uvec4(lightmapData[record], lightmapData[record + 1u], lightmapData[record + 2u], lightmapData[record + 3u]));
			vec4 va = uintBitsToFloat(uvec4(lightmapData[record + 4u], lightmapData[record + 5u], lightmapData[record + 6u], lightmapData[record + 7u]));
			uint size = lightmapData[record + 9u];
			int width = int(size & 65535u), height = int(size >> 16u);
			p.start = lightmapData[record + 8u];
			p.listStart = lightmapData[record + 10u];
			p.listCount = lightmapData[record + 11u];
			p.texels = uint(width * height);
			p.width = width;
			vec2 t = vec2(dot(ua.xyz, position) + ua.w, dot(va.xyz, position) + va.w);
			p.f = fract(t);
			ivec2 t0 = ivec2(floor(t));
			p.corners = ivec4(clamp(t0.x, 0, width - 1), clamp(t0.x + 1, 0, width - 1), clamp(t0.y, 0, height - 1), clamp(t0.y + 1, 0, height - 1));
			return p;
		}

		// A baked light's shadow mask at the point, doubled as the engine
		// counts it: 2 where the light is clear all round, falling to 0 in
		// shadow, as the engine filters its masks before lighting a surface
		// (LevelScene's FilteredMask) and filtered between texels as a
		// device draws a lightmap; 0 where the surface's lightmap does not
		// have the light at all, and 2 on a surface without one. bakedId is
		// the light's number (Flags.y).
		float bakedScale(LightmapPoint p, uint bakedId)
		{
			if (!p.present)
				return 2.0;
			uint k = 0u;
			while (k < p.listCount && lightmapData[p.listStart + k] != bakedId)
				k++;
			if (k == p.listCount)
				return 0.0;
			uint start = p.start + k * p.texels;
			float top = mix(maskTexel(start, p.corners.x, p.corners.z, p.width), maskTexel(start, p.corners.y, p.corners.z, p.width), p.f.x);
			float bottom = mix(maskTexel(start, p.corners.x, p.corners.w, p.width), maskTexel(start, p.corners.y, p.corners.w, p.width), p.f.x);
			return 2.0 * mix(top, bottom, p.f.y) / 255.0;
		}

		bool lightAt(uint i, vec3 position, vec3 normal, bool specialLit, float meshGlow, vec3 viewDir, uint lightmapSurface, vec3 lightmapPosition, bool masked,
			out vec3 value, out vec3 base, out vec3 dir, out float distance, out bool behind)
		{
			value = vec3(0.0);
			base = vec3(0.0);
			dir = vec3(0.0, 0.0, 1.0);
			distance = 0.0;
			behind = false;
			SceneLight light = lights[i];
			// A flickering light on a frame it is dark: still in the list, so
			// the list does not change length as it flickers.
			if (light.ColorBrightness.a <= 0.0)
				return false;

			bool lightSpecial = light.PositionRadius.w < 0.0;
			if (lightSpecial != specialLit)
				return false;

			vec3 toLight = light.PositionRadius.xyz - position;
			distance = length(toLight);
			float radius = abs(light.PositionRadius.w);
			// A cylinder light's reach is measured across the floor, not
			// up and down.
			float reach = mod(light.Flags.y, 2.0) > 0.5 ? length(toLight.xy) : distance;
			if (reach >= radius || distance <= 0.0001)
				return false;

			dir = toLight / distance;
			float cosTheta = dot(normal, dir);
			// A mesh takes light from behind it too: the engine's sheen
			// needs no more than the light lying along the surface.
			bool mesh = meshGlow >= 0.0;
			if (cosTheta <= 0.0 && !mesh)
				return false;
			// Non incidence: as bright on a surface edge on as face on.
			if (light.Flags.x > 0.5)
				cosTheta = 1.0;
			// A mesh as the engine lights one; see meshResponse.
			float response = mesh ? meshResponse(cosTheta, dir, normal, viewDir) * meshGlow : cosTheta;
			if (response <= 0.0)
				return false;

			// A spotlight, bright along its axis and fading to nothing at
			// the cone's edge, with the engine's squared falloff.
			float spot = 1.0;
			if (light.DirectionCone.w >= 0.0)
			{
				float along = dot(-dir, light.DirectionCone.xyz);
				float edge = light.DirectionCone.w;
				if (along <= edge)
					return false;
				float f = (along - edge) / max(1.0 - edge, 0.0001);
				spot = f * f;
			}

			// Patterned lights, each as Render.dll draws it. All three
			// work from the angle around the light and fade their pattern
			// near the light's vertical axis, measured in world units.
			float disco = 1.0;
			if (light.Flags.w > 2.5)
			{
				// The wavers, which Render.dll applies to each lightmap
				// texel a light reaches: see waver().
				disco = waver(position, light.Flags.w, light.PositionRadius.xyz);
			}
			else if (light.Flags.w > -0.5)
			{
				vec3 v = -dir;
				float across2 = (v.x * v.x + v.y * v.y) * distance * distance;
				float yaw = atan(v.x, v.y);
				if (light.Flags.w < 0.5)
				{
					// Disco: eleven bands around and eleven down from the
					// light, both drifting at five radians a second, so
					// the patches travel up and down as well as round.
					float t = Time * 5.0;
					float a = 0.5 + 0.5 * cos(11.0 * yaw + t);
					float b = 0.5 + 0.5 * cos(11.0 * atan(sqrt(v.x * v.x + v.y * v.y), v.z) + t);
					float f = a + b - a * b;
					float nearAxis = across2 * 5.0e-5;
					if (nearAxis < 1.0)
						f *= nearAxis;
					disco = 1.0 - f;
				}
				else if (light.Flags.w < 1.5)
				{
					// Searchlight: one beam a quarter turn wide, sweeping
					// round once every 8 pi of the offset - four times the
					// angle around the light plus the offset, folded by 8 pi
					// (Render.dll's FUN_10b03770 pushes 8 pi, not 2 pi, to
					// its fmod, which a police car's lights coming round
					// four times too fast gave away), lit from pi to 3 pi,
					// brightest in the middle. C's fmod keeps the sign,
					// which is what the engine used.
					float x = 4.0 * yaw + light.DirectionCone.x;
					x = x - 25.132741 * trunc(x / 25.132741);
					if (x < 3.1415927 || x > 9.424778)
						return false;
					disco = 0.5 + 0.5 * cos(x);
					float nearAxis = across2 * 6.0e-5;
					if (nearAxis < 1.0)
						disco *= nearAxis;
				}
				else
				{
					// Rotor: six blades turning at three and a half
					// radians a second - the way DirectionCone.x says -
					// filling in to full brightness near the axis.
					disco = 0.5 + 0.5 * cos(6.0 * yaw + light.DirectionCone.x * Time * 3.5);
					float nearAxis = across2 * 1.0e-4;
					if (nearAxis < 1.0)
						disco = 1.0 - nearAxis + nearAxis * disco;
				}
				if (disco <= 0.0)
					return false;
			}
	)";

	source += R"(
			// Linear to zero at the radius, as the engine lights a mesh and as
			// the linear lighting lights everything; the engine's lighting
			// takes the level's surfaces by its lightmaps' curve below.
			float falloff = 1.0 - reach / radius;
			float shade = light.ColorBrightness.a * spot * disco;

			// A mesh's light is summed as the engine sums it, in the colours
			// as displayed - the engine's lighting arithmetic is all done on
			// them - and made linear once the sum is clamped: see the end.
			if (mesh)
			{
				base = pow(light.ColorBrightness.rgb, vec3(1.0 / 2.2)) * (shade * falloff);
				value = base * response;
			}
			else if (!EngineLighting)
			{
				base = light.ColorBrightness.rgb * (shade * falloff);
				value = base * response;
			}
			else
			{
				// The level's surfaces as Render.dll builds their lightmaps:
				// the light's colour at its brightness, cone and pattern, times
				// 1 - 3x^2 + 2x^3 of the way x out to its radius and the
				// cosine, all as displayed, and no brighter than full - summed
				// as displayed, as the engine sums them, and made linear at the
				// end, where overlapping lights come out brighter together than
				// apart. A light baked into the lightmaps counts twice what a
				// dynamic one does: its shadow mask is filtered out to 255
				// where a light without one is filled at 127.
				//
				// And on a surface with a lightmap, it is held to the mask as
				// the engine has it there as well as to the trace's shadows.
				// The engine's masks are coarse - a texel can be the best part
				// of a metre across - and blurred, and most of a small wall's
				// texels can lie in shadow where the light reaches the whole
				// of it; traced alone, such a room came out several times as
				// bright as the engine draws it. Unless masked, the light is
				// taken as clear all round: see directLight.
				float x = reach / radius;
				float mask = 1.0;
				if (light.Flags.y > 1.5)
					mask = masked ? bakedScale(lightmapPoint(lightmapSurface, lightmapPosition), uint(light.Flags.y * 0.5)) : 2.0;
				float lit = shade * (1.0 - x * x * (3.0 - 2.0 * x)) * mask;
				base = pow(light.ColorBrightness.rgb, vec3(1.0 / 2.2)) * lit;
				value = min(base * response, vec3(1.0));
			}
			behind = cosTheta <= 0.0;
			return luminance(value) > 0.0;
		}

		// Does a light reach the point? Shadowed as by a light the size of a
		// lamp rather than a point: the shadow ray goes to a random point on
		// a disc around the light, facing the surface, so a shadow is sharp
		// where whatever casts it meets the surface and softens as the two
		// part, as a real one does: the penumbra grows with the distance from
		// the caster, and what is averaged over frames or denoised is that
		// penumbra rather than an edge blurred evenly. Brightness and falloff
		// stay the engine's, from the light's centre.
		//
		// A light behind a mesh reaches it only as the engine's sheen, which a
		// shadow ray could not show: it would start into the mesh itself. The
		// engine never shadows a mesh at all, so that part is left unshadowed.
		//
		// What reaches, of each colour: nothing when something is in the way,
		// and what the glass on the way lets through (glassTransmittance).
		vec3 lightReaches(vec3 position, vec3 dir, float distance, bool behind)
		{
			vec3 shadowDir = dir;
			float shadowDistance = distance;
			if (LightRadius > 0.0)
			{
				vec3 across = normalize(cross(dir, abs(dir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
				vec3 along = cross(dir, across);
				// Kept well in front of a surface the light is close to.
				float r = min(LightRadius, distance * 0.5) * sqrt(randomFloat());
				float a = 6.2831853 * randomFloat();
				vec3 toTarget = dir * distance + (across * cos(a) + along * sin(a)) * r;
				shadowDistance = length(toTarget);
				shadowDir = toTarget / shadowDistance;
			}
			if (behind)
				return vec3(1.0);
			return occluded(position, shadowDir, shadowDistance - RayEpsilon * 2.0) ? vec3(0.0) : shadowTint;
		}
	)";

	source += R"(
		// The light a point takes from the lights.
		//
		// Pick one light in proportion to what it would contribute if nothing
		// were in the way, then trace a single shadow ray at it. Choosing
		// uniformly instead is what made this scene look black: a Deus Ex
		// level holds hundreds of lights and a given surface is in range of
		// perhaps two, so a uniform pick misses almost every time. The
		// weighing is of the lights in the point's cell - no rays - and in a
		// crowded one of the heaviest few and a few drawn from the rest, the
		// draws counted for the lights they stand for (see the grid's
		// ranking, AccelStructure::WriteLightGrid).
		//
		// With the engine's lighting the strongest few are each traced
		// instead, and one pick stands for the rest: the engine's curve is
		// taken of what actually reaches the point, which one ray at a random
		// light cannot say. A point where most of the lights in reach are
		// behind walls otherwise takes the curve of all of them, far too
		// bright. strongest is how many.
		//
		// specialLit is the surface's PF_SpecialLit: such a surface is lit
		// only by lights marked bSpecialLit, and every other surface only by
		// the rest. A special light carries its radius negated. With every
		// light rather than one chosen at random, and no shadow ray, when
		// "everyLight" is set: the same answer each frame, for glass and
		// water, whose own lighting is added over what lies behind them and
		// never passes through the denoiser.
		//
		// Returns the light a matte surface would take, to be multiplied by
		// its colour. For a glossy surface's highlight it also says which
		// light it came from - its direction, and its brightness here before
		// the angle to the surface - or zero when it was blocked or there was
		// none. The light is chosen by what it adds to the matte part, and
		// the highlight is worked out by the caller for that light alone:
		// weighing every light by its highlight cost more than the highlight
		// was worth. shownAmbient is the zone's ambient on a level surface as
		// displayed, which with the engine's lighting goes into the sum with
		// the lights, and what it then gives is left in lightmapAmbient.
		// lightmapPosition is the point where its surface's lightmap is laid
		// out: the world for the level, and a mover's own brush for one of
		// its faces, whose lightmap moves with it.
		vec3 directLight(vec3 position, vec3 normal, bool specialLit, bool everyLight, float meshGlow, vec3 viewDir, vec3 shownAmbient, uint strongest, uint surface,
			vec3 lightmapPosition, out vec3 lightDirection, out vec3 lightBase)
		{
			lightDirection = normal;
			lightBase = vec3(0.0);
			bool engineSum = EngineLighting && meshGlow < 0.0;
			// The engine's shadow masks, looked up only for the lights that
			// are traced: the strongest are chosen by what they would give
			// clear all round, and each pick weighed by that, which is what
			// makes the estimate come out right whatever its mask turns out
			// to be. Looking up every light in reach cost more than the rest
			// of the lighting together. Where the point lies on its
			// lightmap is worked out for each light that wants it rather
			// than once here and carried through the loop: a dozen values
			// held across every light's shadow ray cost an AMD GPU more
			// than the dozen reads.
			uint lightmapSurface = engineSum ? surface : 0u;
			// A lightmap's full is drawn at twice the texture's brightness.
			lightmapAmbient = EngineLighting ? pow(2.0 * min(shownAmbient, vec3(1.0)), vec3(2.2)) : vec3(0.0);
			if ((Disable & 1u) != 0u)
				return vec3(0.0);
			uint count = Counts.y;
			if (count == 0u)
				return vec3(0.0);
			// How many lights at the head of the cell's list - the heaviest,
			// by the grid's ranking - are weighed exactly, and how many are
			// drawn from the rest to stand for them: more where the view
			// meets the world than at a bounce. Every one, as it has to be
			// for everyLight, where the answer must not change from frame to
			// frame; and every one under PT ALLLIGHTS, for comparison.
			uint exactCount = strongest >= 4u ? 8u : 4u;
			uint drawCount = strongest >= 4u ? 4u : 2u;
			if (!engineSum || everyLight)
				strongest = 0u;
			strongest = min(strongest, 4u);

			vec3 total = vec3(0.0);
			bool anyChanging = false;
			// The strongest lights, heaviest first, and one pick from the rest
			// in proportion to its weight. restWeight is what the pick stands
			// for, chosenTarget what the light picked gives unshadowed.
			uint strongIndex[4];
			float strongWeight[4];
			uint strongCount = 0u;
			float restWeight = 0.0;
			int chosen = -1;
			float chosenTarget = 0.0;

			// Only the lights listed for the cell this point is in. A point
			// outside the grid is beyond every light's reach.
			vec3 gridOrigin = uintBitsToFloat(uvec3(lightGrid[0], lightGrid[1], lightGrid[2]));
			float cellSize = uintBitsToFloat(lightGrid[3]);
			ivec3 dims = ivec3(lightGrid[4], lightGrid[5], lightGrid[6]);
			ivec3 cell = ivec3(floor((position - gridOrigin) / cellSize));
			if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, dims)))
				return vec3(0.0);
			uint cellIndex = uint((cell.z * dims.y + cell.y) * dims.x + cell.x);
			uint listStart = lightGrid[8u + cellIndex * 2u];
			uint listCount = lightGrid[9u + cellIndex * 2u];
			uint weighed = (everyLight || (Disable & 2097152u) != 0u) ? listCount : min(listCount, exactCount);

			// One loop through four phases, so that lightAt and the shadow ray
			// are each written once. Every call is inlined, and written once
			// per phase this function came to ten times the code it needs:
			// on AMD's GPUs, the registers it held at once overflowed into
			// memory, and that cost more than the tracing. In order:
			//   0  the lights at the head of the cell's list, each weighed
			//   1  the rest of a crowded cell, stood for by a few drawn from it
			//   2  the strongest, each traced (not for everyLight)
			//   3  the pick standing for the rest, traced
			//
			// Phase 1: the rest of a crowded cell, stood for by a few drawn
			// from it in proportion to their rank, each weighed exactly and
			// offered to the same pick. A draw goes in weighed by what it gives
			// over how likely it was to be drawn, averaged over the draws:
			// resampled importance sampling. The pick then stands for the
			// lights weighed and the rest together, on average exactly as they
			// add up, for the cost of a few however many the cell holds. Each
			// entry's second word is the rank of it and every one after it
			// summed, so a draw is a binary search down the tail.
			//
			// Phases 2 and 3: what reaches the point - the strongest lights
			// each traced, and the pick standing for the rest, divided by the
			// chance it was picked with. The highlight follows the strongest
			// light that got through, or else the pick.
			float tail = weighed < listCount ? uintBitsToFloat(lightGrid[listStart + 2u * weighed + 1u]) : 0.0;
			vec3 reaches = vec3(0.0);
			float highlightWeight = 0.0;
			uint phase = 0u;
			uint step = 0u;
			while (true)
			{
				// The next light, or on to the next phase.
				uint i;
				bool masked = true;
				float drawChance = 0.0;
				if (phase == 0u)
				{
					if (step >= weighed)
					{
						phase = 1u;
						step = 0u;
						continue;
					}
					i = lightGrid[listStart + 2u * step];
					masked = everyLight;
				}
				else if (phase == 1u)
				{
					if (weighed >= listCount || step >= drawCount || tail <= 0.0)
					{
						if (everyLight)
							break;
						phase = 2u;
						step = 0u;
						continue;
					}
					float u = randomFloat() * tail;
					uint lo = weighed, hi = listCount - 1u;
					while (lo < hi)
					{
						uint mid = (lo + hi + 1u) >> 1u;
						if (uintBitsToFloat(lightGrid[listStart + 2u * mid + 1u]) > u)
							lo = mid;
						else
							hi = mid - 1u;
					}
					float here = uintBitsToFloat(lightGrid[listStart + 2u * lo + 1u]);
					float after = lo + 1u < listCount ? uintBitsToFloat(lightGrid[listStart + 2u * lo + 3u]) : 0.0;
					drawChance = (here - after) / tail;
					if (drawChance <= 0.0)
					{
						step++;
						continue;
					}
					i = lightGrid[listStart + 2u * lo];
					masked = everyLight;
				}
				else if (phase == 2u)
				{
					if (step >= strongCount)
					{
						phase = 3u;
						step = 0u;
						continue;
					}
					i = strongIndex[step];
				}
				else
				{
					if (step > 0u || chosen < 0 || chosenTarget <= 0.0)
						break;
					i = uint(chosen);
				}
				step++;

				vec3 value, base, dir;
				float distance;
				bool behind;
				bool lit = lightAt(i, position, normal, specialLit, meshGlow, viewDir, lightmapSurface, lightmapPosition, masked, value, base, dir, distance, behind);

				if (phase == 0u)
				{
					if (!lit)
						continue;
					float weight = luminance(value);
					total += value;
					anyChanging = anyChanging || lights[i].Flags.z > 0.5;

					// Kept among the strongest if it is one, pushing out the
					// weakest of them into the rest.
					uint candidate = i;
					float candidateWeight = weight;
					for (uint t = 0u; t < strongest; t++)
					{
						if (t == strongCount)
						{
							strongIndex[t] = candidate;
							strongWeight[t] = candidateWeight;
							strongCount++;
							candidateWeight = 0.0;
							break;
						}
						if (candidateWeight > strongWeight[t])
						{
							uint heldIndex = strongIndex[t];
							float held = strongWeight[t];
							strongIndex[t] = candidate;
							strongWeight[t] = candidateWeight;
							candidate = heldIndex;
							candidateWeight = held;
						}
					}
					// Reservoir sampling: each candidate replaces the held one with
					// probability equal to its share of the weight seen so far, so
					// one pass leaves a sample drawn in proportion to weight.
					if (candidateWeight > 0.0)
					{
						restWeight += candidateWeight;
						if (randomFloat() < candidateWeight / restWeight)
						{
							chosen = int(candidate);
							chosenTarget = candidateWeight;
						}
					}
				}
				else if (phase == 1u)
				{
					if (!lit)
						continue;
					float target = luminance(value);
					float candidateWeight = target / (float(drawCount) * drawChance);
					if (candidateWeight > 0.0)
					{
						restWeight += candidateWeight;
						if (randomFloat() < candidateWeight / restWeight)
						{
							chosen = int(i);
							chosenTarget = target;
						}
					}
				}
				else
				{
					if (lights[i].Flags.z > 0.5)
						litByChangingLight = true;
					float weight = luminance(value);
					if (weight <= 0.0)
						continue;
					// Glass tints light as it is, linear; the engine's sum is of
					// displayed values.
					vec3 through = lightReaches(position, dir, distance, behind);
					if (luminance(through) <= 0.0)
						continue;
					if (engineSum)
						through = pow(through, vec3(1.0 / 2.2));
					float scale = phase == 3u ? restWeight / chosenTarget : 1.0;
					reaches += value * through * scale;
					if (phase == 2u)
					{
						if (weight > highlightWeight)
						{
							highlightWeight = weight;
							lightDirection = dir;
							lightBase = base * through;
						}
					}
					else if (highlightWeight <= 0.0)
					{
						lightDirection = dir;
						lightBase = base * through * scale;
					}
				}
			}

			if (everyLight)
			{
				if (anyChanging)
					litByChangingLight = true;
				if (engineSum)
				{
					vec3 shown = shownAmbient + total;
					vec3 made = pow(2.0 * min(shown, vec3(1.0)), vec3(2.2)) / max(shown, vec3(1.0e-6));
					lightmapAmbient = made * shownAmbient;
					return made * total;
				}
				// A mesh's is returned as the engine has it, in displayed terms
				// and unclamped: its ambient goes on before the clamp
				// (meshLight).
				return total;
			}

			// Added up as the engine does it: the ambient and the lights as
			// displayed, clamped at full, drawn at twice the texture's
			// brightness as every device draws a lightmap, and made linear.
			if (engineSum)
			{
				vec3 shown = shownAmbient + reaches;
				vec3 made = pow(2.0 * min(shown, vec3(1.0)), vec3(2.2)) / max(shown, vec3(1.0e-6));
				lightmapAmbient = made * shownAmbient;
				lightBase *= made;
				return made * reaches;
			}
			// The estimate is the total in proportion, sampled by its weight,
			// so for a mesh, clamping it clamps the total near enough.
			return reaches;
		}

		// A lit mesh's light as the engine finishes it: its ambient added to
		// the lights' sum, in displayed terms, clamped at one - a lit mesh is
		// never drawn brighter than its texture - then made linear, like every
		// other colour the engine gives.
		vec3 meshLight(vec3 lights, vec3 ambient)
		{
			return pow(min(lights + ambient, vec3(1.0)), vec3(2.2));
		}

		// The ambient on a hit, linear. A lit mesh's instance carries its
		// ambient in displayed terms, for meshLight; the level's surfaces and
		// the movers carry their lightmaps' ambient, linear already.
		vec3 linearAmbient(TriangleAttributes attr, int instanceId)
		{
			vec3 a = instanceAmbient[instanceId].rgb;
			return attr.Ambient.rgb + (attr.CornerNormals.w != 0u ? pow(max(a, vec3(0.0)), vec3(2.2)) : a);
		}
	)";

	source += R"(
		// The light augmentation as a torch rather than the two lights the
		// game gives it - one where the view meets a wall, one at the head -
		// which lit a round patch with shadows falling away from it rather
		// than from the player (LevelScene's flashlight). flashlight[0] xyz
		// is where it shines from, w 1 while it is on; [1] xyz which way, w
		// its intensity; [2] rgb its colour, linear, w how much of its light
		// the air scatters back, per unit of distance.
		//
		// A bright hotspot about 12 degrees across in a dimmer spill out to
		// about 32, falling off with the square of the distance, softened
		// within 128 units where a square law blinds, and shadowed from a
		// lamp two units across. Linear light, added to what the level's
		// lights give as the engine sums them: it is not one of the engine's.
		const float FlashlightRange = 4096.0;
		const float FlashlightNear2 = 128.0 * 128.0;
		const float FlashlightSize = 2.0;

		// How bright the beam is at an angle off its axis, by the cosine.
		float flashlightCone(float c)
		{
			float spill = smoothstep(0.8480, 0.8829, c);   // 32 to 28 degrees
			float hot = smoothstep(0.9613, 0.9945, c);     // 16 to 6 degrees
			return spill * (0.15 + 0.85 * hot);
		}

		// The flashlight's light at a point on a surface facing normal,
		// before anything gets in its way: per unit of the surface's colour,
		// before the angle to the surface, and dir the way to the lamp.
		// False where it gives nothing.
		bool flashlightAt(vec3 position, vec3 normal, out vec3 base, out vec3 dir, out float dist)
		{
			base = vec3(0.0);
			dir = normal;
			dist = 0.0;
			if (flashlight[0].w < 0.5)
				return false;
			vec3 toLight = flashlight[0].xyz - position;
			dist = length(toLight);
			if (dist < 0.5 || dist >= FlashlightRange)
				return false;
			dir = toLight / dist;
			if (dot(normal, dir) <= 0.0)
				return false;
			float cone = flashlightCone(-dot(dir, flashlight[1].xyz));
			if (cone <= 0.0)
				return false;
			float fade = 1.0 - smoothstep(0.75 * FlashlightRange, FlashlightRange, dist);
			base = flashlight[2].rgb * (flashlight[1].w * cone * fade / (dist * dist + FlashlightNear2));
			return true;
		}

		// Does the flashlight reach the point? One shadow ray to a random
		// point on the lamp, as lightReaches does for the level's lights,
		// and what of it glass on the way lets through.
		vec3 flashlightReaches(vec3 position, vec3 dir, float dist)
		{
			vec3 across = normalize(cross(dir, abs(dir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
			vec3 along = cross(dir, across);
			float r = FlashlightSize * sqrt(randomFloat());
			float a = 6.2831853 * randomFloat();
			vec3 toTarget = dir * dist + (across * cos(a) + along * sin(a)) * r;
			float d = length(toTarget);
			return occludedBy(position, toTarget / d, d - RayEpsilon * 2.0, FlashlightRays) ? vec3(0.0) : shadowTint;
		}

		// The beam in the air between the eye and what it meets: the light
		// the air scatters back along the way, the same in every direction,
		// and unshadowed - from beside the lamp, whatever is in the beam's
		// way hides its own shadow. Linear, added to the picture.
		//
		// Along the view, the square law is 1 / ((t - t0)^2 + a^2), where t0
		// is how far along the view passes nearest the lamp and a is how
		// near, softened as the light is: which integrates to an angle,
		// atan((t - t0) / a) / a. So the steps are taken evenly in that angle
		// rather than in distance (Kulla and Fajardo's equiangular sampling),
		// which packs them in where the light is densest, and all that is
		// left to average is the cone. It fades in from 16 to 48 units out
		// from the lamp: every view passes within a few units of a lamp
		// beside the eye, and the beam's first inches, crossing in front of
		// the face, put a faint veil over everything. jitter is the pixel's
		// offset into the steps, the same every frame, so the beam never
		// shimmers.
		vec3 flashlightBeam(vec3 origin, vec3 dir, float len, float jitter)
		{
			if (flashlight[0].w < 0.5 || flashlight[2].w <= 0.0)
				return vec3(0.0);
			float reach = min(len, FlashlightRange);
			vec3 toLamp = flashlight[0].xyz - origin;
			float t0 = dot(toLamp, dir);
			float a = sqrt(max(dot(toLamp, toLamp) - t0 * t0, 0.0) + FlashlightNear2);
			float first = atan(-t0 / a), last = atan((reach - t0) / a);
			const int steps = 12;
			float sum = 0.0;
			for (int s = 0; s < steps; s++)
			{
				float t = t0 + a * tan(mix(first, last, (float(s) + jitter) / float(steps)));
				vec3 fromLight = origin + dir * t - flashlight[0].xyz;
				float ahead = dot(fromLight, flashlight[1].xyz);
				sum += smoothstep(16.0, 48.0, ahead) * flashlightCone(ahead * inversesqrt(max(dot(fromLight, fromLight), 1.0e-4)));
			}
			return flashlight[2].rgb * (flashlight[1].w * flashlight[2].w * (last - first) / a * sum / float(steps));
		}

		// Fog lights' shadows. The engine's volumetric lighting integrates
		// each fog light's sphere along the view whatever is in the way, so a
		// lamp's halo glows on both sides of a pillar and through a wall. Here
		// the air a fog light cannot see is not lit by it: shafts of its glow
		// through a grate, the pillar's shadow in its halo.
		//
		// Each has a cube around it, FogShadowSize texels a face, of how far
		// a ray from the light goes in each texel's direction before
		// something stops it, traced afresh every frame by the pass before
		// the trace (Shaders::FogShadows). The first MaxFogShadows fog lights
		// have one; any more glow as the engine has them.
		const uint FogShadowSize = 64u;
		const uint MaxFogShadows = 128u;
		const int FogSteps = 16;

		// The face a direction from the light falls on, and where on it, -1
		// to 1 each way; and the direction through a point on a face.
		vec2 cubeCoords(vec3 d, out uint face)
		{
			vec3 a = abs(d);
			if (a.x >= a.y && a.x >= a.z)
			{
				face = d.x > 0.0 ? 0u : 1u;
				return d.yz / a.x;
			}
			if (a.y >= a.z)
			{
				face = d.y > 0.0 ? 2u : 3u;
				return d.xz / a.y;
			}
			face = d.z > 0.0 ? 4u : 5u;
			return d.xy / a.z;
		}

		vec3 cubeDirection(uint face, vec2 st)
		{
			float side = (face & 1u) == 0u ? 1.0 : -1.0;
			if (face < 2u)
				return vec3(side, st.x, st.y);
			if (face < 4u)
				return vec3(st.x, side, st.y);
			return vec3(st.x, st.y, side);
		}

		// Whether a fog light sees a point in the air fromLight away from it:
		// 1 in the clear, 0 in shadow. The four texels around the direction
		// are each compared and the answers blended, and each comparison
		// ramps over a few texels' width at that distance, so the edge of a
		// shaft is soft rather than stepped.
		float fogLightSees(uint fogIndex, vec3 fromLight)
		{
			float dist = length(fromLight);
			uint face;
			vec2 texel = (cubeCoords(fromLight, face) * 0.5 + 0.5) * float(FogShadowSize) - 0.5;
			ivec2 t0 = ivec2(floor(texel));
			vec2 f = texel - vec2(t0);
			ivec2 t1 = clamp(t0 + 1, ivec2(0), ivec2(int(FogShadowSize) - 1));
			t0 = clamp(t0, ivec2(0), ivec2(int(FogShadowSize) - 1));
			uint base = (fogIndex * 6u + face) * FogShadowSize * FogShadowSize;
			vec4 reach = vec4(
				fogShadow[base + uint(t0.y) * FogShadowSize + uint(t0.x)],
				fogShadow[base + uint(t0.y) * FogShadowSize + uint(t1.x)],
				fogShadow[base + uint(t1.y) * FogShadowSize + uint(t0.x)],
				fogShadow[base + uint(t1.y) * FogShadowSize + uint(t1.x)]);
			float soft = dist * (3.0 / float(FogShadowSize)) + 2.0;
			vec4 seen = clamp((reach - dist) / soft + 1.0, 0.0, 1.0);
			return mix(mix(seen.x, seen.y, f.x), mix(seen.z, seen.w, f.x), f.y);
		}
	)";

	source += R"(
		// The level's glowing surfaces - signs, light panels, screens - as
		// lights. A glowing surface has always lit what is around it, but
		// only through the bounces that happened to reach it, so a small one
		// lit a wall in sparse speckles that the denoiser smeared, and its
		// colour came and went. Now every surface shaded also picks one of
		// the glowing triangles near it and traces a shadow ray to a point on
		// it, as it does for a light.
		//
		// Both ways count, each weighed by how likely it was to find the
		// light that way against the other (the balance heuristic): a big
		// panel close by is found well by a bounce, a small sign far off by
		// sampling it. So nothing is counted twice, and what a glowing
		// surface gives is what it gave before, with less noise. What it
		// gives is scaled by GlowLighting either way.
		struct Emitter
		{
			vec3 v0, e1, e2, normal;
			float area;
			uint triangle;
			bool twoSided;
		};

		Emitter emitterAt(uint i)
		{
			uint b = emitterData[1] + i * 20u;
			Emitter e;
			e.twoSided = uintBitsToFloat(emitterData[b + 3u]) < 0.0;
			e.normal = uintBitsToFloat(uvec3(emitterData[b + 4u], emitterData[b + 5u], emitterData[b + 6u]));
			e.area = uintBitsToFloat(emitterData[b + 7u]);
			e.v0 = uintBitsToFloat(uvec3(emitterData[b + 8u], emitterData[b + 9u], emitterData[b + 10u]));
			e.e1 = uintBitsToFloat(uvec3(emitterData[b + 12u], emitterData[b + 13u], emitterData[b + 14u]));
			e.triangle = emitterData[b + 15u];
			e.e2 = uintBitsToFloat(uvec3(emitterData[b + 16u], emitterData[b + 17u], emitterData[b + 18u]));
			return e;
		}

		// The list for a point's cell of the grid: where it starts, how long.
		uvec2 emitterList(vec3 p)
		{
			if (emitterData[0] == 0u)
				return uvec2(0u);
			vec3 origin = uintBitsToFloat(uvec3(emitterData[2], emitterData[3], emitterData[4]));
			float cellSize = uintBitsToFloat(emitterData[5]);
			ivec3 dims = ivec3(emitterData[6], emitterData[7], emitterData[8]);
			ivec3 c = ivec3(floor((p - origin) / cellSize));
			if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, dims)))
				return uvec2(0u);
			uint cell = uint((c.z * dims.y + c.y) * dims.x + c.x);
			return uvec2(emitterData[12u + cell * 2u], emitterData[13u + cell * 2u]);
		}

		// How much emitter i is likely to give a point facing n, to choose
		// between them by: its power, the angles at both ends and the square
		// law, all from its middle, the square law held back within its own
		// size and neither angle let fall to nothing, since a big panel close
		// by can light a point its middle hardly faces. None from behind a
		// face that glows one way, nor to a point it lies wholly behind.
		// Reads only the record's first eight words and one more.
		float emitterWeight(uint i, vec3 p, vec3 n)
		{
			uint b = emitterData[1] + i * 20u;
			vec4 middle = uintBitsToFloat(uvec4(emitterData[b], emitterData[b + 1u], emitterData[b + 2u], emitterData[b + 3u]));
			vec4 face = uintBitsToFloat(uvec4(emitterData[b + 4u], emitterData[b + 5u], emitterData[b + 6u], emitterData[b + 7u]));
			vec3 d = middle.xyz - p;
			float side = -dot(face.xyz, d);
			if (middle.w > 0.0 && side <= 0.0)
				return 0.0;
			float along = dot(n, d);
			if (along <= -uintBitsToFloat(emitterData[b + 11u]))
				return 0.0;
			float d2 = dot(d, d);
			float inv = inversesqrt(max(d2, 1.0e-6));
			return abs(middle.w) * max(along * inv, 0.1) * max(abs(side) * inv, 0.1) / max(d2, face.w);
		}

		// What an emitter glows with at a point on it, linear: its texture
		// read coarsely - where the triangle is four texels or so across - as
		// the point stands for the light from the part of it around it. A
		// masked texture's holes glow with nothing.
		vec3 emitterRadiance(Emitter e, vec2 bary)
		{
			TriangleAttributes attr = tris[e.triangle];
			int index = int(attr.UV2Tex.z);
			if (index < 0 || uint(index) >= TextureCount)
				return attr.Albedo.rgb;
			vec2 uv = surfaceUV(attr, bary, vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0));
			ivec2 size = textureSize(sceneTextures[nonuniformEXT(index)], 0);
			vec2 a = attr.UV01.zw - attr.UV01.xy, b = attr.UV2Tex.xy - attr.UV01.xy;
			float texels = abs(a.x * b.y - a.y * b.x) * 0.5 * float(size.x) * float(size.y);
			vec4 t = textureLod(sceneTextures[nonuniformEXT(index)], uv, max(0.5 * log2(max(texels, 1.0)) - 2.0, 0.0));
			return pow(max(t.rgb, vec3(0.0)), vec3(2.2)) * (attr.UV2Tex.w > 0.5 ? t.a : 1.0);
		}

		// What the last surface the path shaded sampled, for weighing a
		// glowing surface the bounce from it then finds: the sum of the
		// weights it chose from (none when it sampled nothing, or the path
		// went on by a mirror or a glossy reflection), where it was, which
		// way it faced, and the share of its bounces that go the matte way.
		float glowWeights = 0.0;
		vec3 glowFrom = vec3(0.0);
		vec3 glowNormal = vec3(0.0, 0.0, 1.0);
		float glowPdfScale = 1.0;

		// The light a point facing n takes from one glowing triangle near it,
		// chosen by weight and sampled at a point uniformly over its area,
		// per unit of the point's colour, linear. pdfScale is the share of
		// the point's bounces that go the matte way. Not sampled where what
		// the glow would give, by the weights, is under a fiftieth of what
		// the lights already give (lit, linear) or too faint to see: the
		// bounces find it there as they always did, and the shadow ray is
		// saved.
		vec3 glowLight(vec3 p, vec3 n, float pdfScale, float lit)
		{
			glowWeights = 0.0;
			if ((Disable & (512u | 262144u)) != 0u)
				return vec3(0.0);
			uvec2 list = emitterList(p);
			float total = 0.0;
			int chosen = -1;
			float chosenWeight = 0.0;
			for (uint k = 0u; k < list.y; k++)
			{
				uint i = emitterData[list.x + k];
				float w = emitterWeight(i, p, n);
				if (w <= 0.0)
					continue;
				total += w;
				if (randomFloat() < w / total)
				{
					chosen = int(i);
					chosenWeight = w;
				}
			}
			if (total * GlowScale / 3.14159265 < max(0.002, 0.02 * lit))
				return vec3(0.0);
			glowWeights = total;
			glowFrom = p;
			glowNormal = n;
			glowPdfScale = pdfScale;
			if (chosen < 0)
				return vec3(0.0);

			Emitter e = emitterAt(uint(chosen));
			float su = sqrt(randomFloat());
			float r = randomFloat();
			vec2 bary = vec2(su * (1.0 - r), su * r);
			vec3 toPoint = e.v0 + e.e1 * bary.x + e.e2 * bary.y - p;
			float dist2 = dot(toPoint, toPoint);
			float dist = sqrt(dist2);
			vec3 dir = toPoint / max(dist, 1.0e-4);
			float cosX = dot(n, dir);
			float area = e.area;
			float cosE = -dot(e.normal, dir);
			if (e.twoSided)
				cosE = abs(cosE);
			if (cosX <= 0.0 || cosE <= 1.0e-4 || area <= 0.0 || dist < 1.0)
				return vec3(0.0);
			vec3 glow = emitterRadiance(e, bary);
			if (luminance(glow) <= 0.0)
				return vec3(0.0);
			if (occludedBy(p, dir, dist - 1.0, ShadowRays))
				return vec3(0.0);
			glow *= shadowTint;
			float pdfLight = (chosenWeight / total) * dist2 / (area * cosE);
			float pdfBounce = pdfScale * cosX / 3.14159265;
			return glow * (GlowScale * cosX / 3.14159265 / (pdfLight + pdfBounce));
		}

		// What a glowing surface a bounce has found gives, as a share of its
		// glow: GlowLighting's, weighed against the chance the surface the
		// bounce left would have sampled it (glowLight). Nothing from behind
		// a surface that glows from one face only, as sampling it never
		// takes. emitterNumber is Emission.z: its number plus one, or 0 for
		// a glowing surface that is not sampled - a mesh's, glass - which
		// the bounces alone find.
		float glowFound(float emitterNumber, vec3 position, vec3 direction)
		{
			if (emitterNumber < 0.5)
				return GlowScale;
			uint index = uint(emitterNumber - 0.5);
			Emitter e = emitterAt(index);
			float facing = -dot(e.normal, direction);
			if (!e.twoSided && facing <= 0.0)
				return 0.0;
			if (glowWeights <= 0.0)
				return GlowScale;
			uvec2 list = emitterList(glowFrom);
			float w = 0.0;
			for (uint k = 0u; k < list.y; k++)
				if (emitterData[list.x + k] == index)
				{
					w = emitterWeight(index, glowFrom, glowNormal);
					break;
				}
			if (w <= 0.0 || abs(facing) <= 1.0e-4)
				return GlowScale;
			vec3 toPoint = position - glowFrom;
			float pdfLight = (w / glowWeights) * dot(toPoint, toPoint) / (e.area * abs(facing));
			float pdfBounce = glowPdfScale * max(dot(glowNormal, direction), 0.0) / 3.14159265;
			return GlowScale * pdfBounce / (pdfBounce + pdfLight);
		}
	)";

	source += R"(
		vec3 skyLight(vec3 dir)
		{
			// Standing in for the level's own sky, which is drawn through a
			// portal this scene does not contain. A weak gradient keeps unlit
			// corners from being pure black, which reads as a hole rather than
			// as shadow.
			float t = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
			return mix(vec3(0.02, 0.02, 0.03), vec3(0.10, 0.12, 0.16), t) * Params.y;
		}

		// Where a point that was at "then" last frame and is at "now" this frame
		// moved on screen, from here to there in screen widths and heights.
		vec2 screenMotion(vec3 nowPosition, vec3 thenPosition)
		{
			vec3 now = nowPosition - CameraOrigin.xyz;
			vec3 then = thenPosition - previousCamera[0].xyz;
			float nowZ = dot(now, CameraForward.xyz);
			float thenZ = dot(then, previousCamera[3].xyz);
			if (nowZ <= 0.0 || thenZ <= 0.0)
				return vec2(0.0);
			vec2 nowUv = vec2(dot(now, CameraRight.xyz) / dot(CameraRight.xyz, CameraRight.xyz),
				dot(now, CameraUp.xyz) / dot(CameraUp.xyz, CameraUp.xyz)) / nowZ - viewShift.xy;
			vec2 thenUv = vec2(dot(then, previousCamera[1].xyz) / dot(previousCamera[1].xyz, previousCamera[1].xyz),
				dot(then, previousCamera[2].xyz) / dot(previousCamera[2].xyz, previousCamera[2].xyz)) / thenZ - viewShift.zw;
			return (thenUv - nowUv) * 0.5;
		}

		// NRD's normal and roughness packing, its R10G10B10A2 variant: an
		// octahedral normal in xy, and roughness in z with the sign of the
		// normal's z folded into it. Written to a float image, which reads back
		// the same.
		vec4 packNormalRoughness(vec3 n, float roughness)
		{
			n /= abs(n.x) + abs(n.y) + abs(n.z);
			vec3 r;
			r.y = n.y * 0.5 + 0.5;
			r.x = n.x * 0.5 + r.y;
			r.y -= n.x * 0.5;
			roughness = max(roughness, 1.5 / 512.0);
			r.z = (n.z < 0.0 ? -roughness : roughness) * 0.5 + 0.5;
			return vec4(r, 0.0);
		}

		// The engine's volumetric lighting: how much a fog light glows in the
		// air between the eye and what the eye sees, and how much of that
		// thing the glow hides. Render.dll's own Fog routine, per pixel rather
		// than per fog map texel. Along the view ray, measured from the point
		// nearest the light, the glow integrates 3(1 - r^2/R^2) over the part
		// of the ray inside the light's sphere, scaled by the light's
		// strength, clamped to one, then doubled. Each light blends over the
		// ones before it, as the engine accumulates them.
		//
		// Then held to the fog light's shadows (fogLightSees): the glow is
		// taken at FogSteps points along the stretch inside the sphere, each
		// weighed by how much the engine's curve puts there, and scaled by
		// the share the light sees - after the clamp, so a shaft shows even
		// where the engine's glow is saturated. jitter is the pixel's offset
		// into the steps, as for flashlightBeam. Disable bit 65536 leaves the
		// shadows out.
		vec4 volumetricFog(vec3 origin, vec3 dir, float len, float jitter)
		{
			vec4 fog = vec4(0.0);
			uint first = Counts.y;
			uint count = lightGrid[7];
			bool shadowed = (Disable & 65536u) == 0u;
			for (uint i = 0u; i < count; i++)
			{
				SceneLight light = lights[first + i];
				float radius = light.PositionRadius.w;
				vec3 toLight = light.PositionRadius.xyz - origin;
				float along = dot(toLight, dir);
				float distance2 = dot(toLight, toLight);
				// Behind the eye only counts from inside the glow.
				if (along < 0.0 && distance2 >= radius * radius)
					continue;
				float across2 = max(distance2 - along * along, 0.0);
				if (across2 > radius * radius)
					continue;
				float halfChord = sqrt(radius * radius - across2);
				// The stretch of ray inside the sphere, measured back from the
				// point nearest the light: the eye sits at along, the surface at
				// along - len.
				float upper = min(halfChord, along);
				float lower = max(-halfChord, along - len);
				if (lower >= upper)
					continue;
				float k = 3.0 - 3.0 * across2 / (radius * radius);
				float u = upper / radius, l = lower / radius;
				float glow = ((k - u * u) * u - (k - l * l) * l) * light.ColorBrightness.w;
				glow = 2.0 * clamp(glow, 0.0, 1.0);
				if (glow <= 0.0)
					continue;
				if (shadowed && i < MaxFogShadows)
				{
					float seen = 0.0, weight = 0.0;
					for (int s = 0; s < FogSteps; s++)
					{
						float x = mix(l, u, (float(s) + jitter) / float(FogSteps));
						float w = max(k - 3.0 * x * x, 0.0);
						seen += w * fogLightSees(i, origin + dir * (along - x * radius) - light.PositionRadius.xyz);
						weight += w;
					}
					if (weight > 0.0)
						glow *= seen / weight;
				}
				float hides = min(glow * light.DirectionCone.x, 1.0);
				fog.rgb = min(fog.rgb * (1.0 - hides) + glow * light.ColorBrightness.rgb, vec3(1.0));
				fog.a = min(fog.a + hides, 1.0);
			}
			return fog;
		}
	)";

	source += R"(
		// Photo mode's focus when none is set: how far ahead the middle of the
		// view meets something, as the view sees it rather than as the
		// engine's collision would - through glass, smoke and the holes in a
		// grate, as the view ray passes them, and far off at a window onto
		// the sky. Every pixel traces the same ray, so all agree.
		float photoFocus()
		{
			vec3 dir = normalize(CameraForward.xyz);
			vec3 from = CameraOrigin.xyz;
			float travelled = 0.0;
			float start = RayEpsilon;
			ivec2 passed[8];
			uint passedCount = 0u;
			for (uint layer = 0u; layer < 8u; layer++)
			{
				rayQueryEXT rq;
				rayQueryInitializeEXT(rq, topLevel, gl_RayFlagsNoneEXT, ViewRays, from, start, dir, 100000.0);
				while (rayQueryProceedEXT(rq))
				{
					if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
					{
						ivec2 candidate = ivec2(rayQueryGetIntersectionInstanceIdEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
						// Through smoke and sparks without counting them.
						if (additiveGlow(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), candidate.y))
							continue;
						bool seen = false;
						for (uint p = 0u; p < passedCount; p++)
							seen = seen || passed[p] == candidate;
						if (!seen && confirmCandidate(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), candidate.y,
								rayQueryGetIntersectionBarycentricsEXT(rq, false), false, dir, mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, false))))
							rayQueryConfirmIntersectionEXT(rq);
					}
				}
				if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
					return 100000.0;
				float t = rayQueryGetIntersectionTEXT(rq, true);
				int primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
				float kind = tris[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + primitive].UV2Tex.w;
				if (kind > 4.5)
					return 100000.0;
				if ((kind < 1.5 || kind > 2.5) && kind < 3.5)
					return max(travelled + t, 1.0);
				// Translucent or modulated: looked through, as the view is.
				travelled += t;
				from += dir * t;
				start = 0.0;
				passed[passedCount++] = ivec2(rayQueryGetIntersectionInstanceIdEXT(rq, true), primitive);
			}
			return max(travelled, 1.0);
		}

		// Wet streets (the device's Wetness). How wet a point of the level is:
		// ground in a zone open to the sky (a level surface's Ambient.w over
		// 3.5), facing up, with nothing above it but the sky - an awning, a
		// balcony or a glass roof keeps what is under it dry, a grate's holes
		// let the rain through. Straight up rather than a spread of
		// directions, so the edge of what is covered stays put from one frame
		// to the next, as the denoiser needs the surface it is told of to.
		float wetnessAt(TriangleAttributes attr, vec3 position, vec3 normal)
		{
			if (Wetness <= 0.0 || attr.Ambient.w < 3.5 || normal.z < 0.7 || attr.UV2Tex.w > 1.5 || attr.Emission.w > 0.5)
				return 0.0;
			vec3 up = vec3(0.0, 0.0, 1.0);
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel, gl_RayFlagsNoneEXT, RainRays, position + normal * 0.5, RayEpsilon, up, 65536.0);
			while (rayQueryProceedEXT(rq))
			{
				if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
				{
					int attributeBase = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
					int primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
					// Smoke and a lamp's corona keep no rain off.
					if (tris[attributeBase + primitive].Emission.w < 1.5 &&
						confirmCandidate(attributeBase, primitive, rayQueryGetIntersectionBarycentricsEXT(rq, false), false, up,
							mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, false))))
						rayQueryConfirmIntersectionEXT(rq);
				}
			}
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
			{
				float kind = tris[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)].UV2Tex.w;
				if (kind < 4.5)
					return 0.0;
			}
			return clamp(Wetness, 0.0, 1.0) * smoothstep(0.7, 0.85, normal.z);
		}

		float puddleHash(ivec2 cell)
		{
			return float(pcgHash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u)) * (1.0 / 4294967296.0);
		}

		float puddleNoise(vec2 p)
		{
			ivec2 c = ivec2(floor(p));
			vec2 f = fract(p);
			f = f * f * (3.0 - 2.0 * f);
			return mix(mix(puddleHash(c), puddleHash(c + ivec2(1, 0)), f.x),
				mix(puddleHash(c + ivec2(0, 1)), puddleHash(c + ivec2(1, 1)), f.x), f.y);
		}

		// Where the water lies, over one field fixed to the ground: in its
		// hollows the ground is only damp; rising towards the puddles a film
		// of water gathers (x), sheen by sheen; and at the top of it water
		// stands (y), on what is flat. The wetter it is, the further the
		// film spreads and the more water stands. A film over all of it
		// alike was one flat sheen across the whole street.
		vec2 wetPatches(vec3 position, vec3 normal, float wet)
		{
			float n = 0.65 * puddleNoise(position.xy / 260.0) + 0.35 * puddleNoise(position.xy / 70.0 + vec2(17.3, 41.9));
			float film = smoothstep(0.66 - 0.2 * wet, 0.66, n) * wet;
			float puddle = smoothstep(0.62, 0.68, n) * smoothstep(0.97, 0.995, normal.z) * smoothstep(0.3, 0.8, wet);
			return vec2(film, puddle);
		}

		// A surface made wet: darker all over, as water fills its pores;
		// where a film has gathered, darker again and shining, its
		// reflection sharper the more water there is; and a mirror where
		// water stands. The reflection is traced, so the neon across the
		// street shows in it.
		void wetten(inout Material m, inout vec3 albedo, float wet, vec2 patches)
		{
			if (wet <= 0.0)
				return;
			float film = patches.x, puddle = patches.y;
			albedo *= mix(1.0, 0.65, wet) * mix(1.0, 0.8, film) * mix(1.0, 0.7, puddle);
			if (film <= 0.0 && puddle <= 0.0)
				return;
			m.roughness = mix(m.roughness, mix(0.25, 0.02, puddle), max(film, puddle));
			m.metalness *= 1.0 - puddle;
			m.reflectance = max(m.reflectance, 0.02);
			m.glossy = m.glossy || m.roughness < 0.8;
			m.traced = m.roughness < 0.5 && GlossBounces > 0u;
		}

		// A texel's height, for the relief: its brightness as displayed.
		// Deus Ex has no height or normal maps, but in what is built of
		// pieces the gaps between them are dark - the mortar between bricks,
		// the grout between tiles, a crack in concrete - and the faces
		// lighter, which is the shape near enough.
		float reliefHeight(uint index, vec2 uv, float lod)
		{
			return dot(textureLod(sceneTextures[nonuniformEXT(index)], uv, lod).rgb, vec3(0.2126, 0.7152, 0.0722));
		}

		// How much a texture's height rises across the surface, in world
		// units per unit: measured a texel apart at the level of the mip the
		// footprint's longer axis calls for, so a wall far off or seen along
		// its length flattens rather than sparkling. tu and tv are the
		// world's gradients of the texture's coordinates, scale how much
		// finer this texture is laid on.
		vec3 reliefSlope(uint index, vec2 uv, vec2 scale, Footprint footprint, vec3 tu, vec3 tv, float depth)
		{
			vec2 size = vec2(textureSize(sceneTextures[nonuniformEXT(index)], 0));
			float lod = max(log2(max(length(footprint.dx * scale * size), length(footprint.dy * scale * size))), 0.0);
			vec2 texel = exp2(lod) / size;
			float h = reliefHeight(index, uv, lod);
			float du = (reliefHeight(index, uv + vec2(texel.x, 0.0), lod) - h) / texel.x;
			float dv = (reliefHeight(index, uv + vec2(0.0, texel.y), lod) - h) / texel.y;
			return depth * (du * scale.x * tu + dv * scale.y * tv);
		}

		// Bump mapping (the device's BumpMapping): a flat surface's normal
		// tilted by the slope of its texture's height, as deep as its
		// material says (Materials.h), and by its detail texture's up close,
		// where the engine lays that on (detailFactor) - so a wall of bricks
		// or a paved floor is lit as the rough thing it is, by the lights,
		// the flashlight and what it reflects, rather than as one plane.
		// Only a flat surface carries the gradients of its texture across
		// it; a character's mesh keeps its own smooth normals. depth is how
		// far in front of the eye it is, 0 for no detail texture; flatten
		// how much of it standing water has smoothed over.
		vec3 reliefNormal(TriangleAttributes attr, vec2 bary, vec3 dir, vec3 normal, vec3 faceNormal, Footprint footprint,
			mat3 worldToObject, float depth, float flatten)
		{
			if (BumpStrength <= 0.0 || flatten >= 1.0 || !footprint.graded || attr.Normal.w > 0.5 || attr.Emission.w > 0.5 || attr.UV2Tex.w > 1.5)
				return normal;
			int index = int(attr.UV2Tex.z);
			if (index < 0 || uint(index) >= TextureCount)
				return normal;
			float relief = floor(max(materials[index].z, 0.0)) / 8.0 * BumpStrength * (1.0 - flatten);
			if (relief <= 0.0)
				return normal;

			vec2 lengths = unpackHalf2x16(attr.CornerNormals.z);
			mat3 toWorld = transpose(worldToObject);
			vec3 tu = toWorld * (unpackUnitVector(attr.CornerNormals.x) * lengths.x);
			vec3 tv = toWorld * (unpackUnitVector(attr.CornerNormals.y) * lengths.y);
			vec2 uv = surfaceUV(attr, bary, dir, normal);
			vec3 slope = reliefSlope(uint(index), uv, vec2(1.0), footprint, tu, tv, relief);

			// The detail texture's grain, a quarter unit deep, fading out
			// with its first pass: the finer passes are finer than its
			// relief would show.
			if (depth > 0.0 && attr.CornerOffsets.x != 0u && (Disable & 2048u) == 0u)
			{
				uint detail = attr.CornerOffsets.x - 1u;
				float fade = clamp(100.0 / 255.0 * (380.0 / max(depth, 1.0) - 1.0), 0.0, 1.0);
				if (detail < TextureCount && fade > 0.0)
				{
					vec2 scale = uintBitsToFloat(attr.CornerOffsets.yz);
					slope += reliefSlope(detail, uv * scale, scale, footprint, tu, tv, 0.25 * BumpStrength * (1.0 - flatten) * fade);
				}
			}

			// The height stands out along the surface's own front, and the
			// normal here faces whichever way the ray came from.
			if (dot(toWorld * attr.Normal.xyz, faceNormal) < 0.0)
				slope = -slope;
			vec3 tilted = normalize(normal - (slope - dot(slope, normal) * normal));
			// Never turned past the surface itself, or away from the eye.
			float facing = dot(tilted, faceNormal);
			if (facing < 0.2)
				tilted = normalize(tilted + faceNormal * (0.2 - facing));
			float seen = dot(tilted, -dir);
			if (seen < 0.02)
				tilted = normalize(tilted - dir * (0.02 - seen));
			return tilted;
		}
	)";

	return source;
}

std::string Shaders::Trace()
{
	std::string source = TraceCommon();
	source += R"(
		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			ivec2 size = imageSize(outImage);
			if (pixel.x >= size.x || pixel.y >= size.y)
				return;

			rngState = pcgHash(uint(pixel.x) + uint(pixel.y) * 9781u + Counts.x * 26699u);
			// The pixel's offset into the steps the fog and the flashlight's
			// beam are marched in: interleaved gradient noise, fixed per
			// pixel, so neighbours step differently but no pixel changes from
			// one frame to the next. Neither passes through the history.
			float dither = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));

			// Jitter inside the pixel: this is the whole of the antialiasing,
			// and it costs nothing because the samples are being averaged anyway.
			// For an upscaler - Ray Reconstruction, FSR (Disable bit 4194304) -
			// it is one offset for the whole frame, the one it is told: it
			// rebuilds detail finer than a pixel from frames sampled at known,
			// evenly spread offsets, and a random one per pixel would be a lie
			// about where each sample was taken.
			vec2 jitter = (Disable & 4194304u) != 0u ? frameJitter.xy + vec2(0.5) : vec2(randomFloat(), randomFloat());
			vec2 uv = (vec2(pixel) + jitter) / vec2(size) * 2.0 - 1.0;

			vec3 origin = CameraOrigin.xyz;
			vec3 direction = normalize(CameraForward.xyz + CameraRight.xyz * (uv.x + viewShift.x) + CameraUp.xyz * (uv.y + viewShift.y));
			vec3 viewDirection = direction;

			// Photo mode's depth of field: a thin lens, each sample's ray from
			// a point spread over its aperture through where the pinhole's
			// ray meets the plane in focus, so that plane alone is sharp.
			bool photo = (Disable & 524288u) != 0u;
			if (photo && photoLens.x > 0.0)
			{
				float focusDistance = photoLens.y > 0.0 ? photoLens.y : photoFocus();
				vec3 focus = origin + direction * (focusDistance / dot(direction, CameraForward.xyz));
				float r = photoLens.x * sqrt(randomFloat());
				float a = 6.28318531 * randomFloat();
				origin += (normalize(CameraRight.xyz) * cos(a) + normalize(CameraUp.xyz) * sin(a)) * r;
				direction = normalize(focus - origin);
			}

			// The ray's footprint, for choosing mip levels (surfaceLod): a cone
			// a pixel across at the eye - an output pixel across when Ray
			// Reconstruction upscales - widening as it goes. coneWidth is its
			// width where the ray now starts, coneSpread how fast it widens.
			float pixelSpread = 2.0 * length(CameraUp.xyz) / float(size.y) * exp2(MipBias);
			float coneWidth = 0.0;
			float coneSpread = pixelSpread;
			float surfaceConeWidth = 0.0;

			vec3 radiance = vec3(0.0);
			vec3 throughput = vec3(1.0);
			// Passing through a translucent surface is not a bounce: a window
			// with a pane and a frame would otherwise use up the ray's budget
			// before it reached anything solid. Up to sixteen of them; glows
			// (glowAlong) are not passed one by one and do not count.
			uint passes = 0u;
			// How far along the ray to start looking. Generous for a bounce off
			// a surface, because these levels are big and a surface acne
			// artefact is worse than a lost millimetre - but none at all when
			// carrying on through a surface, since the thing behind it may be
			// flush against it. A laser dot sits on a wall, and stepping a whole
			// unit past it skipped the wall entirely and put a hole in the level.
			// The layer just passed is refused by name instead: see passedLayers.
			float rayMin = RayEpsilon;

			// What this pixel is looking at, recorded on the first bounce so the
			// accumulated history can be checked against it.
			vec3 primaryPosition = origin + direction * 100000.0;
			float primaryInstance = -1.0;
			// Set when the view ray passed through something that changed this
			// frame - a new decal, say - on its way to what it finally hit.
			bool primaryChanged = false;
			// The first surface's light was cut by something that moved.
			bool primaryMoverShadow = false;
			bool inSky = false;
			float primaryDistance = 100000.0;
			// How far the view goes through the level's air before it meets
			// something solid or goes out through a window onto the sky:
			// what the fog and the flashlight's beam fill. Measured from the
			// eye, where primaryDistance is from whatever glass the ray last
			// passed and, beyond a sky window, a distance in the skybox.
			float airDistance = 100000.0;
			// Whether what the eye meets is in a fog zone: the engine lays
			// volumetric fog only over a surface or an actor in one - a
			// level surface's Ambient.w carries 2 more there, an instance's
			// 32 - and never over the skybox.
			bool primaryFogged = false;

			// The first solid surface the eye meets, through any glass, decals
			// and the like in front of it: what a denoiser works on. Its
			// lighting is gathered as though it were white, and its colour -
			// and whatever the glass in front did to it - is kept apart.
			bool surfaceFound = false;
			bool pendingSpecular = false;    // part mirror: its reflection is traced next
			// The first lit surface seen in a mirror, captured the same way as
			// the surface itself, for the denoiser's second pass.
			bool reflectionFound = false;
			vec3 reflectionThroughput = vec3(1.0);
			vec3 reflectionEmission = vec3(0.0);
			vec3 reflectionAlbedo = vec3(0.0);
			vec3 reflectionNormal = vec3(0.0);
			float reflectionDistance = 0.0;   // from the mirror to it
			vec3 specularOrigin = vec3(0.0);
			vec3 specularDirection = vec3(0.0);
			vec3 surfaceThroughput = vec3(1.0);
			vec3 emission = vec3(0.0);
			vec3 diffuseAlbedo = vec3(0.0);
			vec3 specularAlbedo = vec3(0.0);
			vec3 surfaceNormal = vec3(0.0);
			float surfaceRoughness = 1.0;
			// A glossy first surface. Its reflection is traced in the second
			// pass like a mirror's, but denoised as a reflection rather than as
			// a surface of its own. A surface is never both, so the glossy one
			// keeps its state in the mirror's variables rather than a set of
			// its own: every value carried through the path costs the whole
			// trace, glossy or not, in how many pixels the GPU can keep in
			// flight. For a glossy surface:
			//   specularOrigin, specularDirection  where its reflection starts
			//   reflectionThroughput               what that ray carries
			//   specularAlbedo                     the colour it goes back on with
			//   reflectionEmission                 what the lights and the ambient
			//                                      give it directly
			bool glossySurface = false;
			bool pendingGloss = false;
			float surfaceMetalness = 0.0;
			vec3 surfacePosition = origin + direction * 100000.0;
			vec3 surfaceObject = surfacePosition;   // the same point in its instance's own space
			int surfaceInstance = -1;
			bool wantHitDistance = false;
			float hitDistance = 0.0;

			// The path's bounces in the low byte, and in the next how many a
			// glossy surface's reflection is given: the push constants are at
			// the size every device must allow.
			uint bounces = max(Counts.z & 255u, 1u);
			// Two passes on a surface that is part mirror, so that a denoiser
			// has both its halves every frame: the path off the surface as
			// usual, then the reflection on its own, from the surface on.
			uint firstBounce = 0u;
			vec3 diffuseRadiance = vec3(0.0);
			float diffuseHitDistance = 0.0;
			for (int lobePass = 0; lobePass < 2; lobePass++)
			{
				glowWeights = 0.0;
				if (lobePass == 1)
				{
					if (!pendingSpecular && !pendingGloss)
						break;
					diffuseRadiance = radiance;
					diffuseHitDistance = hitDistance;
					radiance = vec3(0.0);
					throughput = pendingGloss ? reflectionThroughput : vec3(1.0);
					origin = specularOrigin;
					direction = specularDirection;
					// A mirror keeps the view's cone; a glossy reflection spreads.
					coneWidth = surfaceConeWidth;
					coneSpread = pendingGloss ? 0.1 : pixelSpread;
					rayMin = LiftedRayMin;
					passes = 0u;
					hitDistance = 0.0;
					wantHitDistance = true;
					firstBounce = 1u;
				}

				// The translucent layers already passed through on the way to the
				// next solid thing. Layers can lie exactly on top of one another -
				// the Dragon's Tooth blade is a core, a glow and an edge in one
				// plane - so rather than skip a little way past each one, which
				// skipped the others too, the ray carries on from where it hit
				// and refuses only the triangles it has already been through.
				ivec2 passedLayers[8];
				uint passedCount = 0u;
				bool passingThrough = false;
	)";

	source += R"(
				// A glossy reflection goes as far as its own budget allows:
				// with one, what it shows is lit by the lights and the ambient
				// but not by light bounced on from there.
				uint lastBounce = (lobePass == 1 && pendingGloss) ? min(bounces, 1u + GlossBounces) : bounces;
				for (uint bounce = firstBounce; bounce < lastBounce; bounce++)
				{
					if (!passingThrough)
						passedCount = 0u;
					passingThrough = false;
					rayQueryEXT rq;
					// The view's own ray until it first bounces: passing through
					// glass or into the sky zone keeps it at bounce zero.
					// A view in a window of the HUD's is someone else's
					// (Disable bit 32768): it sees the player, and neither
					// the weapon at the player's eyes nor its own camera.
					uint cullMask = (lobePass == 0 && bounce == 0u) ? ((Disable & 32768u) != 0u ? WindowRays : ViewRays) : BouncedRays;
					rayQueryInitializeEXT(rq, topLevel, (Disable & 8u) != 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT, cullMask, origin, rayMin, direction, 100000.0);
					// Glows are never stopped at: those before the next thing
					// the ray does stop at are summed afterwards (glowAlong).
					bool glowsMet = false;
					while (rayQueryProceedEXT(rq))
					{
						// Only geometry holding masked or translucent art is
						// non-opaque, so this runs for grates, windows and glass and
						// nothing else.
						if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
						{
							ivec2 candidate = ivec2(rayQueryGetIntersectionInstanceIdEXT(rq, false), rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
							if (additiveGlow(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), candidate.y))
							{
								glowsMet = true;
								continue;
							}
							bool passed = false;
							for (uint p = 0u; p < passedCount; p++)
								passed = passed || passedLayers[p] == candidate;
							if (!passed && confirmCandidate(
									rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false),
									candidate.y,
									rayQueryGetIntersectionBarycentricsEXT(rq, false),
									false, direction, mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, false))))
								rayQueryConfirmIntersectionEXT(rq);
						}
					}

					if (glowsMet)
					{
						bool glowChanged = false;
						float reach = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 100000.0 : rayQueryGetIntersectionTEXT(rq, true);
						radiance += throughput * glowAlong(origin, direction, rayMin, reach, cullMask, coneWidth, coneSpread, bounce > firstBounce, glowChanged);
						// The pixel records what lies behind them as what it
						// sees, so one arriving would otherwise fade in.
						if (bounce == 0u && glowChanged)
							primaryChanged = true;
					}

					if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
					{
						if (wantHitDistance)
							hitDistance = 100000.0;
						// Out through the side of the skybox there is nothing: the
						// engine shows black there. The stand-in sky is only for a
						// level with no sky zone, and lighting the inside of the
						// skybox with it is what washed the clouds out.
						if (!inSky)
							radiance += throughput * skyLight(direction);
						break;
					}

					float t = rayQueryGetIntersectionTEXT(rq, true);
					// The footprint's width here, which is where the ray carries
					// on from, through glass or off a bounce.
					float hitWidth = coneWidth + coneSpread * t;
					coneWidth = hitWidth;
					// How far the first ray off the surface went, which a denoiser
					// uses to judge how widely that light can be blurred.
					if (wantHitDistance)
					{
						hitDistance = t;
						wantHitDistance = false;
					}
					int primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);

					// Each instance carries the offset of its geometry's shading
					// data as its custom index, so one buffer serves every shape.
					int attributeBase = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
					TriangleAttributes attr = tris[attributeBase + primitive];

					if (bounce == 0u)
					{
						if (!inSky)
						{
							airDistance = distance(CameraOrigin.xyz, origin + direction * t);
							primaryFogged = mod(attr.Ambient.w, 4.0) > 1.5 || abs(instanceAmbient[rayQueryGetIntersectionInstanceIdEXT(rq, true)].w) > 32.0;
						}
						primaryDistance = t;
						primaryPosition = origin + direction * t;
						primaryInstance = float(rayQueryGetIntersectionInstanceIdEXT(rq, true));
						// A surface whose texture animates has nothing worth reusing
						// from earlier frames: averaging a screen against what it
						// showed a second ago is what made them look frozen while
						// standing still.
						if (attr.Albedo.w > 0.5)
							primaryInstance = -2.0 - float(Counts.x);
					}
					// Normals are stored in object space, because the same mesh is
					// instanced at whatever orientation the actor happens to have.
					// Rotating by the instance transform is what puts it back in the
					// world - without it every mover and character would be lit as
					// though it had never turned.
					mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
					vec3 faceNormal = normalize(mat3(objectToWorld) * attr.Normal.xyz);
					// These surfaces are single sided in the engine but solid from
					// either direction here, so face the normal back at the ray.
					float side = dot(faceNormal, direction) > 0.0 ? -1.0 : 1.0;
					faceNormal *= side;

					vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
					vec3 position = origin + direction * t;
					vec3 lifted;
					vec3 normal = smoothNormal(attr, bary, mat3(objectToWorld), faceNormal, side, position, lifted);
					Footprint footprint;
					footprint.logWidth = footprintOf(hitWidth, faceNormal, direction, objectToWorld, attr.Normal.xyz);
					footprint.graded = footprintAxes(attr, hitWidth, faceNormal, direction,
						mat3(rayQueryGetIntersectionWorldToObjectEXT(rq, true)), footprint.dx, footprint.dy);
					attr.Albedo = vec4(surfaceAlbedo(attr, bary, direction, normal, footprint), attr.Albedo.w);
					// The engine lays a detail texture over what is in view, a
					// mirror's reflection included, where the depth is the
					// reflection's own, as far behind the glass as it looks.
					if (bounce == firstBounce && attr.UV2Tex.w < 1.5 && attr.CornerOffsets.x != 0u && !inSky)
					{
						float depth = dot(position - CameraOrigin.xyz, CameraForward.xyz);
						if (lobePass == 1)
						{
							vec3 toMirror = surfacePosition - CameraOrigin.xyz;
							depth = dot(toMirror, CameraForward.xyz) * (1.0 + distance(position, specularOrigin) / max(length(toMirror), 1.0));
						}
						if (depth > 0.0)
							attr.Albedo.rgb *= detailFactor(attr, surfaceUV(attr, bary, direction, normal), depth, footprint);
					}

					// Translucent: add what this surface contributes and carry on
					// through it in the same direction. UE1 draws these additively,
					// which is why the muzzle flash quad on a weapon is invisible
					// until it is lit and why a red dot sight glows rather than
					// showing as a dark blob.
					float kind = attr.UV2Tex.w;

					// A sprite is as bright as its actor's ScaleGlow, which the
					// instance carries in place of an ambient it has no use for.
					bool sprite = attr.Emission.w > 1.5;
					// So is an unlit actor's mesh (Emission.w 1.25), whose instance
					// carries its ScaleGlow in the same place.
					// The engine scales the colour as displayed, and textures here
					// are made linear, so the same dimming is the glow to the power
					// 2.2: scaling linear light by 0.175 put an unlit tree on screen
					// at over twice the brightness the engine draws it.
					float glow = attr.Emission.w > 1.1 ? pow(max(instanceAmbient[rayQueryGetIntersectionInstanceIdEXT(rq, true)].x, 0.0), 2.2) : 1.0;

					// A window onto the sky zone. The engine draws the skybox from
					// the sky zone's viewpoint, looking the view's way turned by
					// the sky zone's rotation, so the ray does exactly that:
					// turned the same, from the new start. Unturned, Liberty
					// Island's skyline stood well off to the side of where the
					// engine puts it.
					// Only once per path - a second window means there is no sky
					// zone behind this one, and the stand-in sky is all there is.
					if (kind > 4.5)
					{
						if (SkyOrigin.w > 0.5 && !inSky && (Disable & 4u) == 0u)
						{
							inSky = true;
							primaryFogged = false;
							glowWeights = 0.0;
							origin = SkyOrigin.xyz;
							direction = skyAxes[0].xyz * direction.x + skyAxes[1].xyz * direction.y + skyAxes[2].xyz * direction.z;
							rayMin = RayEpsilon;
							// The eye is at the sky zone's viewpoint now, so the
							// cone starts again from a point there. Carried on from
							// the window, however far off that was, it reached the
							// skybox - a small room close by - as wide as the
							// window's distance made it, and blurred the skyline.
							coneWidth = 0.0;
							if (passes < 16u)
							{
								passes++;
								bounce--;
							}
							continue;
						}
						// Out through the side of the skybox there is nothing: the
						// engine shows black there. The stand-in sky is only for a
						// level with no sky zone, and lighting the inside of the
						// skybox with it is what washed the clouds out.
						if (!inSky)
							radiance += throughput * skyLight(direction);
						break;
					}

					if ((kind > 1.5 && kind < 2.5) || kind > 3.5)
					{
						// The pixel records the surface behind this one as what it
						// sees, so this one appearing would otherwise go unnoticed
						// and fade in over many frames.
						if (bounce == 0u && instanceAmbient[rayQueryGetIntersectionInstanceIdEXT(rq, true)].w < 0.0)
							primaryChanged = true;
	)";

	source += R"(
						if (kind > 3.5)
						{
							// Modulated: the surface multiplies what is behind it,
							// as modulate-2x, so mid grey leaves the background
							// alone. Treating it as additive along with translucent
							// turned a pair of dark sunglasses bright white.
							//
							// The engine modulates in gamma space, where mid grey
							// times two is exactly one. In linear terms that is
							// 2^2.2 times the linear colour; doubling the linear
							// value instead darkened what should vanish, and left
							// every decal sitting in a grey square.
							throughput *= clamp(attr.Albedo.rgb * 4.595, vec3(0.0), vec3(4.595));
						}
						else
						{
							// Translucent: additive, so black is invisible and
							// bright glows. But additive of the surface as lit,
							// not as it would glow: the engine multiplies it by
							// its lighting like any other surface unless it is
							// unlit. Adding the bare texture drew the Liberty
							// Island sea and the skybox clouds - translucent,
							// lit, with no light anywhere near - bright grey
							// where the original shows them nearly black.
							vec3 contribution;
							if (sprite || attr.Emission.w > 0.5)
							{
								contribution = bounce > firstBounce ? ((Disable & 512u) != 0u ? vec3(0.0) : attr.Albedo.rgb * (glow * GlowScale)) : attr.Albedo.rgb * glow;
							}
							else
							{
								vec3 surroundings = linearAmbient(attr, rayQueryGetIntersectionInstanceIdEXT(rq, true));
								vec3 unusedDirection, unusedBase;
								contribution = attr.Albedo.rgb * directLight(position, normal, mod(attr.Ambient.w, 2.0) > 0.5, true, -1.0, direction,
									pow(surroundings, vec3(1.0 / 2.2)), 0u, uint(attr.Emission.z + 0.5),
									rayQueryGetIntersectionWorldToObjectEXT(rq, true) * vec4(position, 1.0), unusedDirection, unusedBase);
								contribution += attr.Albedo.rgb * (EngineLighting ? lightmapAmbient : surroundings);
								// The flashlight on it too, with no shadow ray,
								// as for the lights.
								vec3 flashBase, flashDir;
								float flashDistance;
								if (!inSky && flashlightAt(position, normal, flashBase, flashDir, flashDistance))
									contribution += attr.Albedo.rgb * flashBase * dot(normal, flashDir);
							}
							radiance += throughput * contribution;
							// A path gathering light, rather than the view looking
							// through, is tinted by the glass as a shadow ray is:
							// the light behind stained glass bounces in coloured,
							// and a glowing surface behind it is the same colour
							// whichever way it is found.
							if (bounce > firstBounce)
								throughput *= glassTransmittance(attributeBase, primitive, rayQueryGetIntersectionBarycentricsEXT(rq, true),
									direction, mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)));
						}

						// Whatever was passed before lies behind the new start
						// and cannot be met again, unless it was in the same
						// plane: only those need refusing. Past eight of them
						// in one plane - blood on blood - the ray steps on past
						// it rather than meet the ninth over and over.
						if (t > 0.05)
							passedCount = 0u;
						origin = position;
						rayMin = 0.0;
						if (passedCount < 8u)
							passedLayers[passedCount++] = ivec2(rayQueryGetIntersectionInstanceIdEXT(rq, true), primitive);
						else
							rayMin = 0.01;
						passingThrough = true;
						if (passes < 16u)
						{
							passes++;
							bounce--;
						}
						continue;
					}

					// A solid or masked sprite is just its picture: nothing lights it
					// and nothing bounces off it.
					if (sprite && (Params.w < 1.5 || Params.w > 2.5))
					{
						radiance += throughput * attr.Albedo.rgb * glow;
						break;
					}

					// A reflective surface - the polished floor of the UNATCO lobby
					// is the one that shows. Half the rays carry on in the mirrored
					// direction and half shade the surface itself, which averages
					// out to a floor that is both marble and a reflection. Without
					// this the flag meant nothing and the floor was a flat slab of
					// whatever colour its texture averaged to.
					// The denoiser's surface, when this is the first solid thing seen
					// and it is lit. Both halves of a part-mirror surface are
					// described, whichever one this sample takes. An unlit surface
					// is left out - it is its own colour, with nothing to denoise -
					// unless it is also part mirror, like the one-way glass in the
					// clubs: its reflection needs the denoiser as much as any.
					bool unlitSurface = attr.Emission.w > 0.5;
					bool firstSurface = bounce == 0u && !surfaceFound && !inSky && (!unlitSurface || attr.UV2Tex.w > 2.5) &&
						(Params.w < 0.5 || Params.w > 2.5);
					// Wet streets, on the first surface the view meets: what
					// it reflects, and anything further along a path, is
					// shaded dry.
					float wet = 0.0;
					vec2 wetness = vec2(0.0);
					if (firstSurface && lobePass == 0 && Wetness > 0.0)
					{
						wet = wetnessAt(attr, position, normal);
						if (wet > 0.0)
							wetness = wetPatches(position, normal, wet);
					}
					// The relief, on what the view meets first, in a mirror
					// too: past that it is lit as flat. Standing water
					// smooths it over.
					if (bounce == firstBounce && !inSky && BumpStrength > 0.0)
						normal = reliefNormal(attr, bary, direction, normal, faceNormal, footprint,
							mat3(rayQueryGetIntersectionWorldToObjectEXT(rq, true)),
							lobePass == 0 ? dot(position - CameraOrigin.xyz, CameraForward.xyz) : 0.0, wetness.y);
					// Likewise the first lit surface seen in a mirror. A mirror
					// seen in a mirror gives only its own half here: its
					// reflection would need a third pass.
					bool reflectedSurface = lobePass == 1 && pendingSpecular && bounce == 1u && !reflectionFound && !inSky && attr.Emission.w < 0.5 &&
						(Params.w < 0.5 || Params.w > 2.5);
					bool captured = firstSurface || reflectedSurface;
					vec3 mirrorTint = 0.55 + 0.45 * clamp(attr.Albedo.rgb * 2.5, vec3(0.0), vec3(1.0));

					if (reflectedSurface)
					{
						reflectionFound = true;
						reflectionThroughput = throughput;
						reflectionEmission = radiance;
						radiance = vec3(0.0);
						throughput = vec3(1.0);
						reflectionAlbedo = attr.Albedo.rgb * (attr.UV2Tex.w > 2.5 ? 0.5 : 1.0);
						reflectionNormal = normal;
						reflectionDistance = distance(position, specularOrigin);
						hitDistance = 0.0;
						wantHitDistance = true;
					}
					if (firstSurface)
					{
						surfaceFound = true;
						surfaceThroughput = throughput;
						emission = radiance;
						radiance = vec3(0.0);
						throughput = vec3(1.0);
						// A part-mirror surface is half each, as the two halves
						// were taken half the time each before.
						bool mirror = attr.UV2Tex.w > 2.5;
						// What it is made of, and what that makes of its colour.
						// Worked out here and again where it is shaded rather
						// than kept in between.
						Material material = surfaceMaterial(attr);
						vec3 surfaceColour = attr.Albedo.rgb;
						wetten(material, surfaceColour, wet, wetness);
						vec3 toEye = -direction;
						vec3 f0, shineAlbedo, diffuseColour;
						splitColour(material, surfaceColour,
							max(abs(dot(normal, toEye)), 1.0e-4), f0, shineAlbedo, diffuseColour);
						diffuseAlbedo = surfaceThroughput * diffuseColour * (mirror ? 0.5 : 1.0);
						// Unlit, its half is exactly its texture: emission, with no
						// lighting for the denoiser.
						if (unlitSurface)
						{
							emission += diffuseAlbedo * glow;
							diffuseAlbedo = vec3(0.0);
						}
						specularAlbedo = mirror ? surfaceThroughput * mirrorTint * 0.5 : vec3(0.0);
						surfaceNormal = normal;
						if (mirror)
						{
							pendingSpecular = true;
							specularOrigin = lifted + faceNormal * RayEpsilon;
							specularDirection = aboveFace(reflect(direction, surfaceNormal), faceNormal);
						}
						surfaceRoughness = mirror ? 0.0 : (material.glossy ? material.roughness : 1.0);
						surfaceMetalness = material.metalness;
						// Glossy but too rough to trace, its reflection is what the
						// lights and the ambient give it directly.
						glossySurface = material.glossy;
						if (glossySurface)
							specularAlbedo = surfaceThroughput * shineAlbedo;
						if (material.traced && lobePass == 0)
						{
							vec3 weight;
							vec3 L;
							if (sampleGlossy(material, f0, surfaceNormal, toEye, L, weight))
							{
								pendingGloss = true;
								specularOrigin = lifted + faceNormal * RayEpsilon;
								specularDirection = aboveFace(L, faceNormal);
								// Divided by the colour it is put back with.
								reflectionThroughput = weight / max(shineAlbedo, vec3(1.0e-4));
							}
						}
						surfacePosition = position;
						surfaceObject = rayQueryGetIntersectionWorldToObjectEXT(rq, true) * vec4(position, 1.0);
						surfaceConeWidth = hitWidth;
						surfaceInstance = rayQueryGetIntersectionInstanceIdEXT(rq, true);
						wantHitDistance = true;
					}

					// The denoiser's surface takes its reflection in a pass of its
					// own, so here it always takes the other half.
					if (attr.UV2Tex.w > 2.5 && !captured && randomFloat() < 0.5)
					{
						// Tinted by the floor but not dimmed to nothing by it: dark
						// marble has an albedo near 0.1, and multiplying the
						// reflection by that made it invisible.
						throughput *= mirrorTint;
						origin = lifted + faceNormal * RayEpsilon;
						rayMin = LiftedRayMin;
						direction = aboveFace(reflect(direction, normal), faceNormal);
						glowWeights = 0.0;
						continue;
					}

	)";

	source += R"(
					// Debug: light every instance that is not the static world, so
					// that "the actors are not being drawn" can be told apart from
					// "the actors are drawn and too dark to see". The static world
					// is the only geometry whose attributes start at zero.
					// Albedo only: no lights, no ambient, no bounces. If something is
					// invisible in the finished image but plain here, it is lit
					// wrongly rather than missing.
					if (Params.w > 1.5 && Params.w < 2.5)
					{
						radiance = attr.Albedo.rgb;
						break;
					}

					if (Params.w > 0.5 && Params.w < 1.5)
					{
						// Which instances the history is being thrown away for.
						//   green:   an instance flagged as moved or changed shape
						//            this frame - a door while it swings, a
						//            character mid animation, every sprite.
						//   magenta: an instance that is holding still.
						//   dim:     the static world, which is instance zero.
						int instanceId = rayQueryGetIntersectionInstanceIdEXT(rq, true);
						if (instanceId > 0)
						{
							bool moved = instanceAmbient[instanceId].w < 0.0;
							radiance = (moved ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 1.0)) * 4.0;
							break;
						}
						radiance += throughput * attr.Albedo.rgb * 0.1;
						break;
					}

					// Self lit surfaces emit the colour they actually are, which is
					// only known once the texture has been sampled. Emission.w is
					// the flag; the rgb carries nothing.
					//
					// That is all an unlit surface is: the engine shows its texture
					// at full brightness and nothing reaches it, so there is no
					// lighting to gather and nothing to bounce. Carrying on from it
					// is what made the unlit skyline cost a full path per pixel.
					if (attr.Emission.w > 0.5)
					{
						// Already counted, as emission, when it is the
						// denoiser's surface. PT NOGLOW (Disable bit 512) has
						// it light nothing, to see what glowing surfaces add
						// to a room.
						// Found by a bounce, it is weighed against its chance
						// of having been sampled as a light (glowFound).
						if (!firstSurface && !(bounce > firstBounce && (Disable & 512u) != 0u))
							radiance += throughput * attr.Albedo.rgb * (glow * (bounce > firstBounce ? glowFound(attr.Emission.z, position, direction) : 1.0));
						break;
					}
					// Shaded as white on the denoiser's surface: its colour goes back
					// on afterwards. Its glossy reflection likewise, gathered apart
					// and divided by the colour it goes back on with.
					vec3 lightDirection, lightBase;
					// A mesh is lit as the engine lights one, at 1.4 times its
					// ScaleGlow (the instance's w, 1 + ScaleGlow, 32 more in a
					// fog zone, signed by whether it moved), unless PT
					// MESHLIGHT has it lit as a flat surface is (Disable bit
					// 1024).
					int hitInstance = rayQueryGetIntersectionInstanceIdEXT(rq, true);
					float meshGlow = -1.0;
					if (attr.CornerNormals.w != 0u && (Disable & 1024u) == 0u)
					{
						float flags = mod(abs(instanceAmbient[hitInstance].w), 32.0);
						meshGlow = 1.4 * (flags > 0.5 ? flags - 1.0 : 1.0);
					}
					vec3 ambient = linearAmbient(attr, hitInstance);
					vec3 lit = directLight(lifted, normal, mod(attr.Ambient.w, 2.0) > 0.5, false, meshGlow, direction,
						pow(ambient, vec3(1.0 / 2.2)), firstSurface ? 4u : 1u, uint(attr.Emission.z + 0.5),
						rayQueryGetIntersectionWorldToObjectEXT(rq, true) * vec4(lifted, 1.0), lightDirection, lightBase);
					if (meshGlow >= 0.0)
						lit = meshLight(lit, instanceAmbient[hitInstance].rgb);
					// The flashlight, on meshes and flat surfaces alike by the
					// cosine, and on top of what the engine's lights sum to.
					// Not in the skybox, which is somewhere else entirely.
					vec3 flashBase, flashDir;
					float flashDistance;
					bool flashLit = !inSky && flashlightAt(lifted, normal, flashBase, flashDir, flashDistance);
					if (flashLit)
					{
						flashBase *= flashlightReaches(lifted, flashDir, flashDistance);
						flashLit = luminance(flashBase) > 0.0;
					}
					if (flashLit)
						lit += flashBase * dot(normal, flashDir);

					// What it is made of, only now the light loop is done with.
					// A surface seen in a mirror is captured as matte, since its
					// own reflection would need a pass of its own.
					Material material = reflectedSurface ? matte() : surfaceMaterial(attr);
					vec3 surfaceColour = attr.Albedo.rgb;
					wetten(material, surfaceColour, wet, wetness);
					vec3 toEye = -direction;
					vec3 f0, shineAlbedo, diffuseColour;
					splitColour(material, surfaceColour, max(dot(normal, toEye), 1.0e-4), f0, shineAlbedo, diffuseColour);
					bool glossCapture = firstSurface && material.glossy;
					// Which way the path goes on from here is chosen below, a
					// glossy surface's in proportion to what each half
					// reflects; the share going the matte way weighs what the
					// glowing surfaces give (glowLight). The denoiser's surface
					// always takes the matte half.
					float pShine = 0.0;
					if (!captured && material.glossy)
					{
						float shineWeight = luminance(shineAlbedo);
						float matteWeight = luminance(diffuseColour);
						pShine = matteWeight <= 0.0 ? 1.0 : clamp(shineWeight / max(shineWeight + matteWeight, 1.0e-4), 0.05, 0.95);
					}
					// Only on the first two surfaces a path meets: past them
					// what a glowing surface gives is faint, and the bounces
					// alone find it well enough.
					if (!inSky && bounce <= firstBounce + 1u)
						lit += glowLight(lifted, normal, 1.0 - pShine, luminance(lit));
					else
						glowWeights = 0.0;
					vec3 shade = captured ? vec3(1.0) : diffuseColour;
					radiance += throughput * shade * lit;
					// Only the denoiser's own surface can be both captured and
					// glossy: one seen in a mirror is matte.
					if (material.glossy)
					{
						vec3 shine = lightBase * glossyLight(material, f0, normal, toEye, lightDirection);
						if (flashLit)
							shine += flashBase * glossyLight(material, f0, normal, toEye, flashDir);
						if (glossCapture)
							reflectionEmission += shine / max(shineAlbedo, vec3(1.0e-4));
						else
							radiance += throughput * shine;
					}
					if (bounce == 0u)
						primaryMoverShadow = shadowedByMover || litByChangingLight;

					// The zone's ambient. Level surfaces carry their own, because a
					// zone is a property of the surface; an instanced shape takes it
					// from wherever the actor happens to be standing. Without this
					// anything the light actors do not reach is pure black, which is
					// not what the engine shows. A glossy surface reflects it too,
					// as light arriving evenly from everywhere, which is what keeps
					// metal from going black where no light reaches it.
					// A lit mesh's is in its light already, as the engine adds it.
					if (meshGlow < 0.0)
						radiance += throughput * shade * (EngineLighting ? lightmapAmbient : ambient);
					if (glossCapture)
						reflectionEmission += ambient;
					else if (!captured)
						radiance += throughput * shineAlbedo * ambient;

					// The skybox is a backdrop. The engine never lights it by
					// anything bouncing inside it, and bouncing around a box the
					// size of the sky for every sky pixel is what the frame rate
					// was spent on, so the first surface there is the last.
					if (inSky)
						break;

					// All metal: nothing matte left for the diffuse path to find.
					if (firstSurface && max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b)) <= 0.0)
						break;
	)";

	source += R"(
					// Which way the path goes on. The denoiser's surface always
					// takes the matte half here, its reflection having a pass of
					// its own; anywhere else a glossy surface picks one of the
					// two in proportion to what each reflects.
					bool glossyBounce = false;
					if (!captured && material.glossy)
					{
						if (randomFloat() < pShine)
						{
							vec3 weight;
							vec3 L;
							if (!sampleGlossy(material, f0, normal, toEye, L, weight))
								break;
							throughput *= weight / pShine;
							direction = L;
							glossyBounce = true;
							glowWeights = 0.0;
						}
						else
						{
							shade = diffuseColour / (1.0 - pShine);
						}
					}
					if (!glossyBounce)
						throughput *= shade;

					// Russian roulette on the dim paths. Without it the loop spends
					// most of its time on bounces that cannot change the pixel.
					if (bounce >= 2u)
					{
						float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
						if (randomFloat() > p)
							break;
						throughput /= p;
					}

					origin = lifted + faceNormal * RayEpsilon;
					rayMin = LiftedRayMin;
					if (!glossyBounce)
						direction = cosineDirection(normal);
					direction = aboveFace(direction, faceNormal);
					// Light bounced off a surface is gathered from all over, and
					// the fine detail where it lands next hardly shows in it: a
					// wide cone, and coarse mips that are quicker to read.
					coneSpread = 0.25;
				}
			}

			// The picture put back together from the denoiser's parts.
			vec3 diffuseSignal = vec3(0.0);
			vec3 specularSignal = vec3(0.0);
			float specularHitDistance = 0.0;
			// What the mirror shows, and the part of it the denoiser takes:
			// the lighting on the surface seen in it, while whatever lay
			// between - glass, sprites, the sky - joins the emission.
			vec3 reflectionSignal = vec3(0.0);
			float reflectionHitDistance = 0.0;
			vec3 reflectionRemodulation = vec3(0.0);
			vec3 glossSignal = vec3(0.0);
			float glossHitDistance = 0.0;
			// The glossy surface's colour lives where a mirror's would.
			vec3 glossAlbedo = glossySurface ? specularAlbedo : vec3(0.0);
			if (glossySurface)
				specularAlbedo = vec3(0.0);
			if (pendingSpecular)
			{
				if (reflectionFound)
				{
					reflectionSignal = radiance;
					reflectionHitDistance = hitDistance;
					reflectionRemodulation = specularAlbedo * reflectionThroughput * reflectionAlbedo;
					specularSignal = reflectionEmission + reflectionThroughput * reflectionAlbedo * reflectionSignal;
				}
				else
				{
					specularSignal = radiance;
				}
				specularHitDistance = reflectionDistance;
			}
			else if (pendingGloss)
			{
				glossSignal = reflectionEmission + radiance;
				glossHitDistance = hitDistance;
			}
			else
			{
				glossSignal = glossySurface ? reflectionEmission : vec3(0.0);
				diffuseRadiance = radiance;
				diffuseHitDistance = hitDistance;
			}
			vec3 emissionPart = emission;
			if (surfaceFound)
			{
				diffuseSignal = diffuseRadiance;
				radiance = emission + diffuseAlbedo * diffuseSignal + specularAlbedo * specularSignal + glossAlbedo * glossSignal;
				emissionPart = emission + specularAlbedo * (reflectionFound ? reflectionEmission : specularSignal);
			}
			else
			{
				emission = radiance;
				emissionPart = radiance;
				surfacePosition = primaryPosition;
				surfaceObject = primaryPosition;
			}
			// The flashlight's beam in the air in front of it all: on the
			// picture, and with what NRD passes straight through. Ray
			// Reconstruction is given the picture without it, and it is added
			// to what comes back, as the fog is laid over it: it stays where
			// the lamp is as the view turns, which the picture's history,
			// following the surfaces, would drag after them.
			vec3 beam = flashlightBeam(CameraOrigin.xyz, viewDirection, airDistance, dither);
			if ((Disable & 256u) == 0u)
			{
				radiance += beam;
				emissionPart += beam;
			}

			// Depth along the view, and where this point was on screen last
			// frame: carried back by its instance's last placement, then seen
			// through last frame's camera. Measured in screen widths and
			// heights, from here to there, as a denoiser expects.
			float viewZ = dot(surfacePosition - CameraOrigin.xyz, CameraForward.xyz);
			vec3 previousPosition = surfacePosition;
			if (surfaceInstance >= 0)
			{
				vec4 object = vec4(surfaceObject, 1.0);
				int row = surfaceInstance * 3;
				previousPosition = vec3(dot(previousRows[row], object), dot(previousRows[row + 1], object), dot(previousRows[row + 2], object));
			}
			vec2 motion = screenMotion(surfacePosition, previousPosition);

			// The surface seen in a mirror, where it appears to be: as far
			// behind the glass as it really is in front of it, along the line
			// of sight. Its normal is turned the same way. The mirror and what
			// it shows are taken as still, so this point moves on screen only
			// as the camera does.
			float reflectionViewZ = 1.0e7;
			vec2 reflectionMotion = vec2(0.0);
			vec3 virtualNormal = vec3(0.0, 0.0, 1.0);
			if (reflectionFound)
			{
				vec3 toMirror = surfacePosition - CameraOrigin.xyz;
				float mirrorDistance = length(toMirror);
				vec3 virtualPosition = CameraOrigin.xyz + toMirror * ((mirrorDistance + reflectionDistance) / mirrorDistance);
				reflectionViewZ = dot(virtualPosition - CameraOrigin.xyz, CameraForward.xyz);
				reflectionMotion = screenMotion(virtualPosition, virtualPosition);
				virtualNormal = reflect(reflectionNormal, surfaceNormal);
			}

			if ((Disable & 64u) != 0u)
			{
				// Where there is nothing to denoise, NRD is told the pixel is
				// beyond its range, the way it expects the sky to be marked.
				imageStore(guideNormalImage, pixel, packNormalRoughness(surfaceFound ? surfaceNormal : vec3(0.0, 0.0, 1.0), surfaceRoughness));
				imageStore(guideDepthMotionImage, pixel, vec4(surfaceFound ? viewZ : 1.0e7, motion, surfaceFound ? 1.0 : 0.0));
				imageStore(diffuseImage, pixel, vec4(diffuseSignal, diffuseHitDistance));
				imageStore(specularImage, pixel, vec4(reflectionSignal, reflectionHitDistance));
				imageStore(emissionImage, pixel, vec4(emissionPart, 1.0));
				imageStore(diffuseAlbedoImage, pixel, vec4(diffuseAlbedo, 1.0));
				imageStore(specularAlbedoImage, pixel, vec4(reflectionRemodulation, 1.0));
				imageStore(reflectionNormalImage, pixel, packNormalRoughness(virtualNormal, 1.0));
				imageStore(reflectionDepthMotionImage, pixel, vec4(reflectionViewZ, reflectionMotion, reflectionFound ? 1.0 : 0.0));
				// Without materials nothing is glossy, and nothing reads these.
				if ((Disable & 128u) == 0u)
				{
					imageStore(glossImage, pixel, vec4(glossSignal, glossHitDistance));
					imageStore(glossAlbedoImage, pixel, vec4(glossAlbedo, 1.0));
				}
			}

			// Ray Reconstruction takes the picture as it is, noisy, one sample,
			// before any accumulation or tonemapping, and works the lighting out
			// from the colours it is given with it - so there is nothing for this
			// shader to average. Its finish pass tonemaps what comes back and
			// puts the fog and the flash over it.
			if ((Disable & 256u) != 0u)
			{
				// What the picture's colour is made of. Where there is nothing
				// to reflect light - the sky, a surface that only glows - the
				// albedo has to be white rather than black: divided by nothing,
				// the colour turns black, and a sky trails ghosts behind rain.
				// A mirror or a glossy surface's reflection is its specular
				// part, with the mirror's own roughness of nothing.
				vec3 specularGuide = surfaceFound ? specularAlbedo + glossAlbedo : vec3(0.0);
				vec3 diffuseGuide = surfaceFound ? diffuseAlbedo : vec3(0.0);
				if (max(max(diffuseGuide.r + specularGuide.r, diffuseGuide.g + specularGuide.g), diffuseGuide.b + specularGuide.b) < 0.01)
					diffuseGuide = vec3(1.0);
				imageStore(outImage, pixel, vec4(max(radiance, vec3(0.0)) * Params.x, 1.0));
				imageStore(guideNormalImage, pixel, vec4(surfaceFound ? surfaceNormal : -viewDirection, surfaceFound ? surfaceRoughness : 1.0));
				imageStore(diffuseAlbedoImage, pixel, vec4(diffuseGuide, 1.0));
				imageStore(specularAlbedoImage, pixel, vec4(specularGuide, 1.0));
				imageStore(rrDepthImage, pixel, vec4(viewZ));
				imageStore(rrMotionImage, pixel, vec4(motion, 0.0, 0.0));
				imageStore(fogImage, pixel, (Disable & 32u) == 0u && primaryFogged ? volumetricFog(CameraOrigin.xyz, viewDirection, airDistance, dither) : vec4(0.0));
				// In NRD's emission image, which nothing else writes with
				// Ray Reconstruction.
				imageStore(emissionImage, pixel, vec4(beam * Params.x, 1.0));
				return;
			}

			// PT VIEW: one of the parts in place of the picture, averaged the
			// same way. Lighting is shown tonemapped like the picture; the
			// rest as plain values.
			bool plainView = false;
			if (Params.w > 2.5)
			{
				int view = int(Params.w + 0.5);
				plainView = view != 6 && view != 7 && view != 8;
				if (view == 3)
					radiance = surfaceNormal * 0.5 + 0.5;
				else if (view == 4)
					radiance = vec3(clamp(log2(max(viewZ, 1.0)) / 16.0, 0.0, 1.0));
				else if (view == 5)
					radiance = vec3(0.5 + motion * 20.0, 0.5);
				else if (view == 6)
					radiance = diffuseSignal;
				else if (view == 7)
					radiance = specularSignal + glossSignal;
				else if (view == 8)
					radiance = emission;
				else if (view == 9)
					radiance = diffuseAlbedo + specularAlbedo + glossAlbedo;
				else if (view == 10)
					radiance = vec3(clamp(diffuseHitDistance / 2048.0, 0.0, 1.0), clamp(specularHitDistance / 2048.0, 0.0, 1.0), 0.0);
				else if (view == 12)
					radiance = vec3(glossySurface ? surfaceRoughness : 1.0, surfaceMetalness, glossySurface ? 1.0 : 0.0);   // roughness, metalness, glossy
				// 11, the history, is drawn once the average is updated.
			}

			// Average with what has already been traced from this viewpoint.
			// Reset by the device whenever the camera or the scene moves, so a
			// still image converges and a moving one stays responsive.
			// Accumulation is judged per pixel rather than for the whole frame.
			// The device resets everything when the camera moves, but an actor
			// walking past moves nothing the device can see: the instance count
			// does not change and neither does the view, so those pixels went on
			// averaging against the wall behind them. That is what turned a
			// moving character into a fading smear.
			vec4 stored = imageLoad(accumImage, pixel);
			vec4 previousHit = imageLoad(historyImage, pixel);

			// The alpha holds two things: the sample count below 4096, and above
			// it how many more frames this pixel stays on a short history
			// because a moving shadow crossed it. Kept for a while after, so
			// the ground a shadow has just left catches up as fast as the
			// ground it has just reached.
			const float shadowUnit = 4096.0;
			float recentShadow = floor(stored.a / shadowUnit);
			float samples = stored.a - recentShadow * shadowUnit;
			if (photo)
				recentShadow = 0.0;
			else if (primaryMoverShadow)
				recentShadow = 8.0;
			else
				recentShadow = max(recentShadow - 1.0, 0.0);

			if (Counts.w == 0u)
			{
				// The device invalidated everything, typically a camera move.
				samples = 0.0;
			}
			else if (photo)
			{
				// Photo mode: the world is frozen and the view still, so every
				// frame is another sample of the same picture - whatever the
				// jitter, the lens or a flickering light puts under the pixel
				// this time.
			}
			else if (primaryChanged)
			{
				samples = 0.0;
			}
			else if (previousHit.w != primaryInstance)
			{
				// A different object is under this pixel than last frame.
				samples = 0.0;
			}
			else if (primaryInstance >= 0.0 && instanceAmbient[int(primaryInstance)].w < 0.0)
			{
				// The scene says this instance moved or changed shape since the
				// last frame, so nothing accumulated for it still describes it.
				// Exact, where judging by how far the hit point shifted was not:
				// anything slower than the tolerance kept its history and
				// smeared, which is what a medical bot crossing a room does.
				samples = 0.0;
			}
			else
			{
				// The same object, in the same place. The hit point still wanders
				// by about a pixel's footprint because the ray is jittered inside
				// the pixel, which is all this tolerance is for.
				float tolerance = max(0.05, primaryDistance * 0.004);
				if (distance(previousHit.xyz, primaryPosition) > tolerance)
					samples = 0.0;
			}

			// Under a moving shadow, averaged over a few frames rather than a
			// few hundred: enough to settle the noise, short enough that the
			// shadow keeps up with whoever is casting it instead of smearing
			// out behind them.
			if (recentShadow > 0.0)
				samples = min(samples, 3.0);

			vec3 result = radiance;
			if (samples > 0.0)
				result = mix(stored.rgb, radiance, 1.0 / (samples + 1.0));

			samples = min(samples + 1.0, min(float(max(MaxSamples, 1u)), shadowUnit - 1.0));
			imageStore(accumImage, pixel, vec4(result, samples + recentShadow * shadowUnit));
			imageStore(historyImage, pixel, vec4(primaryPosition, primaryInstance));

			// Tonemap and encode here rather than in a present pass: the result
			// is blitted straight to the swap chain, so this is the last thing
			// that happens to the pixel.
			vec3 mapped = toneMap(result * Params.x, (Disable & 8192u) != 0u, ToneCeiling);
			mapped = pow(mapped, vec3(1.0 / 2.2));
			if (Params.w > 2.5)
			{
				// A view of one part: no fog or flash over it. The history view
				// is how many frames each pixel has averaged, white at 64, red
				// where a change in its light is holding it short.
				vec3 shown = plainView ? result : mapped;
				if (int(Params.w + 0.5) == 11)
					shown = recentShadow > 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(min(samples / 64.0, 1.0));
				imageStore(outImage, pixel, vec4(shown, 1.0));
				return;
			}

			// The engine's screen flash - taking damage, being under water -
			// carried in the camera vectors' spare w. The same blend the other
			// devices draw it with, in display terms and after the history, so
			// a flash does not linger in the accumulation: the picture scaled,
			// then the flash colour added.
			// Volumetric fog, blended over the picture the way a fog map is
			// drawn over a surface: the glow added, what it hides taken away.
			// Worked out afresh each frame from the first surface the eye
			// meets, so it never enters the history and follows a moving
			// light or eye at once.
			if ((Disable & 32u) == 0u)
			{
				vec4 fog = primaryFogged ? volumetricFog(CameraOrigin.xyz, viewDirection, airDistance, dither) : vec4(0.0);
				mapped = fog.rgb + mapped * (1.0 - fog.a);
				if ((Disable & 64u) != 0u)
					imageStore(fogImage, pixel, fog);
			}
			else if ((Disable & 64u) != 0u)
			{
				imageStore(fogImage, pixel, vec4(0.0));
			}

			vec3 flashFog = vec3(CameraRight.w, CameraUp.w, CameraForward.w);
			mapped = flashFog + mapped * CameraOrigin.w;

			imageStore(outImage, pixel, vec4(mapped, 1.0));
		}
	)";

	return source;
}

// The pass before the trace that gives each fog light its shadow cube (see
// fogLightSees): for every texel of every face, how far a ray from the light
// goes that way before something stops it, out to the glow's radius. What
// stops a shadow ray stops it: glass and the holes in a grate let the glow
// through. What lies within 8 units of the light does not: a light sunk a
// little way into a wall or a ceiling would see nothing at all, and its glow
// would go, where the engine's glows out regardless. Any further, and a
// light just the other side of a wall would glow through it. It shares the trace's
// bindings, push constants and pipeline layout. One invocation a texel: x and y across the face, z the face and
// the light, six faces to a light.
std::string Shaders::FogShadows()
{
	std::string source = TraceCommon();
	source += R"(
		void main()
		{
			uvec3 id = gl_GlobalInvocationID;
			uint fogIndex = id.z / 6u;
			uint face = id.z - fogIndex * 6u;
			if (id.x >= FogShadowSize || id.y >= FogShadowSize || fogIndex >= min(lightGrid[7], MaxFogShadows))
				return;
			SceneLight light = lights[Counts.y + fogIndex];
			vec2 st = (vec2(id.xy) + 0.5) / float(FogShadowSize) * 2.0 - 1.0;
			vec3 dir = normalize(cubeDirection(face, st));
			float radius = light.PositionRadius.w;
			float start = min(8.0, 0.25 * radius);

			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel, (Disable & 8u) != 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT,
				ShadowRays, light.PositionRadius.xyz, start, dir, radius);
			while (rayQueryProceedEXT(rq))
			{
				if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
				{
					if (confirmCandidate(
							rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false),
							rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
							rayQueryGetIntersectionBarycentricsEXT(rq, false),
							true, dir, mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, false))))
						rayQueryConfirmIntersectionEXT(rq);
				}
			}
			float reach = radius;
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
				reach = rayQueryGetIntersectionTEXT(rq, true);
			fogShadow[(fogIndex * 6u + face) * FogShadowSize * FogShadowSize + id.y * FogShadowSize + id.x] = reach;
		}
	)";
	return source;
}

std::string Shaders::Composite()
{
	return R"(
		#version 460

		layout(local_size_x = 8, local_size_y = 8) in;

		layout(binding = 0, rgba16f) uniform writeonly image2D outImage;
		layout(binding = 1, rgba16f) uniform readonly image2D emissionImage;
		layout(binding = 2, rgba16f) uniform readonly image2D diffuseAlbedoImage;
		layout(binding = 3, rgba16f) uniform readonly image2D specularAlbedoImage;
		layout(binding = 4, rgba16f) uniform readonly image2D diffuseImage;    // denoised, demodulated
		layout(binding = 5, rgba16f) uniform readonly image2D specularImage;   // what mirrors show, denoised
		layout(binding = 6, rgba16f) uniform readonly image2D fogImage;
		layout(binding = 7, rgba16f) uniform readonly image2D glossAlbedoImage;
		layout(binding = 8, rgba16f) uniform readonly image2D glossImage;      // glossy reflection, denoised
		layout(binding = 9, rgba32f) uniform readonly image2D guideDepthMotionImage;   // view z, motion in screens
		layout(binding = 10, r32f) uniform writeonly image2D upscaleDepthImage;
		layout(binding = 11, rg16f) uniform writeonly image2D upscaleMotionImage;

		layout(push_constant) uniform PushConstants
		{
			vec4 Flash;      // x the picture's scale, yzw the flash colour
			vec4 Finish;     // x exposure, y 1 when there are glossy reflections to add, z 1 for the neutral tone curve, w its ceiling (toneMap)
			vec4 Upscale;    // x 1 when FSR takes the picture up to size, y the near plane its depth is written against
		};
	)" + ToneMapGlsl() + R"(

		// The same finish the trace gives its own picture: the lighting put
		// back on its surfaces, tonemapped, then the fog and the screen flash
		// over it.
		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			ivec2 size = imageSize(outImage);
			if (pixel.x >= size.x || pixel.y >= size.y)
				return;

			vec3 result = imageLoad(emissionImage, pixel).rgb
				+ imageLoad(diffuseAlbedoImage, pixel).rgb * max(imageLoad(diffuseImage, pixel).rgb, vec3(0.0))
				+ imageLoad(specularAlbedoImage, pixel).rgb * max(imageLoad(specularImage, pixel).rgb, vec3(0.0));
			if (Finish.y > 0.5)
				result += imageLoad(glossAlbedoImage, pixel).rgb * max(imageLoad(glossImage, pixel).rgb, vec3(0.0));

			vec3 mapped = toneMap(result * Finish.x, Finish.z > 0.5, Finish.w);
			mapped = pow(mapped, vec3(1.0 / 2.2));

			// A tent over the pixel and its neighbours: the fog's shadows
			// are marched in steps each pixel offsets differently (the
			// trace's dither), which this smooths.
			vec4 fog = vec4(0.0);
			for (int y = -1; y <= 1; y++)
				for (int x = -1; x <= 1; x++)
					fog += imageLoad(fogImage, clamp(pixel + ivec2(x, y), ivec2(0), size - 1)) * float((2 - abs(x)) * (2 - abs(y)));
			fog /= 16.0;
			mapped = fog.rgb + mapped * (1.0 - fog.a);

			mapped = Flash.yzw + mapped * Flash.x;
			imageStore(outImage, pixel, vec4(mapped, 1.0));

			// For FSR, beside the picture: the depth as near over the view's
			// z, so the nearer the larger and nothing beyond reach of it -
			// inverted and infinite - and the motion as the trace has it.
			if (Upscale.x > 0.5)
			{
				vec4 depthMotion = imageLoad(guideDepthMotionImage, pixel);
				imageStore(upscaleDepthImage, pixel, vec4(clamp(Upscale.y / max(depthMotion.x, Upscale.y), 0.0, 1.0)));
				imageStore(upscaleMotionImage, pixel, vec4(depthMotion.yz, 0.0, 0.0));
			}
		}
	)";
}

// After Ray Reconstruction, at the output's size: what it returns is the
// picture, denoised and upscaled but still linear, so it is tonemapped the
// way the trace tonemaps its own, and the fog and the screen flash go over
// it, with the flashlight's beam added first. Both were traced at the render
// size, and are smooth enough to be filtered up to this one.
std::string Shaders::Finish()
{
	return R"(
		#version 460

		layout(local_size_x = 8, local_size_y = 8) in;

		layout(binding = 0, rgba16f) uniform writeonly image2D outImage;
		layout(binding = 1, rgba16f) uniform readonly image2D reconstructedImage;
		layout(binding = 2) uniform sampler2D fogTexture;
		// The flashlight's beam, at the exposure, traced at the render size
		// as the fog is.
		layout(binding = 3) uniform sampler2D beamTexture;

		layout(push_constant) uniform PushConstants
		{
			vec4 Flash;      // x the picture's scale, yzw the flash colour
			vec4 Mode;       // x 1 for the neutral tone curve, y its ceiling (toneMap)
		};
	)" + ToneMapGlsl() + R"(

		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			ivec2 size = imageSize(outImage);
			if (pixel.x >= size.x || pixel.y >= size.y)
				return;

			vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
			vec3 mapped = toneMap(imageLoad(reconstructedImage, pixel).rgb + texture(beamTexture, uv).rgb, Mode.x > 0.5, Mode.y);
			mapped = pow(mapped, vec3(1.0 / 2.2));

			// Four filtered reads half a traced pixel apart, which make a
			// tent over the traced pixels around this one: the fog's
			// shadows are marched in steps each pixel offsets differently
			// (the trace's dither), which this smooths.
			vec2 d = 0.5 / vec2(textureSize(fogTexture, 0));
			vec4 fog = 0.25 * (texture(fogTexture, uv + vec2(-d.x, -d.y)) + texture(fogTexture, uv + vec2(d.x, -d.y)) +
				texture(fogTexture, uv + vec2(-d.x, d.y)) + texture(fogTexture, uv + vec2(d.x, d.y)));
			mapped = fog.rgb + mapped * (1.0 - fog.a);

			mapped = Flash.yzw + mapped * Flash.x;
			imageStore(outImage, pixel, vec4(mapped, 1.0));
		}
	)";
}

std::string Shaders::TileVertex()
{
	return R"(
		#version 460

		layout(location = 0) in vec2 aPosition;   // already in normalised device coordinates
		layout(location = 1) in vec2 aTexCoord;
		layout(location = 2) in vec4 aColor;

		layout(location = 0) out vec2 vTexCoord;
		layout(location = 1) out vec4 vColor;

		void main()
		{
			vTexCoord = aTexCoord;
			vColor = aColor;
			gl_Position = vec4(aPosition, 0.0, 1.0);
		}
	)";
}

std::string Shaders::TileFragment()
{
	return R"(
		#version 460

		layout(binding = 0) uniform sampler2D texSampler;

		layout(location = 0) in vec2 vTexCoord;
		layout(location = 1) in vec4 vColor;
		layout(location = 0) out vec4 outColor;

		void main()
		{
			vec4 texel = texture(texSampler, vTexCoord);

			// The traced image is tonemapped and gamma encoded by the time the
			// tiles land on it, so this pass works in display space too and the
			// engine's colours can be used as they are given.
			outColor = texel * vColor;

			// Masked art arrives with a zero alpha hole; anything that survives
			// a blend at all should not be written where the hole is.
			if (outColor.a < 0.01)
				discard;
		}
	)";
}

// A modulated tile drawn into the HUD's image for the headset, whose world
// is put under the HUD by the headset's compositor rather than drawn first.
// The image holds colour premultiplied by how much it covers, and the
// compositor shows colour + (1 - alpha) * world. Modulating by m (2x the
// tile, as on the screen) takes both to m times themselves: the colour
// blended by the tile's own colour, the coverage as 1 - m + m * alpha, which
// with the tile's alpha 1 - m and the blend "over" is what alpha comes to.
// Only darkening can be carried that way: m is held to 1, which a modulated
// tile's neutral grey is.
std::string Shaders::TileFragmentModulatedHud()
{
	return R"(
		#version 460

		layout(binding = 0) uniform sampler2D texSampler;

		layout(location = 0) in vec2 vTexCoord;
		layout(location = 1) in vec4 vColor;
		layout(location = 0) out vec4 outColor;

		void main()
		{
			vec4 tile = texture(texSampler, vTexCoord) * vColor;
			if (tile.a < 0.01)
				discard;
			vec3 m = min(2.0 * tile.rgb, vec3(1.0));
			outColor = vec4(m, 1.0 - (m.r + m.g + m.b) / 3.0);
		}
	)";
}

std::string Shaders::Brightness()
{
	return R"(
		#version 460

		layout(local_size_x = 8, local_size_y = 8) in;
		layout(binding = 0, rgba16f) uniform image2D picture;
		layout(push_constant) uniform BrightnessConstants
		{
			vec4 InvGamma;  // x the exponent
			ivec4 Size;     // xy the picture's size
		};

		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			if (pixel.x >= Size.x || pixel.y >= Size.y)
				return;
			vec4 c = imageLoad(picture, pixel);
			imageStore(picture, pixel, vec4(pow(max(c.rgb, vec3(0.0)), vec3(InvGamma.x)), c.a));
		}
	)";
}

// The finished picture - the trace, the fog and flash, the HUD and menus -
// encoded for an HDR display, or brought back to SDR for a picture saved.
// The picture stays in the display's terms throughout, as it is for SDR:
// gamma encoded, 1 the white the SDR picture and the HUD are drawn at, and
// above that only what the tone curve's ceiling let through (toneMap). So
// everything drawn over it blends exactly as it does in SDR.
//
// scRGB takes linear light with 1 meaning 80 nits, and the compositor does
// the rest. HDR10 is ours to encode: Rec.2020 primaries and the ST.2084 (PQ)
// curve, in which a code value is a luminance. A saved picture - a photo, a
// save game's, a screenshot - has the HDR shoulder undone and the SDR one put
// back, so it comes out as it would have without HDR.
std::string Shaders::Encode()
{
	return R"(
		#version 460

		layout(local_size_x = 8, local_size_y = 8) in;
		layout(binding = 0, rgba16f) uniform readonly image2D picture;
		layout(binding = 1, rgba16f) uniform writeonly image2D encoded;
		layout(push_constant) uniform EncodeConstants
		{
			vec4 Params;    // x 1 scRGB, 2 HDR10, 3 SDR; y the SDR white in nits; z the tone curve's ceiling
			ivec4 Size;     // xy the picture's size
		};

		// Rec.709 to Rec.2020. Both are D65, so it is a pure rotation of the
		// gamut; left out, the display reads narrow gamut numbers as wide gamut
		// ones and every colour comes back oversaturated.
		vec3 rec709ToRec2020(vec3 c)
		{
			const mat3 M = mat3(
				0.6274040, 0.0690970, 0.0163916,
				0.3292820, 0.9195400, 0.0880132,
				0.0433136, 0.0113612, 0.8955950);
			return M * c;
		}

		// ST.2084's inverse EOTF: luminance, 1 being PQ's 10000 nits, to the
		// code value the display decodes.
		vec3 pqEncode(vec3 L)
		{
			const float m1 = 0.1593017578125;
			const float m2 = 78.84375;
			const float c1 = 0.8359375;
			const float c2 = 18.8515625;
			const float c3 = 18.6875;
			vec3 y = pow(clamp(L, 0.0, 1.0), vec3(m1));
			return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
		}

		// toneMap's neutral curve with its ceiling at top: what a peak of p
		// comes out as, and how far it is drawn towards white.
		const float shoulder = 0.8;
		float shoulderPeak(float p, float top)
		{
			float d = top - shoulder;
			return top - d * d / (p + d - shoulder);
		}
		float whitening(float p, float newPeak)
		{
			return 1.0 - 1.0 / (0.15 * (p - newPeak) + 1.0);
		}

		// A picture made with the neutral curve at ceiling, as the SDR one
		// would have made it: linear, in the SDR white. HDR always takes the
		// neutral curve (toneMap).
		vec3 toSdr(vec3 c, float ceiling)
		{
			float m = max(c.r, max(c.g, c.b));
			if (m < shoulder)
				return c;
			// The scene's peak that came out as m, and what the SDR shoulder
			// makes of it. The HDR curve drew the colour towards white as far
			// as its own shoulder asked; the rest of the SDR one's is added.
			float d = ceiling - shoulder;
			float p = d * d / max(ceiling - min(m, ceiling - 1.0e-4), 1.0e-4) - d + shoulder;
			float sdrPeak = shoulderPeak(p, 1.0);
			float hdrWhite = whitening(p, m);
			float sdrWhite = whitening(p, sdrPeak);
			c *= sdrPeak / m;
			float more = 1.0 - (1.0 - sdrWhite) / max(1.0 - hdrWhite, 1.0e-4);
			return mix(c, vec3(sdrPeak), clamp(more, 0.0, 1.0));
		}

		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			if (pixel.x >= Size.x || pixel.y >= Size.y)
				return;
			vec3 shown = max(imageLoad(picture, pixel).rgb, vec3(0.0));
			vec3 linear = pow(shown, vec3(2.2));
			vec3 result;
			if (Params.x < 1.5)
				result = linear * (Params.y / 80.0);
			else if (Params.x < 2.5)
				result = pqEncode(max(rec709ToRec2020(linear * Params.y), vec3(0.0)) / 10000.0);
			else
				result = pow(clamp(toSdr(linear, max(Params.z, 1.0)), 0.0, 1.0), vec3(1.0 / 2.2));
			imageStore(encoded, pixel, vec4(result, 1.0));
		}
	)";
}

// What the headset is given: the eyes' pictures and the HUD, both gamma
// encoded as the screen takes them, in linear light for the headset's
// swap chains, which encode them again as their format asks. The Brightness
// the device puts over the screen's picture goes on here, before.
//
// The HUD is premultiplied: colour already multiplied by how much of what is
// behind it it covers. Its gamma comes off the colour as it would be alone,
// then the coverage goes back on. What is added over and above that - the
// translucent tiles, which cover nothing - comes off on its own.
std::string Shaders::HeadsetEncode()
{
	return R"(
		#version 460

		layout(local_size_x = 8, local_size_y = 8) in;
		layout(binding = 0, rgba16f) uniform readonly image2D picture;
		layout(binding = 1, rgba16f) uniform writeonly image2D linear;
		layout(push_constant) uniform HeadsetConstants
		{
			vec4 Params;    // x the Brightness's exponent, y 1 for the HUD
			ivec4 Size;     // xy the size to write
		};

		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			if (pixel.x >= Size.x || pixel.y >= Size.y)
				return;
			vec4 c = max(imageLoad(picture, pixel), vec4(0.0));
			float g = Params.x * 2.2;
			if (Params.y < 0.5)
			{
				imageStore(linear, pixel, vec4(pow(c.rgb, vec3(g)), 1.0));
				return;
			}
			float a = min(c.a, 1.0);
			vec3 covered = min(c.rgb, vec3(a));
			vec3 added = c.rgb - covered;
			vec3 result = pow(added, vec3(g));
			if (a > 1.0e-4)
				result += a * pow(covered / a, vec3(g));
			imageStore(linear, pixel, vec4(result, a));
		}
	)";
}
