#pragma once

#include "vec.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The scene as the tracer consumes it: plain data, with nothing of the engine
// in it. The game's process builds it from the level (LevelScene) and the
// 64-bit helper that traces it holds a copy, sent over the shared memory
// channel (TraceProtocol.h), so every type here has the same layout in a 32-bit
// and a 64-bit build.

// One triangle's worth of shading data, indexed in the trace shader by the
// instance's custom index plus the primitive index. Normals are in object space
// and rotated into world space by the shader using the instance transform, so
// the same geometry can be instanced at any orientation.
struct TriangleAttributes
{
	vec4 Normal;
	vec4 Albedo;
	// xy how fast the texture pans; w how the surface is drawn without
	// lighting: 0 lit, 1 unlit, 1.25 unlit at its instance's ScaleGlow, 2 a
	// sprite.
	vec4 Emission;
	// The zone's ambient light. A property of the surface for level geometry,
	// which is why it lives here rather than only per instance: one room can be
	// lit and the next pitch dark, and the engine's own lighting says so.
	vec4 Ambient;
	// Texture coordinates at the three corners, and which texture they index.
	// Interpolated in the shader from the hit's barycentrics. Texture is stored
	// as a float purely to keep the record to whole vec4s; it is an integer
	// index into the bound texture array, or -1 for an untextured surface.
	vec4 UV01;      // u0 v0 u1 v1
	vec4 UV2Tex;    // u2 v2 texture unused
	// A mesh's normal at each corner, smoothed as the engine smooths it: the
	// average of the unit normals of every face using that vertex. The engine
	// lights a mesh at its vertices and blends the light across each face, so
	// its meshes look rounded where the triangles are flat. Each is packed
	// octahedrally, 16 bits a component (the shader's unpackSnorm2x16); w is 1
	// when the corners are there and 0 for a flat surface, which is everything
	// that is not a mesh.
	uint32_t CornerNormals[4] = {};
	// How far each corner's two neighbours lie off its tangent plane,
	// dot(Pj - Pi, Ni), as half floats in pairs: corner 0's to 1 and 2, corner
	// 1's to 0 and 2, corner 2's to 0 and 1. What the trace needs to lift a ray's
	// start off the flat triangle onto the rounded surface: see smoothNormal in
	// Shaders.cpp.
	uint32_t CornerOffsets[4] = {};
	// CornerOffsets[3] is every triangle's: how finely its texture is laid on
	// it, for picking a mip level (SetUvDensity). On a flat triangle, which
	// has no corner normals, CornerNormals[0..2] say how its texture
	// coordinates change across it, for filtering its texture anisotropically.
};

// A unit vector in 32 bits: octahedral, 16 bits a component, laid out as the
// shader's unpackSnorm2x16 reads it.
inline uint32_t PackUnitVector(vec3 n)
{
	const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
	float x = n.x / l1, y = n.y / l1;
	if (n.z < 0.0f)
	{
		const float foldedX = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
		const float foldedY = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
		x = foldedX;
		y = foldedY;
	}
	// floor(v * 32767 + 0.5), without the library's floor: this runs for
	// every corner of every character every frame, and the call made packing
	// a triangle's normals seven times slower in the 32 bit device.
	auto snorm = [](float v)
	{
		const float f = (v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v)) * 32767.0f + 0.5f;
		int r = (int)f;
		if ((float)r > f)
			r--;
		return (uint32_t)(uint16_t)(int16_t)r;
	};
	return snorm(x) | (snorm(y) << 16);
}

// log2 to within 2e-6, for a triangle's mip density, without the library
// call, which took 50 ns in the 32 bit device - more than all the rest of
// posing a character's triangle. The exponent from the bits, and the rest
// from the series for ln((1 + t) / (1 - t)) with t under a third.
inline float FastLog2(float x)
{
	uint32_t bits;
	memcpy(&bits, &x, sizeof(bits));
	const uint32_t exponent = (bits >> 23) & 255u;
	if (exponent == 0 || exponent == 255)
		return std::log2(x);
	bits = (bits & 0x7fffffu) | 0x3f800000u;
	float m;
	memcpy(&m, &bits, sizeof(m));
	const float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
	const float series = t * (1.0f + t2 * (1.0f / 3.0f + t2 * (1.0f / 5.0f + t2 * (1.0f / 7.0f + t2 * (1.0f / 9.0f)))));
	return (float)((int)exponent - 127) + 2.8853900817779268f * series;
}

