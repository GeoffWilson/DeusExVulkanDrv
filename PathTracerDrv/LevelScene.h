#pragma once

#include "vec.h"
#include <vector>
#include <unordered_map>

class UPathTracerRenderDevice;

// One triangle's worth of shading data, indexed in the trace shader by the
// instance's custom index plus the primitive index. Normals are in object space
// and rotated into world space by the shader using the instance transform, so
// the same geometry can be instanced at any orientation.
struct TriangleAttributes
{
	vec4 Normal;
	vec4 Albedo;
	vec4 Emission;
};

// A light as the engine describes it, converted to something physical.
struct SceneLight
{
	vec4 PositionRadius;
	vec4 ColorBrightness;
};

// Triangles that share a bottom level acceleration structure.
struct SceneGeometry
{
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
};

// Turns the engine's level into geometry, lights and placements.
//
// Deliberately not built from what the engine pushes at a render device:
// DrawComplexSurface only describes what survived frustum and BSP culling, and a
// path tracer needs the geometry behind the camera as much as in front of it.
// The level is read out of UModel instead, through FSceneNode::Level.
class LevelScene
{
public:
	// The static world. Returns false if there was nothing to build from.
	bool BuildStatic(ULevel* level);

	// The things that move: mover brushes and mesh actors. Called every frame.
	// Geometry is built once per distinct shape and cached; only the placements
	// change from frame to frame.
	void CollectDynamic(ULevel* level);

	void Clear();

	bool IsEmpty() const { return Geometries.empty(); }

	std::vector<SceneGeometry> Geometries;
	std::vector<SceneLight> Lights;

	// Rebuilt each frame. The first entry is always the static world.
	std::vector<SceneInstance> Instances;

	// Set when CollectDynamic added geometry, so the device knows the bottom
	// level structures and the attribute buffer need extending.
	bool GeometryAdded = false;

	ULevel* SourceLevel = nullptr;
	int SourceNodeCount = 0;

private:
	void AddBspSurfaces(UModel* model, SceneGeometry& out, bool skipPortals);
	void AddLights(ULevel* level);

	// Geometry index for a mover's brush, built on first sight.
	int GeometryForBrush(UModel* brush);
	// Geometry index for a mesh at a particular animation frame.
	int GeometryForMesh(UMesh* mesh, int frame);

	std::unordered_map<void*, int> BrushGeometry;
	std::unordered_map<uint64_t, int> MeshGeometry;

	// A ceiling on how many poses are kept. Each one is a bottom level
	// structure, and a level with many characters could otherwise build them
	// without limit.
	static const int MaxMeshGeometries = 768;
};
