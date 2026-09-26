#pragma once

#include <functional>

class VulkanDevice;
class VulkanCommandBuffer;

// What the tracer's GPU side needs from whoever owns the Vulkan device: the
// device itself, and a way to run a command buffer to completion for uploads
// that happen once per level rather than once per frame.
//
// Frames overlap: the next is recorded while the last is still on the GPU. So
// anything that would change what a frame in flight reads - a buffer
// reallocated, a descriptor rewritten - first waits for the GPU with
// WaitForGpu. ExecuteImmediate waits for everything before it as well.
class GpuContext
{
public:
	// How many frames may be on the GPU at once: the one tracing, and the next
	// being recorded behind it. Everything a frame writes from the host comes
	// in this many copies, one per slot.
	static const int FramesInFlight = 2;

	virtual ~GpuContext() = default;
	virtual VulkanDevice* GetDevice() const = 0;
	virtual void ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn) = 0;
	virtual void WaitForGpu() = 0;
};
