#pragma once

#include <functional>

class VulkanDevice;
class VulkanCommandBuffer;

// What the tracer's GPU side needs from whoever owns the Vulkan device: the
// device itself, and a way to run a command buffer to completion for uploads
// and structure builds that happen once per level rather than once per frame.
class GpuContext
{
public:
	virtual ~GpuContext() = default;
	virtual VulkanDevice* GetDevice() const = 0;
	virtual void ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn) = 0;
};
