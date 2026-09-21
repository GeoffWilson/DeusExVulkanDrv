#pragma once

#include "LevelScene.h"
#include <memory>
#include <vector>

class UPathTracerRenderDevice;

// The scene as the ray tracing hardware wants it.
//
// One bottom level structure per distinct shape - the static world, each mover's
// brush, each mesh pose - and a top level structure rebuilt every frame from the
// placements. Bottom level structures are built once and kept: that is the whole
// point of instancing, and it is why a door costs a transform per frame rather
// than a rebuild.
class AccelStructure
{
public:
	AccelStructure(UPathTracerRenderDevice* renderer);
	~AccelStructure();

	// Builds bottom level structures for any geometry that does not have one
	// yet, and re-uploads the shading attributes if that added some.
	void SyncGeometry(const LevelScene& scene);

	// Rebuilt every frame, inside the frame's own command buffer.
	void BuildTopLevel(const LevelScene& scene, VulkanCommandBuffer* commands);

	void Reset();

	bool IsReady() const { return TopLevel != nullptr && !Bottom.empty(); }
	bool AttributesChanged() const { return attributesChanged; }
	void ClearAttributesChanged() { attributesChanged = false; }

	VulkanAccelerationStructure* GetTopLevel() const { return TopLevel.get(); }
	VulkanBuffer* GetAttributeBuffer() const { return AttributeBuffer.get(); }
	VulkanBuffer* GetLightBuffer() const { return LightBuffer.get(); }
	int LightCount() const { return Lights; }

	// Where each geometry's attributes begin, which is what an instance carries
	// as its custom index.
	uint32_t AttributeBase(int geometryIndex) const { return Bottom[geometryIndex].AttributeBase; }

private:
	struct BottomLevel
	{
		std::unique_ptr<VulkanBuffer> Vertices;
		std::unique_ptr<VulkanBuffer> Buffer;
		std::unique_ptr<VulkanAccelerationStructure> Structure;
		uint32_t AttributeBase = 0;
	};

	std::unique_ptr<VulkanBuffer> UploadBuffer(const void* data, size_t size, VkBufferUsageFlags usage, const char* debugName);
	void BuildBottomLevel(const SceneGeometry& geometry, BottomLevel& out);
	void EnsureTopLevelCapacity(size_t instanceCount);

	UPathTracerRenderDevice* renderer = nullptr;

	std::vector<BottomLevel> Bottom;
	std::vector<TriangleAttributes> AllAttributes;

	std::unique_ptr<VulkanBuffer> AttributeBuffer;
	std::unique_ptr<VulkanBuffer> LightBuffer;
	std::unique_ptr<VulkanBuffer> InstanceBuffer;
	std::unique_ptr<VulkanBuffer> TopBuffer;
	std::unique_ptr<VulkanBuffer> TopScratch;
	std::unique_ptr<VulkanAccelerationStructure> TopLevel;

	size_t TopCapacity = 0;
	int Lights = 0;
	bool attributesChanged = false;
};
