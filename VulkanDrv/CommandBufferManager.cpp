
#include "Precomp.h"
#include "CommandBufferManager.h"
#include "UVulkanRenderDevice.h"

CommandBufferManager::CommandBufferManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
	SwapChain = VulkanSwapChainBuilder()
		.Create(renderer->Device.get());

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		ImageAvailableSemaphores[i] = SemaphoreBuilder()
			.DebugName("ImageAvailableSemaphore")
			.Create(renderer->Device.get());

		RenderFinishedSemaphores[i] = SemaphoreBuilder()
			.DebugName("RenderFinishedSemaphore")
			.Create(renderer->Device.get());

		DrawFinishedSemaphores[i] = SemaphoreBuilder()
			.DebugName("DrawFinishedSemaphores")
			.Create(renderer->Device.get());

		TransferSemaphores[i] = SemaphoreBuilder()
			.DebugName("TransferSemaphore")
			.Create(renderer->Device.get());

		RenderFinishedFences[i] = FenceBuilder()
			.DebugName("RenderFinishedFence")
			.Flags(VK_FENCE_CREATE_SIGNALED_BIT)
			.Create(renderer->Device.get());

		FrameDeleteLists[i] = std::make_unique<DeleteList>();
	}

	CommandPool = CommandPoolBuilder()
		.QueueFamily(renderer->Device.get()->GraphicsFamily)
		.DebugName("CommandPool")
		.Create(renderer->Device.get());
}

CommandBufferManager::~CommandBufferManager()
{
	DeleteFrameObjects();
}

void CommandBufferManager::BeginFrame()
{
	VkFence currentFence = RenderFinishedFences[CurrentFrameIndex]->fence;
	vkWaitForFences(renderer->Device.get()->device, 1, &currentFence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(renderer->Device.get()->device, 1, &currentFence);

	// Safely clear old Vulkan objects now that the GPU is 100% done with this frame index
	FrameDeleteLists[CurrentFrameIndex] = std::make_unique<DeleteList>();

	// Reset per-frame CPU write positions now that this frame index is safe to reuse
	renderer->Buffers->UploadBufferPositions[CurrentFrameIndex] = 0;

}

void CommandBufferManager::WaitForTransfer()
{
	renderer->Uploads->SubmitUploads();

	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];
	auto& RenderFinishedFence = RenderFinishedFences[CurrentFrameIndex];

	// Whether this frame began recording, not whether the slot has a buffer at
	// all - it keeps the one from last time round.
	if (TransferCommandsBegun[CurrentFrameIndex])
	{
		TransferCommands->end();

		QueueSubmit()
			.AddCommandBuffer(TransferCommands.get())
			.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue, RenderFinishedFence.get());
		vkWaitForFences(renderer->Device.get()->device, 1, &RenderFinishedFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(renderer->Device.get()->device, 1, &RenderFinishedFence->fence);

		TransferCommands->begin();
	}
}

