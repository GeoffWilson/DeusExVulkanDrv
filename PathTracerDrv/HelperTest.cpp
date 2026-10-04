// PathTracerHelperTest.exe: drives PathTracerHelper.exe with a made up scene,
// the way the render device does, and writes what comes back to a file.
//
// 32-bit, like the game, so the whole road is the one the game takes: a
// 32-bit Vulkan device here, the helper on the same GPU, the scene over the
// shared memory, and each frame taken over through the shared image and
// semaphores. What it cannot check is the engine's end - LevelScene and the
// textures - which only the game can.
//
//   PathTracerHelperTest.exe [frames] [width] [height] [--dlss quality] [--fsr quality] [--still] [--pan d] [--pan-angle a]
//                            [--lightsize radius] [--reference] [--backlight]
//                            [--fog] [--flashlight] [--dark] [--glow] [--glow-unsampled]
//                            [--photo aperture] [--glass] [--wet percent] [--view n]
//                            [--decal] [--decal-lift units]
//                            [--bump percent] [--lights n] [--every-light]
//                            [--hdr ceiling] [--neutral] [--sprites n] [--headset] [--headset-real]
//   (helper beside it)
//
// --dlss denoises with DLSS Ray Reconstruction at that quality (0 DLAA to
// 4 ultra performance) rather than NRD, where it can run. --fsr denoises
// with NRD at the render size that quality gives and upscales with AMD's
// FSR 3.1, on any GPU.
//
// --lights crowds the scene with n more lights, each reaching all of it, so
// every cell of the light grid holds them all, and the trace draws from a
// cell's lights rather than weighing each; --every-light weighs each, as PT
// ALLLIGHTS does. With --reference the two should come to the same picture.
//
// --hdr finishes the picture for an HDR display whose peak is that many times
// the SDR white (the device's HDRPeakNits over HDRPaperWhite), and says how
// much of it went above that white; everything below the tone curve's
// shoulder should come out as it does without. HDR takes the device's
// default neutral curve, which --neutral gives SDR too, to compare them;
// without it the harness's SDR is Reinhard's, as it has always been.
//
// --sprites hangs a plume of n puffs of smoke between the camera and the
// box, as the device places sprites: square on to the view, drawn by adding
// a texture that fades to black at its edges, each overlapping the next.
// Their edges should never show, however many overlap: the picture behind
// them should come out as it does without them, with the smoke added.
//
// --headset traces for the helper's simulated headset instead of the screen:
// two eyes 64 mm apart, each seeing further out than in, the head turning
// slowly one way and back. The picture written is the two eyes side by side,
// which the helper sends back in the headset's place, each a little to its
// own side of the other, the box nearer the middle in each than the wall.
// A HUD goes the other way every frame, as the device draws one - a
// crosshair and a translucent bar - which the helper puts on the simulated
// headset's panel. --headset-real does the same with the OpenXR runtime's
// headset rather than a simulated one; with 0 frames it only says what the
// helper found.
//
// --still holds everything still instead - no animation, nothing arriving, no
// change of size - and reports how much the picture changes from one frame
// to the next over the last quarter of them: with nothing moving, what is
// left is noise and shimmer, which is what a wrong jitter shows up as.
//
// --pan d holds the scene still but takes the eye round the box, d degrees
// a frame (from --pan-angle a, which with --reference gives the picture a pan
// should end on): what an upscaler makes of a moving camera, which --still
// cannot show - its depth and motion going missing look fine held still.
//
// --lightsize casts shadows from a disc of that radius around the light
// rather than from its centre (the game's LightSize). --reference holds the
// scene still and denoises nothing, so the frames average towards what the
// trace converges on - the picture a denoiser is trying to reach.
// --backlight puts the light behind the red box, so its shadow falls towards
// the camera, from where the box stands to well beyond it.
//
// --fog adds a fog light to the left of the box, whose glow the box should
// cut a shadow through towards the right; --fog-unshadowed the same glow as
// the engine draws it, through the box (PT FOGSHADOWS). --flashlight
// shines the light augmentation's torch from beside the eye at the box, its
// beam in the air as in a fog zone - eight times as bright as the player's,
// the camera being so far off - and --dark turns the scene's light down to a
// fiftieth and the sky off to show it.
//
// --glow puts a glowing cyan strip low on the wall, sampled as a light as the
// level's glowing surfaces are; --glow-unsampled the same strip found only by
// the bounces that reach it. With --reference the two should come to the
// same picture, the sampled one with less noise on the way.
//
// --glass hangs a pane of red glass under the light, which should throw a
// red patch on the floor rather than a shadow (glassTransmittance); --wet
// makes the scene ground open to the sky and that wet, darker and shining
// with puddles on the floor - but for the patch under the glass, which the
// rain does not reach.
//
// --bump gives the checkered floor stone's relief (Materials.h), drawn at
// that percent: the light squares standing a few units proud of the dark,
// lit along their edges on the side facing the light.
//
// --view shows one part of the picture in its place, numbered as PT VIEW
// numbers them: 12 is each surface's material, red its roughness.
//
// --photo refines the picture as photo mode does (the device's PT PHOTO):
// --reference, with every frame taken as another sample whatever changes
// under a pixel, and a lens of that radius focused - as photo mode focuses by
// default - on what the middle of the view meets, which is the box, so the
// floor before it and the wall behind it blur and the box stays sharp. 0 is
// a pinhole.
//
// Frames are taken the way the render device takes them: the next is asked
// for before the last is waited for, so the helper records one while the GPU
// traces the other. Along the way the scene does what a level does to the
// helper's buffers: a shape animates every frame and later outgrows its
// buffer, a large shape arrives and grows the shading data, and three hundred
// more instances and lights arrive at once. Halfway through, the size goes up
// by half, as a change of resolution does, so the helper makes a new shared
// image and this side takes it up.
//
// Writes helper-test.ppm: a floor with a glossy checkerboard, a red box and a
// wall, lit by one light. Exits 0 when every frame arrived and the picture is
// lit.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "TraceClient.h"
#include "EmitterGrid.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	printf("[%s] %s\n", typestr, msg.c_str());
}

void VulkanError(const char* text)
{
	throw std::runtime_error(text);
}

#ifdef PATHTRACER_LOCAL
// Built with the tracing in it (PathTracerLocalTest), as a 64-bit game's
// device is: its log comes here.
#include "RayReconstruction.h"
#include <cstdarg>
void HelperLog(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("  tracer: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
}
#endif

static float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
	float value;
	if (exponent == 0)
		value = std::ldexp((float)mantissa, -24);
	else if (exponent == 31)
		value = mantissa ? NAN : INFINITY;
	else
		value = std::ldexp((float)(mantissa | 1024), (int)exponent - 25);
	return sign ? -value : value;
}

