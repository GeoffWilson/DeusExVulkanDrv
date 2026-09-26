#pragma once

#include "SceneData.h"
#include <memory>
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
	AccelStructure(GpuContext* renderer);
	~AccelStructure();

	// The host's half: a bottom level structure for any geometry that does not
	// have one yet, the shapes that animate and their shading data, the lights
	// and their grid, and the top level structure's instances.
	void Update(const SceneData& scene, FrameUploads& uploads);

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
	// considers those. Rebuilt with the light list every frame.
	void WriteLightGrid(const SceneData& scene, FrameUploads& uploads);
	std::unique_ptr<VulkanBuffer> LightGridBuffer;
	size_t LightGridCapacity = 0;
	std::vector<uint32_t> LightGrid;
	uint32_t LoggedGridCells = 0;
	size_t AttributeCapacity = 0;
	bool haveDynamic = false;
	bool attributesChanged = false;
	bool LoggedInstances = false;
};
