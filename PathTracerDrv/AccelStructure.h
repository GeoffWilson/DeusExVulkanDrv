#pragma once

#include "SceneData.h"
#include <memory>
#include <utility>
#include <vector>

class GpuContext;
class FrameUploads;
class VulkanCommandBuffer;
class VulkanBuffer;
class VulkanAccelerationStructure;

// The scene as the ray tracing hardware wants it.
//
// One bottom level structure per distinct shape - the static world, each mover's
// brush, each mesh pose - and a top level structure rebuilt every frame from the
// placements. Bottom level structures are built once and kept: that is the whole
// point of instancing, and it is why a door costs a transform per frame rather
// than a rebuild.
//
// A frame is made in two halves. Update, on the host, stages everything the
// GPU will read in the frame's FrameUploads; Record, once those copies are
// recorded, builds the structures from them. Nothing the GPU reads is written
// from the host directly, because the frame before may still be reading it.
class AccelStructure
{
public:
	// The host's time writing the lights and their grid, summed until read.
	double LightsMs = 0.0;
	AccelStructure(GpuContext* renderer);
	~AccelStructure();

	// The host's half: a bottom level structure for any geometry that does not
	// have one yet, the shapes that animate and their shading data, the lights
	// and their grid, and the top level structure's instances.
	void Update(SceneData& scene, FrameUploads& uploads);

	// The GPU's half, after uploads is recorded: the bottom level builds
	// Update asked for, then the top level structure, rebuilt every frame.
	void Record(VulkanCommandBuffer* commands, FrameUploads& uploads);

	void Reset();

	// Diagnostic: give the static world a zero ray mask so nothing can hit it,
	// leaving only the instanced shapes. Answers "are they traced at all"
	// without relying on what the shader reads back from an intersection.
	bool HideStatic = false;


	bool IsReady() const { return TopLevel != nullptr && !Bottom.empty() && InstanceCount > 0; }
	bool AttributesChanged() const { return attributesChanged; }
	void ClearAttributesChanged() { attributesChanged = false; }

	VulkanAccelerationStructure* GetTopLevel() const { return TopLevel.get(); }
	VulkanBuffer* GetAttributeBuffer() const { return AttributeBuffer.get(); }
	VulkanBuffer* GetLightBuffer() const { return LightBuffer.get(); }
	VulkanBuffer* GetLightGridBuffer() const { return LightGridBuffer.get(); }
	VulkanBuffer* GetLightmapBuffer() const { return LightmapBuffer.get(); }
	VulkanBuffer* GetEmitterBuffer() const { return EmitterBuffer.get(); }
	// One entry per instance, indexed in the shader by the intersection's
	// instance id. Carries what varies by placement rather than by shape.
	VulkanBuffer* GetInstanceDataBuffer() const { return InstanceDataBuffer.get(); }
	int LightCount() const { return Lights; }
	int BottomCount() const { return (int)Bottom.size(); }

	// Where each geometry's attributes begin, which is what an instance carries
	// as its custom index.
	uint32_t AttributeBase(int geometryIndex) const { return Bottom[geometryIndex].AttributeBase; }

private:
	struct BottomLevel
	{
		std::unique_ptr<VulkanBuffer> Vertices;
		std::unique_ptr<VulkanBuffer> Buffer;
		std::unique_ptr<VulkanAccelerationStructure> Structure;
		std::unique_ptr<VulkanBuffer> Scratch;
		uint32_t AttributeBase = 0;
		// How many triangles' attributes that slot holds. A dynamic shape that
		// grows past it - decals being added, a character changing its skin -
		// moves to a bigger slot at the end rather than writing over the next.
		size_t AttributeSlots = 0;
		int TriangleCount = 0;
		// Rebuilt whenever its pose changes rather than built once and
		// instanced. An animated character's shape genuinely changes.
		bool Dynamic = false;
		bool Opaque = true;
		size_t VertexCapacity = 0;
		// Which version of the scene's geometry was last uploaded, and whether
		// the structure still has to be built from it. Something dynamic that
		// did not change this frame costs nothing.
		uint32_t WrittenVersion = 0;
		bool NeedsBuild = false;
	};

