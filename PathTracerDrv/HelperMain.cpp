#include "TracePrecomp.h"
#include "TraceProtocol.h"
#include "TraceRenderer.h"
#include <cstdarg>
#include <stdexcept>

// PathTracerHelper.exe: the 64-bit half of the path tracer.
//
// Started by the render device in the game's 32-bit process, on the same GPU,
// because only a 64-bit client is offered ray tracing everywhere but upstream
// wine. It keeps a copy of the scene the device sends, traces it when asked,
// and hands each frame back through an image and two semaphores the two
// processes share. See TraceProtocol.h for the channel between them.
//
//   PathTracerHelper.exe --parent <pid> --name <base> --uuid <hex> [--vkdebug]
//
// It exits when told to, when anything goes wrong it cannot carry on from, or
// when the game's process goes away.

static FILE* LogFile = nullptr;

void HelperLog(const char* format, ...)
{
	if (!LogFile)
		return;
	// The engine's %S - a narrow string in a wide format - is %s here.
	std::string fixed = format;
	for (size_t i = 0; (i = fixed.find("%S", i)) != std::string::npos; i += 2)
		fixed[i + 1] = 's';
	SYSTEMTIME now = {};
	GetLocalTime(&now);
	fprintf(LogFile, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
	va_list args;
	va_start(args, format);
	vfprintf(LogFile, fixed.c_str(), args);
	va_end(args);
	fprintf(LogFile, "\n");
	fflush(LogFile);
}

// ZVulkan's two hooks into its user.
void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	HelperLog("[%s] %s", typestr, msg.c_str());
}

void VulkanError(const char* text)
{
	throw std::runtime_error(text);
}

class Helper : public GpuContext
{
public:
	~Helper();

	int Run(DWORD parentPid, const std::string& name, const std::string& uuid, bool vkDebug);

	VulkanDevice* GetDevice() const override { return Device.get(); }
	void ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn) override;

private:
	bool OpenChannel(DWORD parentPid, const std::string& name);
	void CreateDevice(const std::string& uuid, bool vkDebug);
	VkSemaphore ExportSemaphore(uint64_t& handleInParent);
	void EnsureOutput(uint32_t width, uint32_t height);
	void DestroyOutput();
	bool Batch();
	void TraceFrame(const TraceProtocol::TraceCommand& frame);
	void WaitForFrame();
	void Fail(const char* what);
	void Reply();

	HANDLE Parent = nullptr;
	HANDLE Mapping = nullptr;
	HANDLE RequestEvent = nullptr;
	HANDLE ReplyEvent = nullptr;
	TraceProtocol::Header* Shared = nullptr;
	uint8_t* Commands = nullptr;

	std::shared_ptr<VulkanInstance> Instance;
	std::shared_ptr<VulkanDevice> Device;
	std::unique_ptr<VulkanCommandPool> CommandPool;
	std::unique_ptr<VulkanFence> FrameFence;
	std::unique_ptr<VulkanCommandBuffer> FrameCommands;
	bool FramePending = false;

	// Ready, signalled once a frame is in the shared image; Released, signalled
	// by the device once it has copied the frame out. Every Ready is answered
	// by one Released, which the next frame waits for before writing.
	VkSemaphore ReadySemaphore = VK_NULL_HANDLE;
	VkSemaphore ReleasedSemaphore = VK_NULL_HANDLE;
	bool AwaitingRelease = false;

	// The shared image and its exportable memory.
	VkImage SharedImage = VK_NULL_HANDLE;
	VkDeviceMemory SharedMemory = VK_NULL_HANDLE;
	uint32_t SharedWidth = 0, SharedHeight = 0;
	bool SharedFresh = true;

	std::unique_ptr<TraceRenderer> Renderer;
	bool Quit = false;
};

Helper::~Helper()
{
	if (Device)
	{
		vkDeviceWaitIdle(Device->device);
		Renderer.reset();
		DestroyOutput();
		if (ReadySemaphore) vkDestroySemaphore(Device->device, ReadySemaphore, nullptr);
		if (ReleasedSemaphore) vkDestroySemaphore(Device->device, ReleasedSemaphore, nullptr);
		FrameCommands.reset();
		FrameFence.reset();
		CommandPool.reset();
	}
	Device.reset();
	Instance.reset();
	if (Shared) UnmapViewOfFile(Shared);
	if (Mapping) CloseHandle(Mapping);
	if (RequestEvent) CloseHandle(RequestEvent);
	if (ReplyEvent) CloseHandle(ReplyEvent);
	if (Parent) CloseHandle(Parent);
}

