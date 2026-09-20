#pragma once

#include <array>
#include <cstdint>

class UVulkanRenderDevice;

class CommandBufferManager
{
public:
	CommandBufferManager(UVulkanRenderDevice* renderer);
	~CommandBufferManager();

	void BeginFrame();
	void WaitForTransfer();
	void SubmitCommands(bool present, int presentWidth, int presentHeight, bool presentFullscreen);
	VulkanCommandBuffer* GetTransferCommands();
	VulkanCommandBuffer* GetDrawCommands();
	void DeleteFrameObjects();

	// Waits for the most recent present to reach the screen. Does nothing, and
	// says so, where the device has no VK_KHR_present_wait.
	bool WaitForLastPresent(std::chrono::steady_clock::duration timeout);

	struct DeleteList
	{
		std::vector<std::unique_ptr<VulkanImage>> images;
		std::vector<std::unique_ptr<VulkanImageView>> imageViews;
		std::vector<std::unique_ptr<VulkanBuffer>> buffers;
		std::vector<std::unique_ptr<VulkanDescriptorSet>> descriptors;
	};
	std::array<std::unique_ptr<DeleteList>, MAX_FRAMES_IN_FLIGHT> FrameDeleteLists;
	DeleteList* GetCurrentDeleteList() { return FrameDeleteLists[CurrentFrameIndex].get(); }

	// Which of this device's frame slots is being written. It used to be one
	// variable for the whole process, which held up for exactly as long as there
	// was one device: with two - the editor's viewport and a browser preview, or
	// the terrain editor - each of them advanced the other's, so a device would
	// pick the slot its own previous frame was still rendering from and start
	// overwriting its vertices. Both pictures come apart, and the more the GPU
	// has to do, the more often. It belongs to the device that waits on the
	// fences guarding it.
	uint32_t CurrentFrameIndex = 0;

	std::shared_ptr<VulkanSwapChain> SwapChain;
	int PresentImageIndex = -1;
	uint64_t PresentId = 0;
	BITFIELD UsingVsync = 0;
	BITFIELD UsingHdr = 0;
	VkFormat UsingSwapChainFormat = VK_FORMAT_UNDEFINED;

private:
	UVulkanRenderDevice* renderer = nullptr;

	std::array<std::unique_ptr<VulkanSemaphore>, MAX_FRAMES_IN_FLIGHT> ImageAvailableSemaphores;
	std::array<std::unique_ptr<VulkanSemaphore>, MAX_FRAMES_IN_FLIGHT> RenderFinishedSemaphores;
	std::array<std::unique_ptr<VulkanSemaphore>, MAX_FRAMES_IN_FLIGHT> DrawFinishedSemaphores;
	std::array<std::unique_ptr<VulkanSemaphore>, MAX_FRAMES_IN_FLIGHT> TransferSemaphores;
	std::array<std::unique_ptr<VulkanFence>, MAX_FRAMES_IN_FLIGHT> RenderFinishedFences;
	std::unique_ptr<VulkanCommandPool> CommandPool;
	std::array<std::unique_ptr<VulkanCommandBuffer>, MAX_FRAMES_IN_FLIGHT> DrawCommandsArray;
	std::array<std::unique_ptr<VulkanCommandBuffer>, MAX_FRAMES_IN_FLIGHT> TransferCommandsArray;

	bool FrameBegun = false;
	bool IsFirstFrame = true;
	std::array<bool, MAX_FRAMES_IN_FLIGHT> DrawCommandsBegun = {};
	std::array<bool, MAX_FRAMES_IN_FLIGHT> TransferCommandsBegun = {};
};