// A float as a half, rounded to nearest, for distances across one of a mesh's
// triangles: too small for a normal half is zero, too large the largest.
inline uint32_t PackHalf(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	const uint32_t sign = (bits >> 16) & 0x8000u;
	const int exponent = (int)((bits >> 23) & 0xffu) - 127 + 15;
	const uint32_t mantissa = bits & 0x7fffffu;
	if (exponent <= 0)
		return sign;
	if (exponent >= 31)
		return sign | 0x7bffu;
	uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
	if (mantissa & 0x1000u)
		half++;
	if ((half & 0x7fffu) > 0x7bffu)
		half = sign | 0x7bffu;
	return half;
}

// Fills in a triangle's CornerNormals and CornerOffsets from its corners and
// the unit normal wanted at each, in the same space as its positions.
inline void SetCornerNormals(TriangleAttributes& attr, const vec3 corners[3], const vec3 normals[3])
{
	for (int i = 0; i < 3; i++)
	{
		attr.CornerNormals[i] = PackUnitVector(normals[i]);
		// The other two corners, in order.
		const int j = i == 0 ? 1 : 0;
		const int k = i == 2 ? 1 : 2;
		attr.CornerOffsets[i] = PackHalf(dot(corners[j] - corners[i], normals[i])) |
			(PackHalf(dot(corners[k] - corners[i], normals[i])) << 16);
	}
	attr.CornerNormals[3] = 1;
	attr.CornerOffsets[3] = 0;
}

// How finely a triangle's texture is laid on it: half the log2 of its area in
// texture coordinates over its area in space, measured in its geometry's own
// space. The trace adds the texture's size and the width of the ray's
// footprint where it lands to choose a mip level (surfaceLod in Shaders.cpp).
// In CornerOffsets[3], after SetCornerNormals, which clears it; zero for a
// triangle with no texture laid across it.
inline void SetUvDensity(TriangleAttributes& attr, const vec3 corners[3])
{
	attr.CornerOffsets[3] = 0;
	const float du1 = attr.UV01.z - attr.UV01.x, dv1 = attr.UV01.w - attr.UV01.y;
	const float du2 = attr.UV2Tex.x - attr.UV01.x, dv2 = attr.UV2Tex.y - attr.UV01.y;
	const float uvArea = std::abs(du1 * dv2 - dv1 * du2);
	const vec3 e1 = corners[1] - corners[0], e2 = corners[2] - corners[0];
	const vec3 c(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
	const float area = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
	if (!(uvArea > 0.0f) || !(area > 0.0f))
		return;
	float density = 0.5f * FastLog2(uvArea / area);
	if (density == 0.0f)
		density = 1.0e-6f;
	memcpy(&attr.CornerOffsets[3], &density, sizeof(density));

	// A flat triangle also carries how its texture coordinates change across
	// it: the gradients of u and of v along its plane, each as a direction
	// and a length, which the trace turns a ray's footprint into texture
	// space with to filter along the footprint's longer axis - a floor seen
	// at a glance down a corridor is a pixel wide one way and twenty texels
	// the other. Solved from the corners: the vector g in the plane with
	// g.e1 and g.e2 the change along each edge.
	if (attr.CornerNormals[3] == 0)
	{
		const float nn = c.x * c.x + c.y * c.y + c.z * c.z;
		const vec3 a(e2.y * c.z - e2.z * c.y, e2.z * c.x - e2.x * c.z, e2.x * c.y - e2.y * c.x);   // e2 x n
		const vec3 b(c.y * e1.z - c.z * e1.y, c.z * e1.x - c.x * e1.z, c.x * e1.y - c.y * e1.x);   // n x e1
		const vec3 tu = (a * du1 + b * du2) * (1.0f / nn);
		const vec3 tv = (a * dv1 + b * dv2) * (1.0f / nn);
		const float lu = std::sqrt(tu.x * tu.x + tu.y * tu.y + tu.z * tu.z);
		const float lv = std::sqrt(tv.x * tv.x + tv.y * tv.y + tv.z * tv.z);
		if (lu > 0.0f && lv > 0.0f)
		{
			attr.CornerNormals[0] = PackUnitVector(tu * (1.0f / lu));
			attr.CornerNormals[1] = PackUnitVector(tv * (1.0f / lv));
			attr.CornerNormals[2] = PackHalf(lu) | (PackHalf(lv) << 16);
		}
	}
}

// A light as the engine describes it, converted to something physical.
struct SceneLight
{
	vec4 PositionRadius;
	vec4 ColorBrightness;
	// xyz which way a spotlight points; w the cosine of its cone's edge, or
	// -1 for a light that shines every way.
	vec4 DirectionCone;
	// x no incidence falloff, y 1 for distance measured horizontally (a
	// cylinder) plus twice the light's number among those baked into the
	// level's lightmaps (numbered from 1, so 2 or more is baked), z
	// brightness changes from frame to frame, w the light's pattern: 0
	// disco, 1 searchlight (its sweep offset in DirectionCone.x), 2 rotor
	// (which way it turns, 1 or -1, in DirectionCone.x), -1 none.
	vec4 Flags;
};

// Triangles that share a bottom level acceleration structure.
struct SceneGeometry
{
	// Whether anything in here needs its texture's alpha consulted before a hit
	// counts. Geometry without it can be marked opaque, which lets traversal
	// accept a hit without ever calling back into the shader.
	bool HasMasked = false;
	// Rebuilt every frame, so its acceleration structure has to be refitted
	// rather than built once.
	bool Dynamic = false;
	// Bumped whenever a dynamic geometry is rebuilt, so the device uploads and
	// rebuilds only what changed. An animating character changes every frame;
	// the decals change only when one is added or removed.
	uint32_t Version = 1;
	std::vector<vec3> Positions;
	std::vector<TriangleAttributes> Attributes;
};

// One placement of a geometry in the world, rebuilt every frame for anything
// that moves.
// Which rays see an instance: its Vulkan instance mask, against the cull mask
// each kind of ray is traced with in Shaders.cpp.
enum InstanceMask : uint32_t
{
	InstanceSeenByAll = 0xFF,
	// The viewer's own body while the camera is inside it. The engine never
	// draws it from there, but a mirror shows it, and so does anything else
	// that bounces, and a view in a window of the HUD's - everything except
	// the view itself and shadows.
	InstanceSeenReflected = 0x12,
	// The weapon in the player's hands, which the engine draws over the view:
	// seen by the view and throwing its shadows, but not floating at the
	// player's eyes in a mirror or in a window's view.
	InstanceSeenByView = 0x04,
	// Seen by everything but shadow rays: a light fitting with its lamp
	// inside it. The engine never lets a mesh shadow a lightmap, and a
	// fitting built round its light shut the room's light in with it.
	InstanceCastsNoShadow = 0x18,
	// Taken out of an instance's mask, it is not seen by the views in the
	// HUD's windows: the security camera or drone a window is seen from,
	// which the window hides while it draws, and whose inside the view
	// would otherwise start in.
	InstanceSeenByWindows = 0x10,
};

struct SceneInstance
{
	int GeometryIndex = 0;
	uint32_t Mask = InstanceSeenByAll;
	uint32_t AttributeBase = 0;
	float Transform[12] = {};   // 3x4, row major, as Vulkan wants it
	// An actor carries its zone's ambient with it, because the same mesh is
	// instanced in rooms with different lighting.
	vec4 Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);
	// Where it was last frame, for motion vectors. Left unset for anything
	// that did not exist then, or never moves, which counts as not moving.
	bool HasPrevious = false;
	float PreviousTransform[12] = {};
};