void Helper::ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn)
{
	auto commands = CommandPool->createBuffer();
	commands->begin();
	fn(commands.get());
	commands->end();

	auto fence = FenceBuilder().DebugName("PathTracerImmediate").Create(Device.get());
	QueueSubmit()
		.AddCommandBuffer(commands.get())
		.Execute(Device.get(), Device->GraphicsQueue, fence.get());

	VkFence handle = fence->fence;
	vkWaitForFences(Device->device, 1, &handle, VK_TRUE, UINT64_MAX);
}

bool Helper::OpenChannel(DWORD parentPid, const std::string& name)
{
	Parent = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, parentPid);
	Mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, (name + "Memory").c_str());
	RequestEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + "Request").c_str());
	ReplyEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + "Reply").c_str());
	if (!Parent || !Mapping || !RequestEvent || !ReplyEvent)
	{
		HelperLog("could not open the channel %s (parent %p, memory %p, events %p %p, error %lu)",
			name.c_str(), Parent, Mapping, RequestEvent, ReplyEvent, GetLastError());
		return false;
	}
	Shared = (TraceProtocol::Header*)MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!Shared || Shared->Magic != TraceProtocol::Magic || Shared->Version != TraceProtocol::Version)
	{
		HelperLog("the channel is not one this helper understands");
		return false;
	}
	Commands = (uint8_t*)(Shared + 1);
	HelperLog("channel %s open", name.c_str());
	return true;
}

// The same GPU as the device, by UUID: the image they share is only
// meaningful on the one that made it.
void Helper::CreateDevice(const std::string& uuid, bool vkDebug)
{
	HelperLog("creating the Vulkan instance");
	Instance = VulkanInstanceBuilder()
		.DebugLayer(vkDebug)
		.Create();
	HelperLog("instance created, %d physical devices", (int)Instance->PhysicalDevices.size());

	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.OptionalDescriptorIndexing();
	builder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	builder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);

	std::vector<VulkanCompatibleDevice> devices = builder.FindDevices(Instance);
	int chosen = -1;
	for (size_t i = 0; i < devices.size(); i++)
	{
		VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		props.pNext = &id;
		vkGetPhysicalDeviceProperties2(devices[i].Device->Device, &props);
		char hex[VK_UUID_SIZE * 2 + 1] = {};
		for (int b = 0; b < VK_UUID_SIZE; b++)
			snprintf(hex + b * 2, 3, "%02x", id.deviceUUID[b]);
		HelperLog("device %d: %s, UUID %s", (int)i, props.properties.deviceName, hex);
		if (chosen < 0 && uuid == hex)
			chosen = (int)i;
	}
	if (chosen < 0)
		throw std::runtime_error("no device here has the game's GPU's UUID, or it cannot share memory with another process");
	builder.SelectDevice(chosen);
	Device = builder.Create(Instance);
	HelperLog("device created");

	CommandPool = CommandPoolBuilder()
		.QueueFamily(Device->GraphicsFamily)
		.DebugName("PathTracerHelperCommandPool")
		.Create(Device.get());
	FrameFence = FenceBuilder().DebugName("PathTracerHelperFrame").Create(Device.get());
}

// A semaphore the device's process can wait on or signal, and its handle
// there.
VkSemaphore Helper::ExportSemaphore(uint64_t& handleInParent)
{
	VkExportSemaphoreWin32HandleInfoKHR win32 = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
	win32.dwAccess = GENERIC_ALL;
	VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
	exportInfo.pNext = &win32;
	exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	info.pNext = &exportInfo;
	VkSemaphore semaphore = VK_NULL_HANDLE;
	if (vkCreateSemaphore(Device->device, &info, nullptr, &semaphore) != VK_SUCCESS)
		throw std::runtime_error("could not create an exportable semaphore");

	VkSemaphoreGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
	get.semaphore = semaphore;
	get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE local = nullptr;
	if (vkGetSemaphoreWin32HandleKHR(Device->device, &get, &local) != VK_SUCCESS)
		throw std::runtime_error("could not export a semaphore");
	HANDLE theirs = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), local, Parent, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
		throw std::runtime_error("could not hand a semaphore to the game's process");
	CloseHandle(local);
	handleInParent = (uint64_t)(uintptr_t)theirs;
	return semaphore;
}