static vec3 Cross(const vec3& a, const vec3& b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static vec3 Normalized(const vec3& v) { float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return vec3(v.x / l, v.y / l, v.z / l); }

// A quad as two triangles, textured or not, into the geometry. Given a centre,
// each corner's normal points away from it, the way the engine smooths a
// mesh's normals: the path a mesh takes rather than a level's flat one.
static void AddQuad(SceneGeometry& g, vec3 a, vec3 b, vec3 c, vec3 d, vec3 albedo, int texture, float uvScale, const vec3* centre = nullptr)
{
	const vec3 normal = Normalized(Cross(vec3(b.x - a.x, b.y - a.y, b.z - a.z), vec3(c.x - a.x, c.y - a.y, c.z - a.z)));
	const vec3 corners[2][3] = { { a, b, c }, { a, c, d } };
	const float uvs[2][6] = { { 0, 0, uvScale, 0, uvScale, uvScale }, { 0, 0, uvScale, uvScale, 0, uvScale } };
	for (int t = 0; t < 2; t++)
	{
		for (int v = 0; v < 3; v++)
			g.Positions.push_back(corners[t][v]);
		TriangleAttributes attr = {};
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, 0.0f);
		attr.Ambient = vec4(0.03f, 0.03f, 0.04f, 0.0f);
		attr.UV01 = vec4(uvs[t][0], uvs[t][1], uvs[t][2], uvs[t][3]);
		attr.UV2Tex = vec4(uvs[t][4], uvs[t][5], (float)texture, 0.0f);
		if (centre)
		{
			vec3 normals[3];
			for (int v = 0; v < 3; v++)
				normals[v] = Normalized(vec3(corners[t][v].x - centre->x, corners[t][v].y - centre->y, corners[t][v].z - centre->z));
			SetCornerNormals(attr, corners[t], normals);
		}
		SetUvDensity(attr, corners[t]);
		g.Attributes.push_back(attr);
	}
}