// The shapes, their placements and the lights.
class SceneData
{
public:
	std::vector<SceneGeometry> Geometries;
	// How many of the first geometries are the level itself: the opaque part
	// and the part whose surfaces the shader has to judge.
	int StaticGeometries = 0;
	std::vector<SceneLight> Lights;
	// Lights that also glow in the air of a fog zone, in the same record:
	// PositionRadius holds the glow's own radius, ColorBrightness the colour
	// it tints the air in display terms with its strength in w, and
	// DirectionCone.x how much of what lies behind it the glow hides. See
	// AddFogLight.
	std::vector<SceneLight> FogLights;

	// Rebuilt each frame. The first entry is always the static world.
	std::vector<SceneInstance> Instances;

	// The engine's own shadow masks on the level's lightmapped surfaces, as
	// words: the number of surfaces, then each one's record of 12 words -
	// how a point maps onto its lightmap (u = dot(xyz, point) + w, texel
	// centres at whole numbers), four floats for u and four for v; the byte
	// its masks start at; its width and height, 16 bits each; where its list
	// of baked light numbers starts and how long it is - then the lists, then
	// the masks, a byte a texel, one after another in the list's order. A
	// level surface's triangles carry their record's number plus one in
	// Emission.z. Sent once a level (LevelScene::AddLightmap).
	std::vector<uint32_t> Lightmaps;
	// Set when they arrive, so the buffer the shader reads them from is
	// written once rather than every frame.
	bool LightmapsChanged = false;

	// The level's glowing surfaces as lights, and the grid saying which to
	// sample where, as words: see EmitterGrid.h. Sent once a level, like the
	// lightmaps.
	std::vector<uint32_t> Emitters;
	bool EmittersChanged = false;

	// Set when geometry was added, so whoever keeps acceleration structures
	// for it knows to extend them.
	bool GeometryAdded = false;
};