void Helper::DestroyOutput()
{
	if (SharedImage) vkDestroyImage(Device->device, SharedImage, nullptr);
	if (SharedMemory) vkFreeMemory(Device->device, SharedMemory, nullptr);
	SharedImage = VK_NULL_HANDLE;
	SharedMemory = VK_NULL_HANDLE;
	SharedWidth = SharedHeight = 0;
}

// The image the device presents from, made again whenever the trace size
// changes. Its memory goes to the device's process as a handle; the device
// makes the same image over it, which is why both follow TraceProtocol's
// format and usage exactly.
void Helper::EnsureOutput(uint32_t width, uint32_t height)
{
	if (SharedImage && width == SharedWidth && height == SharedHeight)
		return;

	vkDeviceWaitIdle(Device->device);
	DestroyOutput();

	VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.pNext = &external;
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
	allocate.pNext = &exportInfo;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (type == UINT32_MAX || vkAllocateMemory(Device->device, &allocate, nullptr, &SharedMemory) != VK_SUCCESS)
		throw std::runtime_error("could not allocate the shared image's memory");
	vkBindImageMemory(Device->device, SharedImage, SharedMemory, 0);

	VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
	get.memory = SharedMemory;
	get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE local = nullptr;
	if (vkGetMemoryWin32HandleKHR(Device->device, &get, &local) != VK_SUCCESS)
		throw std::runtime_error("could not export the shared image's memory");
	HANDLE theirs = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), local, Parent, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
		throw std::runtime_error("could not hand the shared image to the game's process");
	CloseHandle(local);

	SharedWidth = width;
	SharedHeight = height;
	SharedFresh = true;
	Shared->OutputMemory = (uint64_t)(uintptr_t)theirs;
	Shared->OutputAllocationSize = requirements.size;
	Shared->OutputWidth = width;
	Shared->OutputHeight = height;
	Shared->OutputGeneration++;
	HelperLog("shared image %ux%u, %llu bytes, generation %u", width, height,
		(unsigned long long)requirements.size, Shared->OutputGeneration);
}

void Helper::WaitForFrame()
{
	if (!FramePending)
		return;
	FramePending = false;
	VkFence handle = FrameFence->fence;
	vkWaitForFences(Device->device, 1, &handle, VK_TRUE, UINT64_MAX);
	vkResetFences(Device->device, 1, &handle);
	FrameCommands.reset();
	Renderer->FrameCompleted();
}

// The frame, then a copy of it into the shared image, handed over to the
// device's process on the GPU: Ready once it is there, and the copy itself
// waiting on Released, which the device signals once it has taken the last
// one out.
void Helper::TraceFrame(const TraceProtocol::TraceCommand& frame)
{
	FrameCommands = CommandPool->createBuffer();
	FrameCommands->begin();
	VulkanCommandBuffer* commands = FrameCommands.get();

	const bool traced = Renderer->Record(commands, frame);
	if (traced)
	{
		EnsureOutput((uint32_t)Renderer->Width(), (uint32_t)Renderer->Height());
		VkImage output = Renderer->Output()->image;
		const uint32_t family = (uint32_t)Device->GraphicsFamily;

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
		barriers[1].srcQueueFamilyIndex = SharedFresh ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
		barriers[1].dstQueueFamilyIndex = SharedFresh ? VK_QUEUE_FAMILY_IGNORED : family;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);

		VkImageCopy copy = {};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = { SharedWidth, SharedHeight, 1 };
		vkCmdCopyImage(commands->buffer, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SharedImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// Back to GENERAL for the next frame's trace, and over to the device.
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].srcQueueFamilyIndex = family;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].dstAccessMask = 0;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);
		SharedFresh = false;
	}
	FrameCommands->end();

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	const bool waitRelease = traced && AwaitingRelease;
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.waitSemaphoreCount = waitRelease ? 1 : 0;
	submit.pWaitSemaphores = &ReleasedSemaphore;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commands->buffer;
	submit.signalSemaphoreCount = traced ? 1 : 0;
	submit.pSignalSemaphores = &ReadySemaphore;
	if (vkQueueSubmit(Device->GraphicsQueue, 1, &submit, FrameFence->fence) != VK_SUCCESS)
		throw std::runtime_error("vkQueueSubmit failed");
	FramePending = true;
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
}

