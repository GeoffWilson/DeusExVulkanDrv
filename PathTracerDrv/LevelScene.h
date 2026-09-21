#pragma once

#include "vec.h"
#include <vector>

class UPathTracerRenderDevice;

// One triangle's worth of shading data, indexed by gl_PrimitiveID in the trace
// shader. Kept separate from the position buffer the acceleration structure is
// built over, which must be tightly packed vec3s and nothing else.
struct TriangleAttributes
{
	vec4 Normal;      // xyz world normal, w unused
	vec4 Albedo;      // rgb reflectance 0..1, a unused
	vec4 Emission;    // rgb emitted radiance, a unused
};

// A light as the engine describes it, converted to something physical.
struct SceneLight
{
	vec4 PositionRadius;   // xyz world position, w radius in world units
	vec4 ColorBrightness;  // rgb linear colour, a scalar brightness
};

// The static level, converted once per level into what a ray tracer needs.
//
// This is deliberately not built from what the engine pushes at a render
// device. DrawComplexSurface only ever arrives for surfaces that survived
// frustum and BSP culling, and a path tracer needs the geometry behind the
// camera as much as the geometry in front of it - that is where the bounce
// light comes from. So the level is read directly out of UModel instead, which
// a render device can reach through FSceneNode::Level.
class LevelScene
{
public:
	// Returns false if there was nothing to build from.
	bool Build(ULevel* level);

	void Clear();

	bool IsEmpty() const { return Positions.empty(); }
	int TriangleCount() const { return (int)(Positions.size() / 3); }

	// Tightly packed triangle soup: three positions per triangle, no indices.
	// An index buffer would save memory, but the shading attributes are per
	// triangle rather than per vertex - the engine's surfaces are flat - so
	// there is nothing to share.
	std::vector<vec3> Positions;
	std::vector<TriangleAttributes> Attributes;
	std::vector<SceneLight> Lights;

	// What this was built from, so the device knows when to rebuild.
	ULevel* SourceLevel = nullptr;
	int SourceNodeCount = 0;

private:
	void AddSurfaces(UModel* model);
	void AddLights(ULevel* level);
};
