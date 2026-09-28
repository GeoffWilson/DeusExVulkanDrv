#pragma once

#include "GpuContext.h"
#include "TraceProtocol.h"
#include <memory>

class TraceRenderer;
class VulkanDevice;
class VulkanCommandPool;
class VulkanCommandBuffer;
class VulkanFence;

// The tracing, wherever it runs: the scene as the device describes it, a
// frame traced when asked, and each frame copied into an image of its own and
// handed over with two semaphores. Batches of TraceProtocol commands are its
// only input, and the Header it is given is where it answers.
//
// It runs in one of two places. In PathTracerHelper.exe, for a 32-bit game,
// on the helper's own device: the image's memory and the semaphores are then
// exported to the game's process, and the image changes hands between the
// two devices as a transfer from VK_QUEUE_FAMILY_EXTERNAL. Or in a 64-bit
// game's own process, on the render device's device, where the device reads
// the image and waits on the semaphores as they are - see TraceClient's
// StartLocal.
class TraceHost : public GpuContext
{
public:
	// exportTo is the process the image and semaphores are handed to, or null
	// when this host is in that process and on its device. Throws when the
	// device cannot trace.
	TraceHost(VulkanDevice* device, TraceProtocol::Header* status, HANDLE exportTo);
	~TraceHost();

	VulkanDevice* GetDevice() const override { return Device; }
	void ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn) override;
	void WaitForGpu() override;

	// Applies one batch of commands, answering in the status header. False
	// once a batch has said to quit. Throws when anything goes wrong it cannot
	// carry on from.
	bool Batch(const uint8_t* commands, uint32_t bytes);

	// The frame's image and semaphores, for a host in the device's process:
	// the handles the header carries are only for the helper's.
	VkImage OutputImage() const { return SharedImage; }
	VkSemaphore ReadySemaphoreHandle() const { return ReadySemaphore; }
	VkSemaphore ReleasedSemaphoreHandle() const { return ReleasedSemaphore; }

private:
	VkSemaphore MakeSemaphore(uint64_t& handleInParent);
	void EnsureOutput(uint32_t width, uint32_t height);
	void DestroyOutput();
	void TraceFrame(const TraceProtocol::TraceCommand& frame);
	void WaitForSlot(int slot);

	VulkanDevice* Device = nullptr;
	TraceProtocol::Header* Shared = nullptr;
	HANDLE ExportTo = nullptr;
	std::unique_ptr<VulkanCommandPool> CommandPool;

	// A frame is recorded while the one before is still on the GPU, so each
	// of the frames in flight has its own fence and command buffers: the
	// trace, and the handoff that copies it into the shared image.
	struct FrameSlot
	{
		std::unique_ptr<VulkanFence> Fence;
		std::unique_ptr<VulkanCommandBuffer> Trace;
		std::unique_ptr<VulkanCommandBuffer> Handoff;
		bool Pending = false;
	};
	FrameSlot Slots[FramesInFlight];
	int NextSlot = 0;   // the next to record into: the oldest in flight

	// This batch's time waiting for the GPU, and how many of those waits were
	// for a frame still in flight rather than for a slot to come free.
	double BatchWaitMs = 0.0;
	uint32_t BatchStalls = 0;

	// The host's time on each frame by stage, logged every StageFrames with
	// LogTimings, a hundred times at most: the slot wait, the command buffer,
	// the recording, ending it, the handoff, the submission and the status
	// written back.
	double FrameStageMs[7] = {};
	int StagedFrames = 0, StagesLogged = 0;
	static const int StageFrames = 300;

	// Ready, signalled once a frame is in the shared image; Released, signalled
	// by the device once it has copied the frame out. Every Ready is answered
	// by one Released, which the next frame waits for before writing.
	VkSemaphore ReadySemaphore = VK_NULL_HANDLE;
	VkSemaphore ReleasedSemaphore = VK_NULL_HANDLE;
	bool AwaitingRelease = false;

	// The image the frame is handed over in, and its memory - exportable when
	// it goes to another process.
	VkImage SharedImage = VK_NULL_HANDLE;
	VkDeviceMemory SharedMemory = VK_NULL_HANDLE;
	uint32_t SharedWidth = 0, SharedHeight = 0;
	bool SharedFresh = true;

	std::unique_ptr<TraceRenderer> Renderer;
	bool Quit = false;
};