void CommandBufferManager::SubmitCommands(bool present, int presentWidth, int presentHeight, bool presentFullscreen)
{
	renderer->Uploads->SubmitUploads();

	auto& ImageAvailableSemaphore = ImageAvailableSemaphores[CurrentFrameIndex];
	auto& RenderFinishedSemaphore = RenderFinishedSemaphores[CurrentFrameIndex];
	auto& TransferSemaphore = TransferSemaphores[CurrentFrameIndex];
	auto& RenderFinishedFence = RenderFinishedFences[CurrentFrameIndex];
	auto& DrawCommands = DrawCommandsArray[CurrentFrameIndex];
	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];

	if (present)
	{
		if (SwapChain->Lost() || SwapChain->Width() != presentWidth || SwapChain->Height() != presentHeight || UsingVsync != renderer->UseVSync || UsingHdr != renderer->Hdr)
		{
			UsingVsync = renderer->UseVSync;
			UsingHdr = renderer->Hdr;
			renderer->Framebuffers->DestroySwapChainFramebuffers();
			SwapChain->Create(presentWidth, presentHeight, renderer->UseVSync ? 2 : 3, renderer->UseVSync, renderer->Hdr, renderer->VkExclusiveFullscreen && presentFullscreen);

			// Turning HDR on or off changes the swapchain's pixel format, and a
			// render pass states the format of its attachment - so the present
			// pass and the pipelines built against it no longer match what they
			// would be rendering into. Rebuild them when, and only when, the
			// format actually moved. Other frames may still hold the old ones,
			// hence the idle: this happens on a settings change, not per frame.
			if (renderer->Hdr)
			{
				switch (SwapChain->Format().colorSpace)
				{
				case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
					debugf(TEXT("HDR output: scRGB, the compositor does the display encode"));
					break;
				case VK_COLOR_SPACE_HDR10_ST2084_EXT:
					debugf(TEXT("HDR output: HDR10, Rec.2020 primaries and the PQ curve encoded by the present shader"));
					break;
				default:
					// Say what was on offer instead. Without it there is no way
					// to tell a compositor that does not do HDR from a surface
					// that does but in a pairing this code did not look for.
					debugf(TEXT("HDR was requested but the surface offers neither scRGB nor HDR10 - staying SDR"));

					// A surface can only ever report an HDR colour space if this
					// instance extension went in. It is optional, so if the
					// loader did not have it we asked a question that could only
					// have one answer, and that is not the compositor's doing.
					{
						const std::set<std::string>& enabled = renderer->Device->Instance->EnabledExtensions;
						bool colorSpaceExt = enabled.find(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) != enabled.end();
						debugf(TEXT("  %s is %s"), appFromAnsi(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME), colorSpaceExt ? TEXT("enabled") : TEXT("NOT enabled - no HDR colour space can be reported without it"));
					}

					for (const VkSurfaceFormatKHR& f : SwapChain->AvailableFormats())
						debugf(TEXT("  surface offers format %d, color space %d"), (int)f.format, (int)f.colorSpace);
					break;
				}
			}

			VkFormat swapChainFormat = SwapChain->Format().format;
			if (swapChainFormat != UsingSwapChainFormat)
			{
				vkDeviceWaitIdle(renderer->Device->device);
				UsingSwapChainFormat = swapChainFormat;
				renderer->RenderPasses->CreatePresentRenderPass();
				renderer->RenderPasses->CreatePresentPipeline();
			}

			renderer->Framebuffers->CreateSwapChainFramebuffers();
		}

		PresentImageIndex = SwapChain->AcquireImage(ImageAvailableSemaphore.get());
		if (PresentImageIndex != -1)
		{
			renderer->DrawPresentTexture(presentWidth, presentHeight);
		}
	}

	// The slot's buffer outlives the frame that recorded it. Ending and
	// submitting it again on a frame that recorded nothing sends the previous
	// recording a second time - the copies of an earlier frame, reading staging
	// memory that has since been freed and writing into images that have since
	// been recreated. Both flags below say what THIS frame recorded.
	if (TransferCommandsBegun[CurrentFrameIndex])
	{
		TransferCommands->end();

		auto SubmitTransfer = QueueSubmit();
		SubmitTransfer.AddCommandBuffer(TransferCommands.get());
		SubmitTransfer.AddSignal(TransferSemaphore.get());

		if (!IsFirstFrame)
		{
			uint32_t PrevFrame = (CurrentFrameIndex + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
			SubmitTransfer.AddWait(VK_PIPELINE_STAGE_TRANSFER_BIT, DrawFinishedSemaphores[PrevFrame].get());
		}

		SubmitTransfer.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue);
	}

	if (DrawCommandsBegun[CurrentFrameIndex])
		DrawCommands->end();

	QueueSubmit submit;
	if (DrawCommandsBegun[CurrentFrameIndex])
	{
		submit.AddCommandBuffer(DrawCommands.get());
	}
	if (TransferCommandsBegun[CurrentFrameIndex])
	{
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, TransferSemaphore.get());
	}
	if (present && PresentImageIndex != -1)
	{
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ImageAvailableSemaphore.get());
		submit.AddSignal(RenderFinishedSemaphore.get());
	}
	submit.AddSignal(DrawFinishedSemaphores[CurrentFrameIndex].get());
	submit.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue, RenderFinishedFence.get());

	FrameBegun = false;
	IsFirstFrame = false;

	if (present && PresentImageIndex != -1)
	{
		// Ids start at one: zero means "no id" to the swap chain.
		SwapChain->QueuePresent(PresentImageIndex, RenderFinishedSemaphore.get(), ++PresentId);
	}

	// Advance frame index. NO vkWaitForFences here!
	CurrentFrameIndex = (CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
}

VulkanCommandBuffer* CommandBufferManager::GetTransferCommands()
{
	if (!FrameBegun)
	{
		BeginFrame();
		DrawCommandsBegun[CurrentFrameIndex] = false;
		TransferCommandsBegun[CurrentFrameIndex] = false;
		FrameBegun = true;
	}

	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];
	if (!TransferCommands)
		TransferCommands = CommandPool->createBuffer();

	if (!TransferCommandsBegun[CurrentFrameIndex])
	{
		TransferCommands->begin();
		TransferCommandsBegun[CurrentFrameIndex] = true;
	}
	return TransferCommands.get();
}

VulkanCommandBuffer* CommandBufferManager::GetDrawCommands()
{
	if (!FrameBegun)
	{
		BeginFrame();
		DrawCommandsBegun[CurrentFrameIndex] = false;
		TransferCommandsBegun[CurrentFrameIndex] = false;
		FrameBegun = true;
	}

	auto& DrawCommands = DrawCommandsArray[CurrentFrameIndex];
	if (!DrawCommands)
		DrawCommands = CommandPool->createBuffer();

	if (!DrawCommandsBegun[CurrentFrameIndex])
	{
		DrawCommands->begin();
		DrawCommandsBegun[CurrentFrameIndex] = true;
	}
	return DrawCommands.get();
}

void CommandBufferManager::DeleteFrameObjects()
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		FrameDeleteLists[i] = std::make_unique<DeleteList>();
}

bool CommandBufferManager::WaitForLastPresent(std::chrono::steady_clock::duration timeout)
{
	if (!SwapChain || PresentId == 0)
		return false;

	auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
	return SwapChain->WaitForPresent(PresentId, (uint64_t)std::max<int64_t>(nanoseconds, 0));
}
