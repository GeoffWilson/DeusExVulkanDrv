#pragma once

#include "LevelScene.h"
#include <memory>

class UPathTracerRenderDevice;

// The scene as the ray tracing hardware wants it: a bottom level structure over
// the triangles, a top level structure holding one instance of it, and the
// shading data the trace shader reads by primitive index.
//
// Everything here is static for the lifetime of a level. Deus Ex's movers would
// need their own instances and a per frame top level rebuild; that is not done
// yet, so doors and lifts are traced where they stood when the level loaded.
class AccelStructure
{
public:
	AccelStructure(UPathTracerRenderDevice* renderer);
	~AccelStructure();

	// Throws on failure, like the rest of the Vulkan code here.
	void Build(const LevelScene& scene);

	bool IsBuilt() const { return TopLevel != nullptr; }

	VulkanAccelerationStructure* GetTopLevel() const { return TopLevel.get(); }
	VulkanBuffer* GetAttributeBuffer() const { return AttributeBuffer.get(); }
	VulkanBuffer* GetLightBuffer() const { return LightBuffer.get(); }
	int LightCount() const { return Lights; }

private:
	void Reset();

	// Uploads through a staging buffer and returns the device local result.
	std::unique_ptr<VulkanBuffer> UploadBuffer(const void* data, size_t size, VkBufferUsageFlags usage, const char* debugName);

	UPathTracerRenderDevice* renderer = nullptr;

	std::unique_ptr<VulkanBuffer> VertexBuffer;
	std::unique_ptr<VulkanBuffer> AttributeBuffer;
	std::unique_ptr<VulkanBuffer> LightBuffer;
	std::unique_ptr<VulkanBuffer> InstanceBuffer;

	std::unique_ptr<VulkanBuffer> BottomBuffer;
	std::unique_ptr<VulkanBuffer> TopBuffer;
	std::unique_ptr<VulkanAccelerationStructure> BottomLevel;
	std::unique_ptr<VulkanAccelerationStructure> TopLevel;

	int Lights = 0;
};
