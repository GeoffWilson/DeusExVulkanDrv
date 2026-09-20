#pragma once

#include "vulkandevice.h"
#include "vulkanobjects.h"

class VulkanSemaphore;
class VulkanFence;

class VulkanSurfaceCapabilities
{
public:
	VkSurfaceCapabilitiesKHR Capabilites = { };
#ifdef WIN32
	VkSurfaceCapabilitiesFullScreenExclusiveEXT FullScreenExclusive = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT };
#else
	struct { void* pNext = nullptr; VkBool32 fullScreenExclusiveSupported = 0; } FullScreenExclusive;
#endif
	std::vector<VkPresentModeKHR> PresentModes;
	std::vector<VkSurfaceFormatKHR> Formats;
};

class VulkanSwapChain
{
public:
	VulkanSwapChain(VulkanDevice* device);
	~VulkanSwapChain();

	void Create(int width, int height, int imageCount, bool vsync, bool hdr, bool exclusivefullscreen);
	bool Lost() const { return lost; }

	int Width() const { return actualExtent.width; }
	int Height() const { return actualExtent.height; }
	VkSurfaceFormatKHR Format() const { return format; }

	// Every format/colour space pair the surface offered at the last Create.
	// Kept so a caller can say what it had to choose between, which is the only
	// way to tell "the compositor does not offer HDR" from "we asked wrongly".
	const std::vector<VkSurfaceFormatKHR>& AvailableFormats() const { return availableFormats; }

	int ImageCount() const { return (int)images.size(); }
	VulkanImage* GetImage(int index) { return images[index].get(); }
	VulkanImageView* GetImageView(int index) { return views[index].get(); }

	int AcquireImage(VulkanSemaphore* semaphore = nullptr, VulkanFence* fence = nullptr);
	void QueuePresent(int imageIndex, VulkanSemaphore* semaphore = nullptr, uint64_t presentId = 0);

	// Blocks until the present carrying this id has actually reached the screen.
	// Returns false if the device cannot tell us - no VK_KHR_present_wait, or
	// the wait timed out - so a caller can fall back to its own timing.
	bool WaitForPresent(uint64_t presentId, uint64_t timeoutNanoseconds);
	bool SupportsPresentWait() const;

private:
	void SelectFormat(const VulkanSurfaceCapabilities& caps, bool hdr);

	bool CreateSwapchain(int width, int height, int imageCount, bool vsync, bool hdr, bool exclusivefullscreen);

	VulkanSurfaceCapabilities GetSurfaceCapabilities(bool exclusivefullscreen);

	VulkanDevice* device = nullptr;
	bool lost = true;

	VkExtent2D actualExtent = {};
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkSurfaceFormatKHR format = {};
	std::vector<VkSurfaceFormatKHR> availableFormats;
	VkPresentModeKHR presentMode;
	std::vector<std::unique_ptr<VulkanImage>> images;
	std::vector<std::unique_ptr<VulkanImageView>> views;

	VulkanSwapChain(const VulkanSwapChain&) = delete;
	VulkanSwapChain& operator=(const VulkanSwapChain&) = delete;
};
