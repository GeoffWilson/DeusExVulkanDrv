#pragma once

#include "vec.h"
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
};

// A light as the engine describes it, converted to something physical.
struct SceneLight
{
	vec4 PositionRadius;
	vec4 ColorBrightness;
	// xyz which way a spotlight points; w the cosine of its cone's edge, or
	// -1 for a light that shines every way.
	vec4 DirectionCone;
	// x no incidence falloff, y distance measured horizontally (a cylinder),
	// z brightness changes from frame to frame, w the light's pattern: 0
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
struct SceneInstance
{
	int GeometryIndex = 0;
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

	// Set when geometry was added, so whoever keeps acceleration structures
	// for it knows to extend them.
	bool GeometryAdded = false;
};
