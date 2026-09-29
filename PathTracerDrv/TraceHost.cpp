#include "TracePrecomp.h"
#include "TraceHost.h"
#include "TraceRenderer.h"
#include <chrono>
#include <stdexcept>

static double NowMs()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

TraceHost::TraceHost(VulkanDevice* device, TraceProtocol::Header* status, HANDLE exportTo)
	: Device(device), Shared(status), ExportTo(exportTo)
{
	const auto& props = Device->PhysicalDevice.Properties.Properties;
	snprintf(Shared->DeviceName, sizeof(Shared->DeviceName), "%s", props.deviceName);
	Shared->RayTracing = (Device->EnabledFeatures.RayQuery.rayQuery && Device->EnabledFeatures.AccelerationStructure.accelerationStructure) ? 1 : 0;
	HelperLog("%s, ray tracing %s", props.deviceName, Shared->RayTracing ? "offered" : "NOT offered");
	if (!Shared->RayTracing)
		throw std::runtime_error(ExportTo ? "the helper's device offers no ray tracing either" : "the device offers no ray tracing (VK_KHR_ray_query and VK_KHR_acceleration_structure)");

	CommandPool = CommandPoolBuilder()
		.QueueFamily(Device->GraphicsFamily)
		.DebugName("PathTracerHostCommandPool")
		.Create(Device);
	for (FrameSlot& slot : Slots)
		slot.Fence = FenceBuilder().DebugName("PathTracerHostFrame").Create(Device);

	ReadySemaphore = MakeSemaphore(Shared->ReadySemaphore);
	ReleasedSemaphore = MakeSemaphore(Shared->ReleasedSemaphore);

	HelperLog("compiling the trace");
	Renderer.reset(new TraceRenderer(this));
	Shared->CanSampleTextures = Renderer->SamplesTextures() ? 1 : 0;
	Shared->Ready = 1;
	HelperLog("ready");
}

TraceHost::~TraceHost()
{
	if (!Device)
		return;
	vkDeviceWaitIdle(Device->device);
	Renderer.reset();
	DestroyOutput();
	if (ReadySemaphore) vkDestroySemaphore(Device->device, ReadySemaphore, nullptr);
	if (ReleasedSemaphore) vkDestroySemaphore(Device->device, ReleasedSemaphore, nullptr);
	for (FrameSlot& slot : Slots)
		slot = FrameSlot();
	CommandPool.reset();
}

void TraceHost::ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn)
{
	auto commands = CommandPool->createBuffer();
	commands->begin();
	fn(commands.get());
	commands->end();

	auto fence = FenceBuilder().DebugName("PathTracerImmediate").Create(Device);
	QueueSubmit()
		.AddCommandBuffer(commands.get())
		.Execute(Device, Device->GraphicsQueue, fence.get());

	const double waitStart = NowMs();
	VkFence handle = fence->fence;
	vkWaitForFences(Device->device, 1, &handle, VK_TRUE, UINT64_MAX);
	BatchWaitMs += NowMs() - waitStart;
}

// Every frame in flight, oldest first. After a submission of its own has
// completed, which ExecuteImmediate's has, this finds them all done already.
void TraceHost::WaitForGpu()
{
	bool stalled = false;
	for (int i = 0; i < FramesInFlight; i++)
	{
		const int slot = (NextSlot + i) % FramesInFlight;
		if (Slots[slot].Pending)
		{
			stalled = true;
			WaitForSlot(slot);
		}
	}
	if (stalled)
		BatchStalls++;
}

// A semaphore the device waits on or signals. For the helper, exported, with
// its handle in the device's process; in the device's own process, plain.
VkSemaphore TraceHost::MakeSemaphore(uint64_t& handleInParent)
{
	handleInParent = 0;
	VkExportSemaphoreWin32HandleInfoKHR win32 = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
	win32.dwAccess = GENERIC_ALL;
	VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
	exportInfo.pNext = &win32;
	exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	info.pNext = ExportTo ? &exportInfo : nullptr;
	VkSemaphore semaphore = VK_NULL_HANDLE;
	if (vkCreateSemaphore(Device->device, &info, nullptr, &semaphore) != VK_SUCCESS)
		throw std::runtime_error(ExportTo ? "could not create an exportable semaphore" : "could not create a semaphore");
	if (!ExportTo)
		return semaphore;

	VkSemaphoreGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
	get.semaphore = semaphore;
	get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE local = nullptr;
	if (vkGetSemaphoreWin32HandleKHR(Device->device, &get, &local) != VK_SUCCESS)
		throw std::runtime_error("could not export a semaphore");
	HANDLE theirs = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), local, ExportTo, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
		throw std::runtime_error("could not hand a semaphore to the game's process");
	CloseHandle(local);
	handleInParent = (uint64_t)(uintptr_t)theirs;
	return semaphore;
}