static void AddBox(SceneGeometry& g, vec3 lo, vec3 hi, vec3 albedo, bool smooth = false)
{
	vec3 p[8];
	for (int i = 0; i < 8; i++)
		p[i] = vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
	const vec3 centre((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
	const vec3* c = smooth ? &centre : nullptr;
	AddQuad(g, p[4], p[5], p[7], p[6], albedo, -1, 1, c);   // top
	AddQuad(g, p[0], p[2], p[3], p[1], albedo, -1, 1, c);   // bottom
	AddQuad(g, p[0], p[1], p[5], p[4], albedo, -1, 1, c);   // -y
	AddQuad(g, p[2], p[6], p[7], p[3], albedo, -1, 1, c);   // +y
	AddQuad(g, p[0], p[4], p[6], p[2], albedo, -1, 1, c);   // -x
	AddQuad(g, p[1], p[3], p[7], p[5], albedo, -1, 1, c);   // +x
}

int main(int argc, char** argv)
{
	std::vector<const char*> args;
	int dlss = -1;
	int fsr = -1;
	bool still = false, reference = false, backlight = false, detail = false, bc1 = false;
	uint32_t lightSize = 0;
	uint32_t engineLighting = 0;
	bool bakedMask = false;
	bool inset = false;
	bool fog = false, flashlight = false, dark = false, fogUnshadowed = false, glowStrip = false, glowUnsampled = false;
	float photoAperture = -1.0f;
	bool glass = false;
	bool decal = false;
	float decalLift = 0.25f;
	int wetness = 0;
	uint32_t view = 0;
	int bump = 0;
	int crowd = 0;
	bool everyLight = false;
	float toneCeiling = 1.0f;
	bool neutral = false;
	int sprites = 0;
	bool headset = false, realHeadset = false;
	float panStep = 0.0f, panAngle = 0.0f;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--dlss") && i + 1 < argc)
			dlss = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fsr") && i + 1 < argc)
			fsr = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--still"))
			still = true;
		else if (!strcmp(argv[i], "--pan") && i + 1 < argc)
		{
			panStep = (float)atof(argv[++i]);
			still = true;
		}
		else if (!strcmp(argv[i], "--pan-angle") && i + 1 < argc)
			panAngle = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--lightsize") && i + 1 < argc)
			lightSize = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--reference"))
			reference = still = true;
		else if (!strcmp(argv[i], "--backlight"))
			backlight = true;
		else if (!strcmp(argv[i], "--detail"))
			detail = true;
		else if (!strcmp(argv[i], "--bc1"))
			bc1 = true;
		else if (!strcmp(argv[i], "--engine-lighting"))
			engineLighting = 1;
		else if (!strcmp(argv[i], "--baked-mask"))
			bakedMask = true;
		else if (!strcmp(argv[i], "--inset"))
			inset = true;
		else if (!strcmp(argv[i], "--fog"))
			fog = true;
		else if (!strcmp(argv[i], "--fog-unshadowed"))
			fog = fogUnshadowed = true;
		else if (!strcmp(argv[i], "--flashlight"))
			flashlight = true;
		else if (!strcmp(argv[i], "--dark"))
			dark = true;
		else if (!strcmp(argv[i], "--glow"))
			glowStrip = true;
		else if (!strcmp(argv[i], "--glow-unsampled"))
			glowStrip = glowUnsampled = true;
		else if (!strcmp(argv[i], "--glass"))
			glass = true;
		else if (!strcmp(argv[i], "--decal"))
			decal = true;
		else if (!strcmp(argv[i], "--decal-lift") && i + 1 < argc)
			decalLift = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--wet") && i + 1 < argc)
			wetness = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--view") && i + 1 < argc)
			view = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bump") && i + 1 < argc)
			bump = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--lights") && i + 1 < argc)
			crowd = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--every-light"))
			everyLight = true;
		else if (!strcmp(argv[i], "--hdr") && i + 1 < argc)
			toneCeiling = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--neutral"))
			neutral = true;
		else if (!strcmp(argv[i], "--sprites") && i + 1 < argc)
			sprites = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--headset"))
			headset = true;
		else if (!strcmp(argv[i], "--headset-real"))
			headset = realHeadset = true;
		else if (!strcmp(argv[i], "--photo") && i + 1 < argc)
		{
			photoAperture = (float)atof(argv[++i]);
			reference = still = true;
		}
		else
			args.push_back(argv[i]);
	}
	const int frames = args.size() > 0 ? atoi(args[0]) : 30;
	const uint32_t width = args.size() > 1 ? (uint32_t)atoi(args[1]) : 320;
	const uint32_t height = args.size() > 2 ? (uint32_t)atoi(args[2]) : 240;
	setvbuf(stdout, nullptr, _IONBF, 0);
	printf("pointer size: %d bits\n", (int)(sizeof(void*) * 8));

	try
	{
#ifdef PATHTRACER_LOCAL
		// A device that traces itself, as a 64-bit game's render device makes.
		std::vector<std::string> ngxInstance, ngxDevice;
		RayReconstruction::RequiredExtensions(ngxInstance, ngxDevice);
		VulkanInstanceBuilder instanceBuilder;
		for (const std::string& name : ngxInstance)
			instanceBuilder.OptionalExtension(name);
		auto instance = instanceBuilder.Create();
		VulkanDeviceBuilder builder;
		builder.OptionalRayQuery();
		builder.OptionalDescriptorIndexing();
		for (const std::string& name : ngxDevice)
			builder.OptionalExtension(name);
#else
		auto instance = VulkanInstanceBuilder().Create();
		VulkanDeviceBuilder builder;
		builder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		builder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#endif
		auto device = builder.Create(instance);
		printf("this side: %s\n", device->PhysicalDevice.Properties.Properties.deviceName);

		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string dir = path;
		dir = dir.substr(0, dir.find_last_of('\\'));

		TraceClient client;
#ifdef PATHTRACER_LOCAL
		if (!client.StartLocal(device.get()))
#else
		if (!client.Start(device.get(), dir + "\\PathTracerHelper.exe", dir, false, realHeadset ? 1 : headset ? 2 : 0))
#endif
		{
			printf("FAIL %s\n", client.Error().c_str());
			return 1;
		}
		printf("helper: %s, ray tracing %s, textures %s\n", client.Status().DeviceName,
			client.Status().RayTracing ? "yes" : "no", client.Status().CanSampleTextures ? "yes" : "no");
		if (headset)
			printf("headset: state %u, %s\n", client.Status().Headset, client.Status().HeadsetStatus);

		// The scene: a checkered glossy floor, a red box, a grey wall.
		SceneGeometry world;
		AddQuad(world, vec3(-600, -600, 0), vec3(600, -600, 0), vec3(600, 600, 0), vec3(-600, 600, 0), vec3(0.8f, 0.8f, 0.8f), 0, 8);
		AddBox(world, vec3(-80, -80, 0), vec3(80, 80, 160), vec3(0.8f, 0.15f, 0.1f), true);
		AddQuad(world, vec3(-600, 400, 0), vec3(600, 400, 0), vec3(600, 400, 600), vec3(-600, 400, 600), vec3(0.6f, 0.6f, 0.6f), -1, 1);
		// With --fog, a row of posts between the fog light and the box, for
		// the glow to fall through in shafts.
		// The floor, the wall and the posts are then in a fog zone, where the
		// engine lays fog over what it draws (Ambient.w's 2).
		if (fog)
		{
			for (int k = 0; k < 7; k++)
				AddBox(world, vec3(-170, -260.0f + k * 80.0f, 0), vec3(-155, -245.0f + k * 80.0f, 320), vec3(0.5f, 0.5f, 0.5f));
			for (TriangleAttributes& attr : world.Attributes)
				attr.Ambient.w += 2.0f;
		}
		// The glowing strip, as the device describes a level's glowing
		// triangle: unlit (Emission.w), numbered in Emission.z, and listed
		// with its edges crossing along the face it glows from.
		std::vector<EmitterSource> glowSources;
		if (glowStrip)
		{
			const size_t first = world.Attributes.size();
			AddQuad(world, vec3(-420, 398, 20), vec3(-220, 398, 20), vec3(-220, 398, 70), vec3(-420, 398, 70), vec3(0.1f, 0.9f, 1.0f), -1, 1);
			for (size_t t = first; t < world.Attributes.size(); t++)
			{
				TriangleAttributes& attr = world.Attributes[t];
				attr.Emission.w = 1.0f;
				EmitterSource e;
				e.V0 = world.Positions[t * 3];
				e.E1 = world.Positions[t * 3 + 1] - e.V0;
				e.E2 = world.Positions[t * 3 + 2] - e.V0;
				const vec3 c = cross(e.E1, e.E2);
				e.Power = (0.2126f * 0.1f + 0.7152f * 0.9f + 0.0722f * 1.0f) * 0.5f * std::sqrt(dot(c, c));
				e.Geometry = 0;
				e.Primitive = (uint32_t)t;
				glowSources.push_back(e);
				attr.Emission.z = (float)glowSources.size();
			}
		}
		// Ground open to the sky, as a level's zone with a window onto the
		// sky zone marks it (Ambient.w's 4).
		if (wetness > 0)
			for (TriangleAttributes& attr : world.Attributes)
				attr.Ambient.w += 4.0f;
		// The red pane under the light: translucent (UV2Tex.w 2), so the
		// world is no longer opaque throughout and its triangles are offered
		// to the shader to pass or stop.
		if (glass)
		{
			const size_t first = world.Attributes.size();
			AddQuad(world, vec3(-260, -310, 200), vec3(-140, -310, 200), vec3(-140, -190, 200), vec3(-260, -190, 200), vec3(0.8f, 0.1f, 0.1f), -1, 1);
			for (size_t t = first; t < world.Attributes.size(); t++)
				world.Attributes[t].UV2Tex.w = 2.0f;
			world.HasMasked = true;
		}
		// A scorch mark, as the device lays a decal: a modulated quad
		// (UV2Tex.w 4) a quarter of a unit off the floor, mid grey - which
		// modulate-2x leaves alone - but for a dark blot in the middle. Only
		// the blot should show; the square it is drawn on should not.
		if (decal)
		{
			const size_t first = world.Attributes.size();
			// As UT attaches one: to each of the surfaces it lies across, the
			// same quad each time.
			for (int copy = 0; copy < 3; copy++)
				AddQuad(world, vec3(80, -420, decalLift), vec3(280, -420, decalLift), vec3(280, -220, decalLift), vec3(80, -220, decalLift), vec3(0.4f, 0.4f, 0.4f), 2, 1);
			for (size_t t = first; t < world.Attributes.size(); t++)
			{
				world.Attributes[t].UV2Tex.w = 4.0f;
				world.Attributes[t].Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);
			}
			world.HasMasked = true;
		}
		std::vector<uint32_t> emitterWords;
		EmitterGrid::Build(glowSources, emitterWords);
		std::vector<uint32_t> checker(64 * 64);
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++)
				checker[y * 64 + x] = (((x / 8) ^ (y / 8)) & 1) ? 0xffe0e0e0u : 0xff303030u;
		// Its mips, each the average of four texels of the one above, sent
		// after it as the device sends a texture's: the far floor should
		// settle to grey rather than sparkle.
		uint32_t checkerLevels = 1;
		for (int size = 64, above = 0; size > 1; size /= 2, checkerLevels++)
		{
			const int half = size / 2;
			const size_t start = checker.size();
			checker.resize(start + (size_t)half * half);
			for (int y = 0; y < half; y++)
				for (int x = 0; x < half; x++)
				{
					uint32_t sum[4] = {};
					for (int k = 0; k < 4; k++)
					{
						const uint32_t c = checker[above + (y * 2 + k / 2) * size + x * 2 + k % 2];
						for (int b = 0; b < 4; b++)
							sum[b] += (c >> (b * 8)) & 255u;
					}
					uint32_t c = 0;
					for (int b = 0; b < 4; b++)
						c |= ((sum[b] + 2) / 4) << (b * 8);
					checker[start + (size_t)y * half + x] = c;
				}
			above = (int)start;
		}

		SceneInstance placed = {};
		placed.GeometryIndex = 0;
		placed.Transform[0] = placed.Transform[5] = placed.Transform[10] = 1.0f;
		std::vector<SceneInstance> instances = { placed };

		// Geometry 1 animates: a green block that bobs, rebuilt every frame,
		// and later a stack of forty that outgrows the buffer it was given.
		auto animated = [](int frame, bool stack)
		{
			SceneGeometry g;
			g.Dynamic = true;
			g.Version = (uint32_t)frame + 1;
			const float bob = 40.0f * std::sin(frame * 0.3f);
			const int count = stack ? 40 : 1;
			for (int b = 0; b < count; b++)
			{
				const float z = b * 12.0f;
				AddBox(g, vec3(220, -60, z), vec3(300, 20, z + (stack ? 10.0f : 120.0f + bob)), vec3(0.1f, 0.7f, 0.2f));
			}
			return g;
		};
		instances.push_back(placed);
		instances.back().GeometryIndex = 1;

		// Geometry 3 arrives later: a blue panel of 5000 triangles in front of
		// the wall, more shading data than the helper's first buffer holds.
		SceneGeometry panel;
		for (int y = 0; y < 50; y++)
			for (int x = 0; x < 50; x++)
			{
				const float x0 = -560.0f + x * 6.0f, z0 = 20.0f + y * 6.0f;
				AddQuad(panel, vec3(x0, 395, z0), vec3(x0 + 6, 395, z0), vec3(x0 + 6, 395, z0 + 6), vec3(x0, 395, z0 + 6), vec3(0.15f, 0.25f, 0.8f), -1, 1);
			}

		// Geometry 2 is a small cube, placed three hundred times at once
		// around the edge of the floor, with a dim light over each. Both arrive
		// after geometry 1, so its growing later has to move its shading data
		// rather than write over theirs.
		SceneGeometry cube;
		AddBox(cube, vec3(-6, -6, 0), vec3(6, 6, 12), vec3(0.9f, 0.8f, 0.2f));
		std::vector<SceneInstance> ring;
		std::vector<SceneLight> ringLights;
		for (int k = 0; k < 300; k++)
		{
			const float angle = k * (6.2831853f / 300.0f);
			SceneInstance c = placed;
			c.GeometryIndex = 2;
			c.Transform[3] = 520.0f * std::cos(angle);
			c.Transform[7] = 520.0f * std::sin(angle);
			ring.push_back(c);
			SceneLight l = {};
			l.PositionRadius = vec4(c.Transform[3], c.Transform[7], 40, 120);
			l.ColorBrightness = vec4(1.0f, 0.7f, 0.3f, 0.05f);
			l.DirectionCone = vec4(0, 0, 0, -1);
			l.Flags = vec4(0, 0, 0, -1);
			ringLights.push_back(l);
		}

		SceneLight light = {};
		light.PositionRadius = backlight ? vec4(40, 260, 300, 1400) : vec4(-200, -250, 350, 1400);
		light.ColorBrightness = vec4(1.0f, 0.9f, 0.8f, 2.5f);
		light.DirectionCone = vec4(0, 0, 0, -1);
		light.Flags = vec4(0, 0, 0, -1);
		if (dark)
			light.ColorBrightness.w *= 0.02f;
		std::vector<SceneLight> lights = { light };
		// --lights n: n more round the box, each reaching the whole scene,
		// sharing the main light's brightness - a cell as crowded as the Wan
		// Chai canal's, whose lights the trace draws from rather than weighs
		// in full. Their brightnesses fall away one after another, as a real
		// crowd's do, so the ranking has something to rank.
		for (int k = 0; k < crowd; k++)
		{
			SceneLight l = light;
			const float a = k * 2.399963f;
			l.PositionRadius = vec4(400.0f * std::cos(a), 400.0f * std::sin(a), 150.0f + (k % 5) * 40.0f, 3000.0f);
			l.ColorBrightness.w = 2.5f * 2.0f / (crowd + 1) * (1.0f - 0.9f * k / crowd);
			l.Peak.x = l.ColorBrightness.w;
			lights.push_back(l);
		}

		// A fog light's glow, as AddFogLight describes one: its colour in
		// display terms, its strength in w, hiding nothing behind it.
		std::vector<SceneLight> fogLights;
		if (fog)
		{
			SceneLight glow = {};
			glow.PositionRadius = vec4(-260, 0, 120, 560);
			glow.ColorBrightness = vec4(0.7f, 0.75f, 0.9f, 0.06f);
			glow.DirectionCone = vec4(0, 0, 0, 0);
			glow.Flags = vec4(0, 0, 0, -1);
			fogLights.push_back(glow);
		}

		// --baked-mask: the light baked into the floor's lightmap, as the
		// engine's lights are into a level's, with a shadow mask that has it
		// reach only the floor's left half (x < 0) - which, with
		// --engine-lighting, is all of the floor it should light.
		std::vector<uint32_t> lightmaps;
		if (bakedMask)
		{
			lights[0].Flags.y = 2.0f;
			const int size = 12;
			const float perTexel = 1200.0f / (size - 1);
			auto bits = [](float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; };
			lightmaps.assign(1 + 12 + 1 + size * size / 4, 0u);
			lightmaps[0] = 1;
			const float axes[8] = { 1.0f / perTexel, 0, 0, 600.0f / perTexel, 0, 1.0f / perTexel, 0, 600.0f / perTexel };
			for (int k = 0; k < 8; k++)
				lightmaps[1 + k] = bits(axes[k]);
			lightmaps[9] = 14 * 4;
			lightmaps[10] = size | (size << 16);
			lightmaps[11] = 13;
			lightmaps[12] = 1;
			lightmaps[13] = 1;
			uint8_t* mask = (uint8_t*)&lightmaps[14];
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
					mask[y * size + x] = x < size / 2 ? 255 : 0;
			for (int t = 0; t < 2; t++)
				world.Attributes[t].Emission.z = 1.0f;
		}

		// --detail: a detail texture on the floor, as LevelScene::SetDetail
		// describes one - stripes, a light and a dark one to each checker
		// square at the first pass's scale - which shows on the floor nearer
		// than 380 units.
		std::vector<uint32_t> stripes(64 * 64);
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++)
				stripes[y * 64 + x] = ((x / 4) & 1) ? 0xffbfbfbfu : 0xff404040u;
		if (detail)
		{
			const float scale = 1.0f;
			for (int t = 0; t < 2; t++)
			{
				world.Attributes[t].CornerOffsets[0] = 2;
				memcpy(&world.Attributes[t].CornerOffsets[1], &scale, sizeof(scale));
				memcpy(&world.Attributes[t].CornerOffsets[2], &scale, sizeof(scale));
			}
		}

		client.ResetScene();
		// --bc1: the checker as S3TC blocks, as New Vision's textures come -
		// each block one colour, the average of its texels, which the
		// checker's squares are at the top levels.
		std::vector<uint32_t> blocks;
		// The floor's material: glossy, and with --bump stone's relief of 3
		// units, packed as Materials.h packs it - in eighths of a unit, in
		// the whole part of the reflectance.
		const vec4 checkerMaterial(0.3f, 0.0f, 0.04f + (bump > 0 ? 24.0f : 0.0f), 0.0f);
		if (bc1)
		{
			size_t level = 0;
			for (uint32_t l = 0, size = 64; l < checkerLevels; l++, size = size > 1 ? size / 2 : 1)
			{
				const uint32_t across = std::max(1u, (size + 3) / 4);
				for (uint32_t by = 0; by < across; by++)
					for (uint32_t bx = 0; bx < across; bx++)
					{
						uint32_t sum[3] = {}, n = 0;
						for (uint32_t y = by * 4; y < std::min(size, by * 4 + 4); y++)
							for (uint32_t x = bx * 4; x < std::min(size, bx * 4 + 4); x++, n++)
								for (int k = 0; k < 3; k++)
									sum[k] += (checker[level + y * size + x] >> (k * 8)) & 255u;
						const uint32_t r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
						const uint32_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
						blocks.push_back(c | (c << 16));
						blocks.push_back(0);
					}
				level += (size_t)size * size;
			}
			client.Texture(0, 64, 64, blocks.data(), checkerMaterial, false, checkerLevels, 1);
		}
		else
			client.Texture(0, 64, 64, checker.data(), checkerMaterial, false, checkerLevels);
		if (detail)
			client.Texture(1, 64, 64, stripes.data(), vec4(1.0f, 0.0f, 0.04f, 0.0f), false);
		if (decal)
		{
			std::vector<uint32_t> mark(64 * 64);
			for (int y = 0; y < 64; y++)
				for (int x = 0; x < 64; x++)
				{
					const float d = std::sqrt((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f)) / 20.0f;
					const uint32_t v = d >= 1.0f ? 128u : (uint32_t)(128.0f * d * d);
					mark[y * 64 + x] = 0xff000000u | (v << 16) | (v << 8) | v;
				}
			// With its mips, as the device sends a texture from its package:
			// each level the average of four texels of the one above, the
			// last few averaging the blot into the grey.
			uint32_t markLevels = 1;
			for (int size = 64, above = 0; size > 1; size /= 2, markLevels++)
			{
				const int half = size / 2;
				for (int y = 0; y < half; y++)
					for (int x = 0; x < half; x++)
					{
						uint32_t sum = 0;
						for (int k = 0; k < 4; k++)
							sum += mark[above + (y * 2 + k / 2) * size + x * 2 + k % 2] & 255u;
						const uint32_t v = sum / 4;
						mark.push_back(0xff000000u | (v << 16) | (v << 8) | v);
					}
				above += size * size;
			}
			client.Texture(2, 64, 64, mark.data(), vec4(1.0f, 0.0f, 0.04f, 0.0f), false, markLevels);
		}
		client.Geometry(0, world);
		client.Lightmaps(lightmaps);
		client.Emitters(emitterWords);

		// The camera, as the render device builds it: right and "up" - which
		// points down the screen - scaled to the view's half extents.
		const vec3 eye0(0, -700, 260), target(0, 0, 80);
		// --pan: the eye goes round the box, that many degrees a frame, from
		// --pan-angle; everything else still.
		auto eyeAt = [&](int i) {
			const float a = (panAngle + panStep * (float)i) * 3.14159265f / 180.0f;
			return vec3(target.x + (eye0.x - target.x) * std::cos(a) - (eye0.y - target.y) * std::sin(a),
				target.y + (eye0.x - target.x) * std::sin(a) + (eye0.y - target.y) * std::cos(a), eye0.z);
		};
		const vec3 eye = eyeAt(0);
		const vec3 forward = Normalized(vec3(target.x - eye.x, target.y - eye.y, target.z - eye.z));
		const vec3 right = Normalized(Cross(forward, vec3(0, 0, 1)));
		const vec3 down = Cross(forward, right);
		const float halfWidth = 1.0f, aspect = (float)height / (float)width;

		// --sprites: the plume, one quad as GeometryForSprite makes it -
		// unlit, drawn by adding (2), lit by nothing and as bright as its
		// instance's glow (Emission.w 2) - placed n times square on to the
		// view, as PlaceSprite places one, each a little further off and
		// round from the last. Its texture is a soft puff, black at the edges.
		if (sprites > 0)
		{
			std::vector<uint32_t> puff;
			for (int y = 0; y < 64; y++)
				for (int x = 0; x < 64; x++)
				{
					const float d = std::sqrt((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f)) / 30.0f;
					const float f = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
					const uint32_t v = (uint32_t)(255.0f * f + 0.5f);
					puff.push_back(0xff000000u | (v << 16) | (v << 8) | v);
				}
			uint32_t puffLevels = 1;
			for (int size = 64, above = 0; size > 1; size /= 2, puffLevels++)
			{
				const int half = size / 2;
				for (int y = 0; y < half; y++)
					for (int x = 0; x < half; x++)
					{
						uint32_t sum = 0;
						for (int k = 0; k < 4; k++)
							sum += puff[above + (y * 2 + k / 2) * size + x * 2 + k % 2] & 255u;
						const uint32_t v = sum / 4;
						puff.push_back(0xff000000u | (v << 16) | (v << 8) | v);
					}
				above += size * size;
			}
			client.Texture(3, 64, 64, puff.data(), vec4(1.0f, 0.0f, 0.04f, 0.0f), false, puffLevels);

			SceneGeometry quad;
			quad.HasMasked = true;
			const vec3 corners[4] = { vec3(-0.5f, 0.5f, 0.0f), vec3(0.5f, 0.5f, 0.0f), vec3(0.5f, -0.5f, 0.0f), vec3(-0.5f, -0.5f, 0.0f) };
			const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
			const int tris[2][3] = { { 0, 3, 2 }, { 0, 2, 1 } };
			for (const auto& tri : tris)
			{
				TriangleAttributes attr = {};
				attr.Normal = vec4(0.0f, 0.0f, 1.0f, 0.0f);
				attr.Albedo = vec4(0.3f, 0.3f, 0.3f, 0.0f);
				attr.Emission = vec4(0.0f, 0.0f, 0.0f, 2.0f);
				attr.UV01 = vec4(uv[tri[0]][0], uv[tri[0]][1], uv[tri[1]][0], uv[tri[1]][1]);
				attr.UV2Tex = vec4(uv[tri[2]][0], uv[tri[2]][1], 3.0f, 2.0f);
				const vec3 laid[3] = { corners[tri[0]], corners[tri[1]], corners[tri[2]] };
				SetUvDensity(attr, laid);
				for (int v = 0; v < 3; v++)
					quad.Positions.push_back(corners[tri[v]]);
				quad.Attributes.push_back(attr);
			}
			client.Geometry(4, quad);

			const vec3 up(-down.x, -down.y, -down.z);
			const float size = 180.0f, glow = 0.25f;
			for (int k = 0; k < sprites; k++)
			{
				const float a = k * 2.399963f, r = 50.0f * std::sqrt((k + 0.5f) / sprites);
				const float along = 380.0f + k * 3.0f;
				const vec3 centre(eye.x + forward.x * along + (right.x * std::cos(a) + up.x * std::sin(a)) * r,
					eye.y + forward.y * along + (right.y * std::cos(a) + up.y * std::sin(a)) * r,
					eye.z + forward.z * along + (right.z * std::cos(a) + up.z * std::sin(a)) * r);
				SceneInstance puffAt = {};
				puffAt.GeometryIndex = 4;
				const vec3 columns[3] = { right * size, up * size, vec3(-forward.x, -forward.y, -forward.z) };
				for (int col = 0; col < 3; col++)
				{
					puffAt.Transform[0 * 4 + col] = columns[col].x;
					puffAt.Transform[1 * 4 + col] = columns[col].y;
					puffAt.Transform[2 * 4 + col] = columns[col].z;
				}
				puffAt.Transform[3] = centre.x;
				puffAt.Transform[7] = centre.y;
				puffAt.Transform[11] = centre.z;
				// Its glow, and InstanceFlags' w: one more than the glow, 32
				// more in the fog zone --fog makes, and still.
				puffAt.Ambient = vec4(glow, glow, glow, 1.0f + glow + (fog ? 32.0f : 0.0f));
				instances.push_back(puffAt);
			}
		}

		TraceProtocol::TraceCommand frame = {};
		frame.Width = width;
		frame.Height = height;
		frame.MaxSamples = 256;
		frame.Bounces = 3;
		frame.GlossBounces = 1;
		frame.Denoise = reference ? TraceProtocol::DenoiseOff : dlss >= 0 ? TraceProtocol::DenoiseDlss : TraceProtocol::DenoiseNrd;
		frame.LightSize = lightSize;
		frame.MaxAnisotropy = 16;
		frame.Lighting = engineLighting;
		frame.DlssQuality = (uint32_t)std::max(fsr >= 0 ? fsr : dlss, 0);
		frame.Upscaler = fsr >= 0 ? TraceProtocol::UpscaleAlways : TraceProtocol::UpscaleNone;
		frame.Materials = 1;
		frame.DisableBits = (fogUnshadowed ? 65536u : 0u) | (glowUnsampled ? 262144u : 0u) | (everyLight ? 2097152u : 0u) | (neutral ? 8192u : 0u);
		frame.ToneCeiling = toneCeiling;
		if (photoAperture >= 0.0f)
		{
			frame.DisableBits |= 524288u;
			frame.MaxSamples = 4095;
			frame.PhotoLens = vec4(photoAperture, 0.0f, 0.0f, 0.0f);
		}
		frame.GlowLighting = 1.0f;
		frame.Wetness = wetness / 100.0f;
		frame.ViewMode = view;
		frame.BumpMapping = bump / 100.0f;
		frame.Timing = 1;
		if (headset)
		{
			frame.Headset = 1;
			frame.UnitsPerMetre = 52.5f;
			frame.HeadsetResolution = 1.0f;
			frame.HudDistance = 1.5f;
			frame.HeadsetGamma = 1.0f;
			// Three layers, one above the other: the HUD's, which follows the
			// head, the crosshair's, which follows the aim, and the coronas',
			// each the whole picture and 60 degrees across, as the device's
			// default.
			frame.HudWidth = width;
			frame.HudHeight = height * 3;
			frame.HudLayers = 3;
			frame.HudRect[2] = width;
			frame.HudRect[3] = height;
			frame.HudTangents[0] = std::tan(30.0f * 3.14159265f / 180.0f);
			frame.HudTangents[1] = frame.HudTangents[0] * height / width;
			frame.HudHead[3] = 1.0f;
		}
		frame.Exposure = 0.2f + 128 * (2.0f / 255.0f);
		frame.SkyIntensity = dark ? 0.0f : 128 * (2.0f / 255.0f);
		frame.Camera[0] = vec4(eye.x, eye.y, eye.z, 1.0f);     // w: the flash's scale, neutral
		frame.Camera[1] = vec4(right.x * halfWidth, right.y * halfWidth, right.z * halfWidth, 0.0f);
		frame.Camera[2] = vec4(down.x * halfWidth * aspect, down.y * halfWidth * aspect, down.z * halfWidth * aspect, 0.0f);
		frame.Camera[3] = vec4(forward.x, forward.y, forward.z, 0.0f);
		for (int i = 0; i < 4; i++)
			frame.PreviousCamera[i] = frame.Camera[i];
		// The torch as LevelScene places the player's: beside the eye, to
		// the left and up, aimed at the box.
		if (flashlight)
		{
			const vec3 lamp(eye.x - right.x * 6.0f, eye.y - right.y * 6.0f, eye.z - right.z * 6.0f + 2.0f);
			const vec3 aim = Normalized(vec3(target.x - lamp.x, target.y - lamp.y, target.z - lamp.z));
			frame.Flashlight[0] = vec4(lamp.x, lamp.y, lamp.z, 1.0f);
			frame.Flashlight[1] = vec4(aim.x, aim.y, aim.z, 8.0f * 81920.0f);
			frame.Flashlight[2] = vec4(1.0f, 0.93f, 0.8f, 5.0e-5f);
		}
		// --inset: a second view, as a security camera's in a window of the
		// HUD's, from the right of the box and above, in the picture's top
		// right third.
		if (inset)
		{
			const vec3 insetEye(650, 150, 420);
			const vec3 insetForward = Normalized(vec3(target.x - insetEye.x, target.y - insetEye.y, target.z - insetEye.z));
			const vec3 insetRight = Normalized(Cross(insetForward, vec3(0, 0, 1)));
			const vec3 insetDown = Cross(insetForward, insetRight);
			TraceProtocol::TraceInset& view = frame.Insets[0];
			view.Width = width / 3;
			view.Height = height / 3;
			view.X = width - view.Width - width / 32;
			view.Y = height / 32;
			const float insetAspect = (float)view.Height / (float)view.Width;
			view.Camera[0] = vec4(insetEye.x, insetEye.y, insetEye.z, 1.0f);
			view.Camera[1] = vec4(insetRight.x, insetRight.y, insetRight.z, 0.0f) * 0.8f;
			view.Camera[2] = vec4(insetDown.x, insetDown.y, insetDown.z, 0.0f) * (0.8f * insetAspect);
			view.Camera[3] = vec4(insetForward.x, insetForward.y, insetForward.z, 0.0f);
			frame.InsetCount = 1;
		}
		// Frame to frame change over the last quarter, for --still.
		std::vector<float> lastPicture;
		double changeSum = 0.0;
		double gpuTraceSum = 0.0, gpuDenoiseSum = 0.0, gpuTotalSum = 0.0;
		int gpuTimedFrames = 0;
		int changeCount = 0;

		auto pool = CommandPoolBuilder().QueueFamily(device->GraphicsFamily).Create(device.get());
		auto fence = FenceBuilder().Create(device.get());
		std::unique_ptr<VulkanCommandBuffer> pending;
		const size_t bytes = (size_t)(width * 3 / 2) * (height * 3 / 2) * 8;
		auto readback = BufferBuilder().Size(bytes).Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU).Create(device.get());

		// --headset's HUD, as the device would draw it, premultiplied, in
		// half floats: a bar along the bottom of the HUD's layer, black at
		// half coverage, a white crosshair in the middle of the aim's layer
		// below it, and an orange corona up and to the right in the
		// coronas' layer below that.
		std::unique_ptr<VulkanBuffer> hudPixels;
		uint32_t hudGeneration = 0;
		if (headset)
		{
			const size_t hudBytes = (size_t)width * height * 3 * 8;
			hudPixels = BufferBuilder().Size(hudBytes).Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY).Create(device.get());
			uint16_t* texels = (uint16_t*)hudPixels->Map(0, hudBytes);
			const uint16_t one = 0x3c00, half = 0x3800, zero = 0;
			for (uint32_t row = 0; row < height * 3; row++)
				for (uint32_t x = 0; x < width; x++)
				{
					uint16_t* t = texels + ((size_t)row * width + x) * 4;
					const uint32_t layer = row / height;
					const uint32_t y = row - layer * height;
					const bool aim = layer == 1;
					const bool cross = aim && ((std::abs((int)x - (int)width / 2) < 2 && std::abs((int)y - (int)height / 2) < 12) ||
						(std::abs((int)y - (int)height / 2) < 2 && std::abs((int)x - (int)width / 2) < 12));
					const bool bar = layer == 0 && y > height - height / 10;
					const int cx = (int)x - (int)(width * 3 / 4), cy = (int)y - (int)(height / 4);
					const bool corona = layer == 2 && cx * cx + cy * cy < 100;
					t[0] = cross || corona ? one : zero;
					t[1] = cross ? one : corona ? half : zero;
					t[2] = cross ? one : zero;
					t[3] = cross || corona ? one : bar ? half : zero;
				}
			hudPixels->Unmap();
		}

		int arrived = 0;
		double sendMs = 0.0, helperWait = 0.0, helperApply = 0.0, helperRecord = 0.0;
		uint32_t stalls = 0;
		LARGE_INTEGER loopStart, loopEnd, frequency;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&loopStart);
		for (int i = 0; i < frames; i++)
		{
			if (!still || i == 0)
				client.Geometry(1, animated(still ? 0 : i, !still && i >= frames / 3));
			if (!still && i == frames / 5)
			{
				client.Geometry(2, cube);
				instances.insert(instances.end(), ring.begin(), ring.end());
				lights.insert(lights.end(), ringLights.begin(), ringLights.end());
			}
			if (!still && i == frames / 4)
			{
				client.Geometry(3, panel);
				instances.push_back(placed);
				instances.back().GeometryIndex = 3;
			}
			client.Instances(instances, 1);
			client.Lights(lights, fogLights);
			frame.Frame = (uint32_t)i;
			if (panStep != 0.0f)
			{
				for (int c = 0; c < 4; c++)
					frame.PreviousCamera[c] = frame.Camera[c];
				const vec3 e = eyeAt(i);
				const vec3 f = Normalized(vec3(target.x - e.x, target.y - e.y, target.z - e.z));
				const vec3 r = Normalized(Cross(f, vec3(0, 0, 1)));
				const vec3 d = Cross(f, r);
				frame.Camera[0] = vec4(e.x, e.y, e.z, 1.0f);
				frame.Camera[1] = vec4(r.x * halfWidth, r.y * halfWidth, r.z * halfWidth, 0.0f);
				frame.Camera[2] = vec4(d.x * halfWidth * aspect, d.y * halfWidth * aspect, d.z * halfWidth * aspect, 0.0f);
				frame.Camera[3] = vec4(f.x, f.y, f.z, 0.0f);
				if (i == 0)
					for (int c = 0; c < 4; c++)
						frame.PreviousCamera[c] = frame.Camera[c];
			}
			if (!still && i == frames / 2)
			{
				frame.Width = width * 3 / 2;
				frame.Height = height * 3 / 2;
			}
			frame.AccumulatedFrames = (uint32_t)i;
			frame.Insets[0].AccumulatedFrames = (uint32_t)i;
			frame.RestartDenoiser = i == 0;
			// The HUD drawn last frame, into the image as it then was.
			frame.HudDrawn = headset && hudGeneration != 0 && hudGeneration == client.HudGeneration() ? 1 : 0;
			frame.HudDrawnGeneration = hudGeneration;
			LARGE_INTEGER a, b, f;
			QueryPerformanceCounter(&a);
			const bool traced = client.Trace(frame);
			QueryPerformanceCounter(&b);
			QueryPerformanceFrequency(&f);
			sendMs += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
			if (client.Alive())
			{
				helperWait += client.Status().HelperWaitMs;
				helperApply += client.Status().HelperApplyMs;
				helperRecord += client.Status().HelperRecordMs;
				stalls += client.Status().HelperStalls;
			}

			// The last frame is waited for only now, as the render device does.
			if (pending)
			{
				vkWaitForFences(device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
				vkResetFences(device->device, 1, &fence->fence);
				pending.reset();
			}
			if (!traced)
			{
				printf("frame %d: not traced%s%s\n", i, client.Alive() ? "" : " - ", client.Alive() ? "" : client.Error().c_str());
				if (!client.Alive())
					return 1;
				continue;
			}

			// Taken over from the helper exactly as the render device takes it.
			auto commands = pool->createBuffer();
			commands->begin();

			// The HUD, drawn as the device draws it for the headset: into the
			// helper's image, which a new generation of has never been
			// anywhere and otherwise comes back from the helper, and handed
			// back to it.
			if (headset && client.Hud())
			{
				const bool fresh = client.HudGeneration() != hudGeneration;
				VkImageMemoryBarrier hud = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				hud.image = client.Hud();
				hud.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				hud.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
				hud.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				hud.srcQueueFamilyIndex = fresh ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
				hud.dstQueueFamilyIndex = fresh ? VK_QUEUE_FAMILY_IGNORED : (uint32_t)device->GraphicsFamily;
				hud.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hud);
				VkBufferImageCopy region = {};
				region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.imageExtent = { std::min(width, client.HudWidth()), std::min(height * 3, client.HudHeight()), 1 };
				region.bufferRowLength = width;
				vkCmdCopyBufferToImage(commands->buffer, hudPixels->buffer, client.Hud(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				hud.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				hud.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				hud.srcQueueFamilyIndex = (uint32_t)device->GraphicsFamily;
				hud.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
				hud.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				hud.dstAccessMask = 0;
				vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &hud);
				hudGeneration = client.HudGeneration();
			}

			// A frame handed over with nothing new in the picture - the
			// headset's, with nothing to trace - leaves it as it was.
			if (client.OutputFresh())
			{
				VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				barrier.image = client.Output();
				barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				// Traced here, on this queue, it has no hands to change.
				const bool handOver = !client.IsLocal();
				barrier.srcQueueFamilyIndex = handOver ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = handOver ? (uint32_t)device->GraphicsFamily : VK_QUEUE_FAMILY_IGNORED;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
				VkBufferImageCopy region = {};
				region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.imageExtent = { client.OutputWidth(), client.OutputHeight(), 1 };
				vkCmdCopyImageToBuffer(commands->buffer, client.Output(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &region);
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				barrier.srcQueueFamilyIndex = handOver ? (uint32_t)device->GraphicsFamily : VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = handOver ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				barrier.dstAccessMask = 0;
				vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			}
			commands->end();

			VkSemaphore ready = client.Ready(), released = client.Released();
			VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &ready;
			submit.pWaitDstStageMask = &stage;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &commands->buffer;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &released;
			vkQueueSubmit(device->GraphicsQueue, 1, &submit, fence->fence);
			pending = std::move(commands);
			arrived++;

			if (still && i >= frames - frames / 4 - 1)
			{
				vkWaitForFences(device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
				vkResetFences(device->device, 1, &fence->fence);
				pending.reset();
				const size_t count = (size_t)client.OutputWidth() * client.OutputHeight();
				const uint16_t* half = (const uint16_t*)readback->Map(0, count * 8);
				std::vector<float> picture(count * 3);
				for (size_t p = 0; p < count; p++)
					for (int c = 0; c < 3; c++)
						picture[p * 3 + c] = std::min(std::max(HalfToFloat(half[p * 4 + c]), 0.0f), 1.0f);
				readback->Unmap();
				if (lastPicture.size() == picture.size())
				{
					double sum = 0.0;
					for (size_t p = 0; p < picture.size(); p++)
						sum += std::abs(picture[p] - lastPicture[p]);
					changeSum += sum / picture.size();
					changeCount++;
				}
				lastPicture.swap(picture);
			}

			const auto& s = client.Status();
			// The GPU's times averaged over the second half, once the scene
			// has settled, for comparing one build against another.
			if (i >= frames / 2 && s.GpuTimed)
			{
				gpuTraceSum += s.GpuTraceMs;
				gpuDenoiseSum += s.GpuDenoiseMs;
				gpuTotalSum += s.GpuBuildMs + s.GpuTraceMs + s.GpuDenoiseMs + s.GpuCompositeMs;
				gpuTimedFrames++;
			}
			if (i == 0 || i == frames / 2 || i == frames - 1)
			{
				static const char* denoisers[] = { "none", "NRD", "DLSS-RR", "NRD+FSR" };
				printf("frame %d: %ux%u traced at %ux%u, %u lights, %u textures, %u shapes, %u instances, denoised with %s (NRD %s, DLSS %s), GPU build %.2f trace %.2f denoise %.2f composite %.2f ms%s\n",
					i, client.OutputWidth(), client.OutputHeight(), s.RenderWidth, s.RenderHeight, s.LightCount, s.TextureCount, s.BottomCount, s.InstanceCount,
					denoisers[std::min(s.DenoisedWith, 3u)], s.DenoiserStatus, s.DlssStatus, s.GpuBuildMs, s.GpuTraceMs, s.GpuDenoiseMs, s.GpuCompositeMs,
					s.GpuTimed ? "" : " (not timed)");
			}
		}
		if (pending)
			vkWaitForFences(device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
		QueryPerformanceCounter(&loopEnd);
		const double loopMs = (double)(loopEnd.QuadPart - loopStart.QuadPart) * 1000.0 / (double)frequency.QuadPart;
		printf("%d of %d frames arrived, %.2f ms a frame in all, %.2f ms a frame to send and have traced (helper: wait %.2f, apply %.2f, record %.2f; %u stalls)\n",
			arrived, frames, loopMs / frames, sendMs / frames, helperWait / frames, helperApply / frames, helperRecord / frames, stalls);
		if (gpuTimedFrames > 0)
			printf("GPU over the last %d frames: trace %.2f ms, denoise %.2f ms, all %.2f ms\n", gpuTimedFrames,
				gpuTraceSum / gpuTimedFrames, gpuDenoiseSum / gpuTimedFrames, gpuTotalSum / gpuTimedFrames);

		// The last frame, tonemapped already by the helper, as a PPM.
		const uint32_t outWidth = client.OutputWidth(), outHeight = client.OutputHeight();
		const uint16_t* half = (const uint16_t*)readback->Map(0, bytes);
		FILE* out = fopen("helper-test.ppm", "wb");
		fprintf(out, "P6\n%u %u\n255\n", outWidth, outHeight);
		double sum = 0.0;
		uint32_t aboveWhite = 0;
		float brightest = 0.0f;
		for (uint32_t i = 0; i < outWidth * outHeight; i++)
		{
			float peak = 0.0f;
			for (int c = 0; c < 3; c++)
				peak = std::max(peak, HalfToFloat(half[i * 4 + c]));
			// Gamma encoded, as the display's picture is: 1 is the SDR white.
			brightest = std::max(brightest, std::pow(std::max(peak, 0.0f), 2.2f));
			if (peak > 1.0f)
				aboveWhite++;
			for (int c = 0; c < 3; c++)
			{
				float v = HalfToFloat(half[i * 4 + c]);
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				sum += v;
				fputc((int)(v * 255.0f + 0.5f), out);
			}
		}
		fclose(out);
		readback->Unmap();
		const double mean = sum / (outWidth * outHeight * 3.0);
		printf("mean brightness %.3f, written to helper-test.ppm\n", mean);
		if (toneCeiling > 1.0f)
			printf("hdr: %.2f%% of the picture above the SDR white, the brightest %.2f times it (ceiling %.2f)\n",
				100.0 * aboveWhite / (outWidth * outHeight), brightest, toneCeiling);
		if (still && changeCount > 0)
			printf("still: frame to frame change %.5f on average over the last %d frames\n",
				changeSum / changeCount, changeCount);

		vkDeviceWaitIdle(device->device);
		const bool ok = arrived == frames && mean > 0.02;
		printf("=> %s\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
	catch (const std::exception& e)
	{
		printf("FAIL %s\n", e.what());
		return 1;
	}
}