	void SyncGeometry(const SceneData& scene, FrameUploads& uploads);
	void CreateStaticBottomLevel(const SceneGeometry& geometry, BottomLevel& out, FrameUploads& uploads);
	void CreateDynamicBottomLevel(const SceneGeometry& geometry, BottomLevel& out);
	void RetireBottomLevel(BottomLevel& level, FrameUploads& uploads);
	void WriteDynamicGeometry(const SceneData& scene, FrameUploads& uploads);
	void WriteLights(const SceneData& scene, FrameUploads& uploads);
	void WriteInstances(const SceneData& scene, FrameUploads& uploads);
	void RecordBottomLevelBuilds(VulkanCommandBuffer* commands, FrameUploads& uploads);
	bool EnsureAttributeCapacity(size_t count, FrameUploads& uploads);
	void EnsureTopLevelCapacity(size_t instanceCount, FrameUploads& uploads);

	GpuContext* renderer = nullptr;

	std::vector<BottomLevel> Bottom;
	std::vector<TriangleAttributes> AllAttributes;

	std::unique_ptr<VulkanBuffer> AttributeBuffer;
	std::unique_ptr<VulkanBuffer> LightBuffer;
	std::unique_ptr<VulkanBuffer> InstanceBuffer;
	std::unique_ptr<VulkanBuffer> InstanceDataBuffer;
	std::unique_ptr<VulkanBuffer> TopBuffer;
	std::unique_ptr<VulkanBuffer> TopScratch;
	std::unique_ptr<VulkanAccelerationStructure> TopLevel;

	size_t TopCapacity = 0;
	size_t InstanceCount = 0;
	int Lights = 0;
	size_t LightCapacity = 0;

	// Which lights can reach which part of the level, so a shaded point only
	// considers those, ranked by what each could give there. Built again when
	// a light moves, changes its reach or how bright it gets, or comes or
	// goes; GridInputs, Cylinders and Powers are what it was last built from.
	void WriteLightGrid(const SceneData& scene, FrameUploads& uploads);
	std::unique_ptr<VulkanBuffer> LightGridBuffer;
	size_t LightGridCapacity = 0;
	std::vector<uint32_t> LightGrid;
	std::vector<vec4> GridInputs, LastGridInputs;
	std::vector<uint8_t> Cylinders, LastCylinders;
	std::vector<float> Powers, LastPowers;
	// The most lights at the head of a cell the shader weighs exactly
	// (directLight's exactCount): as many are kept in order, heaviest first.
	static const uint32_t ExactLights = 8;
	// Kept from one build to the next, so a light moving every frame does
	// not allocate the grid's worth again every frame.
	struct GridEntry { uint32_t Cell; float Rank; uint32_t Light; };
	std::vector<GridEntry> GridEntries;
	std::vector<uint32_t> GridCounts, GridFill;
	std::vector<std::pair<float, uint32_t>> GridRanked;
	uint32_t LoggedGridCells = 0;

	// The engine's shadow masks (SceneData's Lightmaps), written when a level
	// sends them. Never empty: a level without them has a count of none.
	void WriteLightmaps(SceneData& scene, FrameUploads& uploads);
	std::unique_ptr<VulkanBuffer> LightmapBuffer;
	size_t LightmapCapacity = 0;

	// The level's glowing surfaces (SceneData's Emitters), written when a
	// level sends them, each record's geometry and triangle turned into the
	// triangle's attribute index. Never empty: a level without any has a
	// count of none.
	void WriteEmitters(SceneData& scene, FrameUploads& uploads);
	std::unique_ptr<VulkanBuffer> EmitterBuffer;
	size_t EmitterCapacity = 0;
	size_t AttributeCapacity = 0;
	bool haveDynamic = false;
	bool attributesChanged = false;
	bool LoggedInstances = false;
};