void Helper::Fail(const char* what)
{
	HelperLog("FAILED: %s", what);
	Shared->Status = 1;
	snprintf(Shared->Error, sizeof(Shared->Error), "%s", what);
}

void Helper::Reply()
{
	Shared->ReplySerial = Shared->BatchSerial;
	SetEvent(ReplyEvent);
}

// Applies one batch. The last frame must be finished with first: a batch can
// replace geometry and textures the last frame was reading, and a frame reuses
// the buffers the last one filled.
bool Helper::Batch()
{
	using namespace TraceProtocol;
	WaitForFrame();
	Shared->Traced = 0;

	const uint32_t total = std::min(Shared->CommandBytes, Shared->CommandCapacity);
	uint32_t offset = 0;
	while (offset + sizeof(CommandHeader) <= total)
	{
		CommandHeader header;
		memcpy(&header, Commands + offset, sizeof(header));
		if (header.Bytes < sizeof(CommandHeader) || offset + header.Bytes > total)
			throw std::runtime_error("a malformed command");
		const uint8_t* body = Commands + offset;

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

		case CmdTexture:
		{
			TextureCommand c;
			memcpy(&c, body, sizeof(c));
			const bool hasPixels = c.Width && c.Height && header.Bytes >= sizeof(c) + (size_t)c.Width * c.Height * 4;
			Renderer->SetTexture(c.Index, c.Width, c.Height, hasPixels ? (const uint32_t*)(body + sizeof(c)) : nullptr, c.Material);
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
			TraceFrame(c);
			break;
		}

		case CmdQuit:
			Quit = true;
			break;
		}

		offset += header.Bytes;
	}
	return !Quit;
}

int Helper::Run(DWORD parentPid, const std::string& name, const std::string& uuid, bool vkDebug)
{
	if (!OpenChannel(parentPid, name))
		return 2;

	try
	{
		CreateDevice(uuid, vkDebug);
		const auto& props = Device->PhysicalDevice.Properties.Properties;
		snprintf(Shared->DeviceName, sizeof(Shared->DeviceName), "%s", props.deviceName);
		Shared->RayTracing = (Device->EnabledFeatures.RayQuery.rayQuery && Device->EnabledFeatures.AccelerationStructure.accelerationStructure) ? 1 : 0;
		HelperLog("%s, ray tracing %s", props.deviceName, Shared->RayTracing ? "offered" : "NOT offered");
		if (!Shared->RayTracing)
			throw std::runtime_error("the helper's device offers no ray tracing either");

		ReadySemaphore = ExportSemaphore(Shared->ReadySemaphore);
		ReleasedSemaphore = ExportSemaphore(Shared->ReleasedSemaphore);

		HelperLog("compiling the trace");
		Renderer.reset(new TraceRenderer(this));
		Shared->CanSampleTextures = Renderer->SamplesTextures() ? 1 : 0;
		Shared->Ready = 1;
		HelperLog("ready");
	}
	catch (const std::exception& e)
	{
		Fail(e.what());
		Reply();
		return 1;
	}
	Reply();

	for (;;)
	{
		HANDLE waits[2] = { RequestEvent, Parent };
		const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (woke != WAIT_OBJECT_0)
		{
			HelperLog("the game's process has gone");
			return 0;
		}
		try
		{
			const bool carryOn = Batch();
			Reply();
			if (!carryOn)
			{
				HelperLog("told to quit");
				return 0;
			}
		}
		catch (const std::exception& e)
		{
			Fail(e.what());
			Reply();
			return 1;
		}
	}
}

int main(int argc, char** argv)
{
	DWORD parent = 0;
	std::string name, uuid;
	bool vkDebug = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--parent") && i + 1 < argc) parent = (DWORD)strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--uuid") && i + 1 < argc) uuid = argv[++i];
		else if (!strcmp(argv[i], "--vkdebug")) vkDebug = true;
	}

	LogFile = fopen("PathTracerHelper.log", "w");
	HelperLog("PathTracerHelper, %d-bit, for process %lu", (int)(sizeof(void*) * 8), parent);
	if (!parent || name.empty() || uuid.empty())
	{
		HelperLog("started without a game to trace for");
		return 2;
	}

	int result;
	{
		Helper helper;
		result = helper.Run(parent, name, uuid, vkDebug);
	}
	HelperLog("exiting, %d", result);
	if (LogFile)
		fclose(LogFile);
	return result;
}