void TraceHost::DestroyOutput()
{
	if (SharedImage) vkDestroyImage(Device->device, SharedImage, nullptr);
	if (SharedMemory) vkFreeMemory(Device->device, SharedMemory, nullptr);
	SharedImage = VK_NULL_HANDLE;
	SharedMemory = VK_NULL_HANDLE;
	SharedWidth = SharedHeight = 0;
}

// The image the device presents from, made again whenever the trace size
// changes. For the helper its memory goes to the device's process as a
// handle; the device makes the same image over it, which is why both follow
// TraceProtocol's format and usage exactly.
void TraceHost::EnsureOutput(uint32_t width, uint32_t height)
{
	if (SharedImage && width == SharedWidth && height == SharedHeight)
		return;

	// The device may still be copying the last frame out of the old one,
	// on its own queue. In this process that queue is the same device's, and
	// waiting for the device covers it. The helper's wait covers only its own
	// queue: what keeps the old image alive there is that the device imported
	// its memory, and memory imported is not released until every process
	// holding it has freed it - which the device does in ImportOutput, once
	// its own queue is idle. So the old image is safe to free here only while
	// the frame is shared as memory handed over like that.
	if (ExportTo)
		WaitForGpu();
	else
		vkDeviceWaitIdle(Device->device);
	DestroyOutput();

	VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.pNext = ExportTo ? &external : nullptr;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = TraceProtocol::OutputFormat;
	info.extent = { width, height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = TraceProtocol::OutputUsage;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(Device->device, &info, nullptr, &SharedImage) != VK_SUCCESS)
		throw std::runtime_error("could not create the shared image");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(Device->device, SharedImage, &requirements);
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(Device->PhysicalDevice.Device, &memory);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		{
			type = i;
			break;
		}

	VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	dedicated.image = SharedImage;
	VkExportMemoryWin32HandleInfoKHR win32 = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
	win32.pNext = &dedicated;
	win32.dwAccess = GENERIC_ALL;
	VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
	exportInfo.pNext = &win32;
	exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.pNext = ExportTo ? (const void*)&exportInfo : (const void*)&dedicated;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (type == UINT32_MAX || vkAllocateMemory(Device->device, &allocate, nullptr, &SharedMemory) != VK_SUCCESS)
		throw std::runtime_error("could not allocate the shared image's memory");
	vkBindImageMemory(Device->device, SharedImage, SharedMemory, 0);

	Shared->OutputMemory = 0;
	if (ExportTo)
	{
		VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
		get.memory = SharedMemory;
		get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		HANDLE local = nullptr;
		if (vkGetMemoryWin32HandleKHR(Device->device, &get, &local) != VK_SUCCESS)
			throw std::runtime_error("could not export the shared image's memory");
		HANDLE theirs = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), local, ExportTo, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
			throw std::runtime_error("could not hand the shared image to the game's process");
		CloseHandle(local);
		Shared->OutputMemory = (uint64_t)(uintptr_t)theirs;
	}

	SharedWidth = width;
	SharedHeight = height;
	SharedFresh = true;
	Shared->OutputAllocationSize = requirements.size;
	Shared->OutputWidth = width;
	Shared->OutputHeight = height;
	Shared->OutputGeneration++;
	HelperLog("shared image %ux%u, %llu bytes, generation %u", width, height,
		(unsigned long long)requirements.size, Shared->OutputGeneration);
}

void TraceHost::WaitForSlot(int slot)
{
	FrameSlot& frame = Slots[slot];
	if (!frame.Pending)
		return;
	const double waitStart = NowMs();
	VkFence handle = frame.Fence->fence;
	vkWaitForFences(Device->device, 1, &handle, VK_TRUE, UINT64_MAX);
	vkResetFences(Device->device, 1, &handle);
	BatchWaitMs += NowMs() - waitStart;
	frame.Pending = false;
	frame.Trace.reset();
	frame.Handoff.reset();
	Renderer->FrameCompleted(slot);
}

// The frame, recorded into the oldest slot while the frame before it may still
// be tracing, then a copy of it into the shared image, handed over on the GPU:
// Ready once it is there, and the copy itself waiting on Released, which the
// device signals once it has taken the last one out. The copy is a submission
// of its own so that only it waits on the device, not the frame's uploads and
// trace. Between two processes the image also changes hands as a transfer
// from and to VK_QUEUE_FAMILY_EXTERNAL; in one, on one queue, it has no hands
// to change.
void TraceHost::TraceFrame(const TraceProtocol::TraceCommand& frame)
{
	double lapStart = NowMs();
	auto lap = [&](int stage)
	{
		const double t = NowMs();
		FrameStageMs[stage] += t - lapStart;
		lapStart = t;
	};
	const int slotIndex = NextSlot;
	WaitForSlot(slotIndex);
	FrameSlot& slot = Slots[slotIndex];
	lap(0);

	Renderer->SetAnisotropy(frame.MaxAnisotropy);
	slot.Trace = CommandPool->createBuffer();
	slot.Trace->begin();
	lap(1);
	const bool traced = Renderer->Record(slot.Trace.get(), frame, slotIndex);
	lap(2);
	slot.Trace->end();
	lap(3);

	if (traced)
	{
		EnsureOutput((uint32_t)Renderer->Width(), (uint32_t)Renderer->Height());
		slot.Handoff = CommandPool->createBuffer();
		slot.Handoff->begin();
		VkCommandBuffer commands = slot.Handoff->buffer;
		VkImage output = Renderer->Output()->image;
		const uint32_t family = (uint32_t)Device->GraphicsFamily;
		const bool handOver = ExportTo && !SharedFresh;

		VkImageMemoryBarrier barriers[2] = {};
		for (auto& b : barriers)
		{
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		}
		barriers[0].image = output;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		// The shared image comes back from the device's queue, except the
		// first time, when it has never been anywhere.
		barriers[1].image = SharedImage;
		barriers[1].oldLayout = SharedFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].srcQueueFamilyIndex = handOver ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = handOver ? family : VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);

		VkImageCopy copy = {};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = { SharedWidth, SharedHeight, 1 };
		vkCmdCopyImage(commands, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SharedImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// Back to GENERAL for the next frame's trace, and over to the device.
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].srcQueueFamilyIndex = ExportTo ? family : VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = ExportTo ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
		barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].dstAccessMask = 0;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);
		SharedFresh = false;
		slot.Handoff->end();
	}

	lap(4);
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	const bool waitRelease = traced && AwaitingRelease;
	VkSubmitInfo submits[2] = {};
	submits[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submits[0].commandBufferCount = 1;
	submits[0].pCommandBuffers = &slot.Trace->buffer;
	if (traced)
	{
		submits[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submits[1].waitSemaphoreCount = waitRelease ? 1 : 0;
		submits[1].pWaitSemaphores = &ReleasedSemaphore;
		submits[1].pWaitDstStageMask = &waitStage;
		submits[1].commandBufferCount = 1;
		submits[1].pCommandBuffers = &slot.Handoff->buffer;
		submits[1].signalSemaphoreCount = 1;
		submits[1].pSignalSemaphores = &ReadySemaphore;
	}
	if (vkQueueSubmit(Device->GraphicsQueue, traced ? 2 : 1, submits, slot.Fence->fence) != VK_SUCCESS)
		throw std::runtime_error("vkQueueSubmit failed");
	lap(5);
	slot.Pending = true;
	NextSlot = (slotIndex + 1) % FramesInFlight;
	if (waitRelease)
		AwaitingRelease = false;
	if (traced)
		AwaitingRelease = true;

	Shared->Traced = traced ? 1 : 0;
	// The GPU's time on the last frame, read when this batch waited for it.
	Shared->GpuTimed = Renderer->GpuTimed ? 1 : 0;
	Shared->GpuBuildMs = Renderer->GpuMs[0];
	Shared->GpuTraceMs = Renderer->GpuMs[1];
	Shared->GpuDenoiseMs = Renderer->GpuMs[2];
	Shared->GpuCompositeMs = Renderer->GpuMs[3];
	Shared->LightCount = (uint32_t)Renderer->LightCount();
	Shared->TextureCount = (uint32_t)Renderer->TextureCount();
	Shared->BottomCount = (uint32_t)Renderer->BottomCount();
	Shared->InstanceCount = (uint32_t)Renderer->Scene.Instances.size();
	Shared->DenoiserActive = Renderer->DenoiserActive() ? 1 : 0;
	snprintf(Shared->DenoiserStatus, sizeof(Shared->DenoiserStatus), "%s", Renderer->DenoiserStatus());
	Shared->RenderWidth = (uint32_t)Renderer->RenderWidth();
	Shared->RenderHeight = (uint32_t)Renderer->RenderHeight();
	Shared->DenoisedWith = Renderer->DenoisedWith();
	snprintf(Shared->DlssStatus, sizeof(Shared->DlssStatus), "%s", Renderer->DlssStatus());
	lap(6);

	// Where the host's time goes, which the device's timings see only as
	// one figure.
	if (++StagedFrames >= StageFrames)
	{
		const double n = StagedFrames;
		const double* r = Renderer->RecordStageMs;
		if (frame.Timing && StagesLogged < 100)
		{
			StagesLogged++;
			HelperLog("host ms/frame: slot wait %.2f begin %.2f record %.2f (setup %.2f shapes %.2f lights %.2f motion %.2f uploads %.2f builds %.2f descriptors %.2f trace %.2f denoise %.2f insets %.2f) end %.2f handoff %.2f submit %.2f status %.2f | %d instances, %d lights",
				FrameStageMs[0] / n, FrameStageMs[1] / n, FrameStageMs[2] / n,
				r[0] / n, r[1] / n, r[2] / n, r[3] / n, r[4] / n, r[5] / n, r[6] / n, r[7] / n, r[8] / n, r[9] / n,
				FrameStageMs[3] / n, FrameStageMs[4] / n, FrameStageMs[5] / n, FrameStageMs[6] / n,
				(int)Renderer->Scene.Instances.size(), (int)Renderer->Scene.Lights.size());
		}
		for (double& ms : FrameStageMs)
			ms = 0.0;
		for (double& ms : Renderer->RecordStageMs)
			ms = 0.0;
		StagedFrames = 0;
	}
}

// Applies one batch, without waiting for the frames in flight: the scene the
// commands change is the host's copy, which the GPU never reads. What does
// change something a frame in flight reads - a new level, a new texture -
// waits for them itself.
bool TraceHost::Batch(const uint8_t* commands, uint32_t total)
{
	using namespace TraceProtocol;
	const double batchStart = NowMs();
	BatchWaitMs = 0.0;
	BatchStalls = 0;
	double recordMs = 0.0, recordWaitMs = 0.0;
	Shared->Traced = 0;

	uint32_t offset = 0;
	while (offset + sizeof(CommandHeader) <= total)
	{
		CommandHeader header;
		memcpy(&header, commands + offset, sizeof(header));
		if (header.Bytes < sizeof(CommandHeader) || offset + header.Bytes > total)
			throw std::runtime_error("a malformed command");
		const uint8_t* body = commands + offset;

		switch (header.Type)
		{
		case CmdResetScene:
			Renderer->ResetScene();
			break;

		case CmdGeometry:
		{
			GeometryCommand g;
			memcpy(&g, body, sizeof(g));
			SceneData& scene = Renderer->Scene;
			if (g.Index >= scene.Geometries.size())
				scene.Geometries.resize(g.Index + 1);
			SceneGeometry& geometry = scene.Geometries[g.Index];
			geometry.Dynamic = g.Dynamic != 0;
			geometry.HasMasked = g.HasMasked != 0;
			geometry.Version = g.Version;
			geometry.Positions.resize(g.PositionCount);
			geometry.Attributes.resize(g.TriangleCount);
			const uint8_t* data = body + sizeof(g);
			memcpy(geometry.Positions.data(), data, g.PositionCount * sizeof(vec3));
			memcpy(geometry.Attributes.data(), data + g.PositionCount * sizeof(vec3), g.TriangleCount * sizeof(TriangleAttributes));
			scene.GeometryAdded = true;
			break;
		}

		case CmdInstances:
		{
			InstancesCommand c;
			memcpy(&c, body, sizeof(c));
			SceneData& scene = Renderer->Scene;
			scene.StaticGeometries = (int)c.StaticGeometries;
			scene.Instances.resize(c.Count);
			const WireInstance* wire = (const WireInstance*)(body + sizeof(c));
			for (uint32_t i = 0; i < c.Count; i++)
			{
				WireInstance w;
				memcpy(&w, wire + i, sizeof(w));
				SceneInstance& instance = scene.Instances[i];
				instance.GeometryIndex = w.GeometryIndex;
				memcpy(instance.Transform, w.Transform, sizeof(w.Transform));
				memcpy(instance.PreviousTransform, w.PreviousTransform, sizeof(w.PreviousTransform));
				instance.HasPrevious = w.HasPrevious != 0;
				instance.Mask = w.Mask;
				instance.Ambient = w.Ambient;
			}
			break;
		}

		case CmdLights:
		{
			LightsCommand c;
			memcpy(&c, body, sizeof(c));
			SceneData& scene = Renderer->Scene;
			const SceneLight* lights = (const SceneLight*)(body + sizeof(c));
			scene.Lights.assign(lights, lights + c.LightCount);
			scene.FogLights.assign(lights + c.LightCount, lights + c.LightCount + c.FogCount);
			break;
		}

		case CmdLightmaps:
		{
			LightmapsCommand c;
			memcpy(&c, body, sizeof(c));
			const uint32_t* words = (const uint32_t*)(body + sizeof(c));
			Renderer->Scene.Lightmaps.assign(words, words + c.Words);
			Renderer->Scene.LightmapsChanged = true;
			break;
		}

		case CmdEmitters:
		{
			EmittersCommand c;
			memcpy(&c, body, sizeof(c));
			const uint32_t* words = (const uint32_t*)(body + sizeof(c));
			Renderer->Scene.Emitters.assign(words, words + c.Words);
			Renderer->Scene.EmittersChanged = true;
			break;
		}

		case CmdTexture:
		{
			TextureCommand c;
			memcpy(&c, body, sizeof(c));
			const uint32_t levels = std::max(std::min(c.MipLevels, 16u), 1u);
			const uint32_t format = c.Format == TextureBc1 ? TextureBc1 : TextureRgba8;
			const bool hasPixels = c.Width && c.Height && header.Bytes >= sizeof(c) + MipChainBytes(format, c.Width, c.Height, levels);
			Renderer->SetTexture(c.Index, c.Width, c.Height, levels, format, hasPixels ? (const uint32_t*)(body + sizeof(c)) : nullptr, c.Material);
			break;
		}

		case CmdTexturePixels:
		{
			TexturePixelsCommand c;
			memcpy(&c, body, sizeof(c));
			if (header.Bytes >= sizeof(c) + (size_t)c.Width * c.Height * 4)
				Renderer->SetTexturePixels(c.Index, c.Width, c.Height, (const uint32_t*)(body + sizeof(c)));
			break;
		}

		case CmdTrace:
		{
			TraceCommand c;
			memcpy(&c, body, sizeof(c));
			const double traceStart = NowMs(), waitBefore = BatchWaitMs;
			TraceFrame(c);
			recordMs += NowMs() - traceStart;
			recordWaitMs += BatchWaitMs - waitBefore;
			break;
		}

		case CmdQuit:
			Quit = true;
			break;
		}

		offset += header.Bytes;
	}

	const double batchMs = NowMs() - batchStart;
	Shared->HelperWaitMs = (float)BatchWaitMs;
	Shared->HelperRecordMs = (float)(recordMs - recordWaitMs);
	Shared->HelperApplyMs = (float)std::max(batchMs - recordMs - (BatchWaitMs - recordWaitMs), 0.0);
	Shared->HelperStalls = BatchStalls;
	return !Quit;
}
