// Deliberately without the render device's precompiled header: this file
// knows nothing of the engine, and the test harness builds it on its own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include "TraceClient.h"
#include <cstdio>
#include <cstring>
#ifdef PATHTRACER_LOCAL
#include "TraceHost.h"
#include <stdexcept>
#endif

// Room for a batch, in the game's 32-bit address space as well as the
// helper's. The static world has to fit in one command - 132 bytes a triangle,
// so a 100,000 triangle level is 13 MB - while textures and the rest simply
// take more batches.
static const uint32_t CommandCapacity = 64u << 20;

TraceClient::~TraceClient()
{
	Stop();
}

bool TraceClient::Die(const std::string& why)
{
	Dead = true;
	LastError = why;
	return false;
}

bool TraceClient::Start(VulkanDevice* device, const std::string& helperPath, const std::string& workingDirectory, bool vkDebug, int headset)
{
	Device = device;

	// Named for this process and this start, since the engine can make a new
	// render device mid session and the old helper may still be going.
	static int starts = 0;
	char name[128];
	snprintf(name, sizeof(name), "Local\\PathTracer.%lu.%d.", GetCurrentProcessId(), ++starts);

	const DWORD size = (DWORD)(sizeof(TraceProtocol::Header) + CommandCapacity);
	Mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, (std::string(name) + "Memory").c_str());
	RequestEvent = CreateEventA(nullptr, FALSE, FALSE, (std::string(name) + "Request").c_str());
	ReplyEvent = CreateEventA(nullptr, FALSE, FALSE, (std::string(name) + "Reply").c_str());
	if (!Mapping || !RequestEvent || !ReplyEvent)
		return Die("could not create the shared memory and events (" + std::to_string(GetLastError()) + ")");
	Shared = (TraceProtocol::Header*)MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!Shared)
		return Die("could not map the shared memory (" + std::to_string(GetLastError()) + ")");
	memset(Shared, 0, sizeof(*Shared));
	Shared->Magic = TraceProtocol::Magic;
	Shared->Version = TraceProtocol::Version;
	Shared->CommandCapacity = CommandCapacity;
	Commands = (uint8_t*)(Shared + 1);

	// The helper must choose this same GPU: the image they share only means
	// anything there.
	VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	props.pNext = &id;
	vkGetPhysicalDeviceProperties2(device->PhysicalDevice.Device, &props);
	char uuid[VK_UUID_SIZE * 2 + 1] = {};
	for (int b = 0; b < VK_UUID_SIZE; b++)
		snprintf(uuid + b * 2, 3, "%02x", id.deviceUUID[b]);

	char cmdline[MAX_PATH * 2 + 256];
	snprintf(cmdline, sizeof(cmdline), "\"%s\" --parent %lu --name %s --uuid %s%s%s",
		helperPath.c_str(), GetCurrentProcessId(), name, uuid, vkDebug ? " --vkdebug" : "",
		headset == 2 ? " --headset sim" : headset ? " --headset" : "");

	// Tied to this process: a job that kills it when the last handle to the
	// job closes, which the process's end does. The helper also watches for
	// this process to go, for wherever jobs are not honoured.
	Job = CreateJobObjectA(nullptr, nullptr);
	if (Job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessA(helperPath.c_str(), cmdline, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi))
		return Die("could not start " + helperPath + " (" + std::to_string(GetLastError()) + ")");
	if (Job)
		AssignProcessToJobObject(Job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	HelperProcess = pi.hProcess;

	HANDLE waits[2] = { ReplyEvent, HelperProcess };
	const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, TraceProtocol::StartupTimeoutMs);
	if (woke != WAIT_OBJECT_0)
		return Die(woke == WAIT_OBJECT_0 + 1 ? "the helper exited while starting - see PathTracerHelper.log" : "the helper did not start in time");
	if (Shared->Status != 0 || !Shared->Ready)
		return Die(std::string("the helper could not start: ") + Shared->Error);

	// The two semaphores, into this device.
	VkSemaphore* ours[2] = { &ReadySemaphore, &ReleasedSemaphore };
	const uint64_t theirs[2] = { Shared->ReadySemaphore, Shared->ReleasedSemaphore };
	for (int i = 0; i < 2; i++)
	{
		VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (vkCreateSemaphore(device->device, &info, nullptr, ours[i]) != VK_SUCCESS)
			return Die("could not create a semaphore to import into");
		VkImportSemaphoreWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
		import.semaphore = *ours[i];
		import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		import.handle = (HANDLE)(uintptr_t)theirs[i];
		if (vkImportSemaphoreWin32HandleKHR(device->device, &import) != VK_SUCCESS)
			return Die("could not import the helper's semaphores");
	}
	return true;
}

#ifdef PATHTRACER_LOCAL
bool TraceClient::StartLocal(VulkanDevice* device)
{
	Device = device;
	LocalMemory.assign(sizeof(TraceProtocol::Header) + CommandCapacity, 0);
	Shared = (TraceProtocol::Header*)LocalMemory.data();
	Shared->Magic = TraceProtocol::Magic;
	Shared->Version = TraceProtocol::Version;
	Shared->CommandCapacity = CommandCapacity;
	Commands = (uint8_t*)(Shared + 1);
	try
	{
		Local = new TraceHost(device, Shared, nullptr);
	}
	catch (const std::exception& e)
	{
		return Die(std::string("the tracer could not start: ") + e.what());
	}
	ReadySemaphore = Local->ReadySemaphoreHandle();
	ReleasedSemaphore = Local->ReleasedSemaphoreHandle();
	return true;
}
#endif

void TraceClient::Stop()
{
#ifdef PATHTRACER_LOCAL
	if (Local)
	{
		// The host's image and semaphores are its own, and go with it.
		delete Local;
		Local = nullptr;
		OutputImage = VK_NULL_HANDLE;
		ReadySemaphore = ReleasedSemaphore = VK_NULL_HANDLE;
		ImportedWidth = ImportedHeight = 0;
		Shared = nullptr;
		Commands = nullptr;
		LocalMemory.clear();
		LocalMemory.shrink_to_fit();
		return;
	}
#endif
	if (Shared && HelperProcess && !Dead)
	{
		// Asked to go; the job would see to it anyway once this process does.
		Used = 0;
		if (uint8_t* p = Reserve(sizeof(TraceProtocol::CommandHeader)))
		{
			TraceProtocol::CommandHeader quit = { TraceProtocol::CmdQuit, sizeof(TraceProtocol::CommandHeader) };
			memcpy(p, &quit, sizeof(quit));
			Send();
		}
	}
	if (HelperProcess)
	{
		if (WaitForSingleObject(HelperProcess, 3000) != WAIT_OBJECT_0)
			TerminateProcess(HelperProcess, 1);
		CloseHandle(HelperProcess);
		HelperProcess = nullptr;
	}
	if (Device)
	{
		ReleaseOutput();
		ReleaseHud();
		if (ReadySemaphore) vkDestroySemaphore(Device->device, ReadySemaphore, nullptr);
		if (ReleasedSemaphore) vkDestroySemaphore(Device->device, ReleasedSemaphore, nullptr);
		ReadySemaphore = ReleasedSemaphore = VK_NULL_HANDLE;
	}
	if (Shared) UnmapViewOfFile(Shared);
	if (Mapping) CloseHandle(Mapping);
	if (RequestEvent) CloseHandle(RequestEvent);
	if (ReplyEvent) CloseHandle(ReplyEvent);
	if (Job) CloseHandle(Job);
	Shared = nullptr;
	Mapping = RequestEvent = ReplyEvent = Job = nullptr;
}

// Room for a command in the current batch, sending the batch first if the
// command would not fit. Null if it could never fit, or the helper is gone.
uint8_t* TraceClient::Reserve(uint32_t bytes)
{
	if (!Alive())
		return nullptr;
	if (bytes > Shared->CommandCapacity)
	{
		LastError = "a command of " + std::to_string(bytes) + " bytes will not fit in the channel";
		RefusedCount++;
		return nullptr;
	}
	if (Used + bytes > Shared->CommandCapacity && !Send())
		return nullptr;
	uint8_t* p = Commands + Used;
	Used += bytes;
	return p;
}

// The batch as it stands, and the helper's answer to it.
bool TraceClient::Send()
{
	if (!Alive())
		return false;
	Shared->CommandBytes = Used;
	Shared->BatchSerial++;
	Used = 0;
#ifdef PATHTRACER_LOCAL
	if (Local)
	{
		try
		{
			Local->Batch(Commands, Shared->CommandBytes);
		}
		catch (const std::exception& e)
		{
			Shared->Status = 1;
			snprintf(Shared->Error, sizeof(Shared->Error), "%s", e.what());
		}
		Shared->ReplySerial = Shared->BatchSerial;
		if (Shared->Status != 0)
			return Die(std::string("the tracer failed: ") + Shared->Error);
		return true;
	}
#endif
	SetEvent(RequestEvent);

	HANDLE waits[2] = { ReplyEvent, HelperProcess };
	const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, TraceProtocol::BatchTimeoutMs);
	if (woke == WAIT_OBJECT_0 + 1)
		return Die("the helper has exited - see PathTracerHelper.log");
	if (woke != WAIT_OBJECT_0)
		return Die("the helper stopped answering");
	if (Shared->ReplySerial != Shared->BatchSerial)
		return Die("the helper answered out of turn");
	if (Shared->Status != 0)
		return Die(std::string("the helper failed: ") + Shared->Error);
	return true;
}

void TraceClient::ResetScene()
{
	using namespace TraceProtocol;
	if (uint8_t* p = Reserve(sizeof(CommandHeader)))
	{
		CommandHeader h = { CmdResetScene, sizeof(CommandHeader) };
		memcpy(p, &h, sizeof(h));
	}
}

void TraceClient::Geometry(uint32_t index, const SceneGeometry& geometry)
{
	using namespace TraceProtocol;
	const size_t positions = geometry.Positions.size() * sizeof(vec3);
	const size_t attributes = geometry.Attributes.size() * sizeof(TriangleAttributes);
	const uint32_t bytes = Rounded(sizeof(GeometryCommand) + positions + attributes);
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	GeometryCommand c = {};
	c.H = { CmdGeometry, bytes };
	c.Index = index;
	c.Dynamic = geometry.Dynamic ? 1 : 0;
	c.HasMasked = geometry.HasMasked ? 1 : 0;
	c.Version = geometry.Version;
	c.PositionCount = (uint32_t)geometry.Positions.size();
	c.TriangleCount = (uint32_t)geometry.Attributes.size();
	memcpy(p, &c, sizeof(c));
	memcpy(p + sizeof(c), geometry.Positions.data(), positions);
	memcpy(p + sizeof(c) + positions, geometry.Attributes.data(), attributes);
}

void TraceClient::Instances(const std::vector<SceneInstance>& instances, int staticGeometries)
{
	using namespace TraceProtocol;
	const uint32_t bytes = Rounded(sizeof(InstancesCommand) + instances.size() * sizeof(WireInstance));
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	InstancesCommand c = {};
	c.H = { CmdInstances, bytes };
	c.Count = (uint32_t)instances.size();
	c.StaticGeometries = (uint32_t)staticGeometries;
	memcpy(p, &c, sizeof(c));
	WireInstance* wire = (WireInstance*)(p + sizeof(c));
	for (size_t i = 0; i < instances.size(); i++)
	{
		WireInstance w = {};
		w.GeometryIndex = instances[i].GeometryIndex;
		w.HasPrevious = instances[i].HasPrevious ? 1 : 0;
		w.Mask = instances[i].Mask;
		memcpy(w.Transform, instances[i].Transform, sizeof(w.Transform));
		memcpy(w.PreviousTransform, instances[i].PreviousTransform, sizeof(w.PreviousTransform));
		w.Ambient = instances[i].Ambient;
		memcpy(wire + i, &w, sizeof(w));
	}
}

void TraceClient::Lights(const std::vector<SceneLight>& lights, const std::vector<SceneLight>& fogLights)
{
	using namespace TraceProtocol;
	const size_t count = lights.size() + fogLights.size();
	const uint32_t bytes = Rounded(sizeof(LightsCommand) + count * sizeof(SceneLight));
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	LightsCommand c = {};
	c.H = { CmdLights, bytes };
	c.LightCount = (uint32_t)lights.size();
	c.FogCount = (uint32_t)fogLights.size();
	memcpy(p, &c, sizeof(c));
	memcpy(p + sizeof(c), lights.data(), lights.size() * sizeof(SceneLight));
	memcpy(p + sizeof(c) + lights.size() * sizeof(SceneLight), fogLights.data(), fogLights.size() * sizeof(SceneLight));
}

void TraceClient::Lightmaps(const std::vector<uint32_t>& words)
{
	using namespace TraceProtocol;
	const uint32_t bytes = Rounded(sizeof(LightmapsCommand) + words.size() * sizeof(uint32_t));
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	LightmapsCommand c = {};
	c.H = { CmdLightmaps, bytes };
	c.Words = (uint32_t)words.size();
	memcpy(p, &c, sizeof(c));
	if (!words.empty())
		memcpy(p + sizeof(c), words.data(), words.size() * sizeof(uint32_t));
}

void TraceClient::Emitters(const std::vector<uint32_t>& words)
{
	using namespace TraceProtocol;
	const uint32_t bytes = Rounded(sizeof(EmittersCommand) + words.size() * sizeof(uint32_t));
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	EmittersCommand c = {};
	c.H = { CmdEmitters, bytes };
	c.Words = (uint32_t)words.size();
	memcpy(p, &c, sizeof(c));
	if (!words.empty())
		memcpy(p + sizeof(c), words.data(), words.size() * sizeof(uint32_t));
}

int TraceClient::Texture(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels, const vec4& material, bool animated, uint32_t levels, uint32_t format)
{
	using namespace TraceProtocol;
	if (!pixels)
		width = height = 0;
	if (!levels)
		levels = 1;

	// One too big to go in a piece - 4096 square, uncompressed, fills the
	// channel on its own - goes without its top levels until it fits: softer
	// rather than missing. The shader takes a texture's size from its image,
	// so it samples what is left at the right level. One that still does not
	// fit is sent empty, for the slot to be there and white, as one that
	// could not be converted is.
	int dropped = 0;
	auto pixelBytes = [&]() { return MipChainBytes(format, width, height, width && height ? levels : 0); };
	while (width && height && sizeof(TextureCommand) + pixelBytes() > CommandCapacity)
	{
		if (levels < 2)
		{
			width = height = 0;
			dropped = -1;
			break;
		}
		pixels = (const uint32_t*)((const uint8_t*)pixels + MipBytes(format, width, height));
		width = width > 1 ? width >> 1 : 1;
		height = height > 1 ? height >> 1 : 1;
		levels--;
		dropped++;
	}

	const size_t chainBytes = pixelBytes();
	const uint32_t bytes = Rounded(sizeof(TextureCommand) + chainBytes);
	uint8_t* p = Reserve(bytes);
	if (!p)
		return dropped;
	TextureCommand c = {};
	c.H = { CmdTexture, bytes };
	c.Index = index;
	c.Width = width;
	c.Height = height;
	c.Animated = animated ? 1 : 0;
	c.MipLevels = levels;
	c.Format = format;
	c.Material = material;
	memcpy(p, &c, sizeof(c));
	if (chainBytes)
		memcpy(p + sizeof(c), pixels, chainBytes);
	return dropped;
}

void TraceClient::TexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels)
{
	using namespace TraceProtocol;
	const size_t pixelBytes = (size_t)width * height * 4;
	const uint32_t bytes = Rounded(sizeof(TexturePixelsCommand) + pixelBytes);
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	TexturePixelsCommand c = {};
	c.H = { CmdTexturePixels, bytes };
	c.Index = index;
	c.Width = width;
	c.Height = height;
	memcpy(p, &c, sizeof(c));
	memcpy(p + sizeof(c), pixels, pixelBytes);
}

bool TraceClient::Flush()
{
	return Used == 0 || Send();
}

bool TraceClient::Trace(const TraceProtocol::TraceCommand& frame)
{
	using namespace TraceProtocol;
	uint8_t* p = Reserve(sizeof(TraceCommand));
	if (!p)
		return false;
	TraceCommand c = frame;
	c.H = { CmdTrace, (uint32_t)sizeof(TraceCommand) };
	memcpy(p, &c, sizeof(c));
	if (!Send() || !Shared->Traced)
		return false;
	if (Shared->OutputGeneration != ImportedGeneration && !ImportOutput())
		return false;
	if (Shared->HudGeneration != ImportedHudGeneration && !ImportHud())
		return false;
	return true;
}

void TraceClient::ReleaseOutput()
{
	if (OutputImage) vkDestroyImage(Device->device, OutputImage, nullptr);
	if (OutputMemory) vkFreeMemory(Device->device, OutputMemory, nullptr);
	OutputImage = VK_NULL_HANDLE;
	OutputMemory = VK_NULL_HANDLE;
	ImportedWidth = ImportedHeight = 0;
}

// The helper's new shared image, made here over the same memory. The device's
// last frame may still be copying out of the old one, so everything of this
// device's is waited for before it goes. A resize, not a per frame cost.
bool TraceClient::ImportOutput()
{
#ifdef PATHTRACER_LOCAL
	// Here already, and the host's to make and destroy.
	if (Local)
	{
		OutputImage = Local->OutputImage();
		ImportedGeneration = Shared->OutputGeneration;
		ImportedWidth = Shared->OutputWidth;
		ImportedHeight = Shared->OutputHeight;
		return OutputImage != VK_NULL_HANDLE;
	}
#endif
	vkDeviceWaitIdle(Device->device);
	ReleaseOutput();
	if (!ImportImage(TraceProtocol::OutputFormat, TraceProtocol::OutputUsage, Shared->OutputWidth, Shared->OutputHeight,
		Shared->OutputMemory, Shared->OutputAllocationSize, OutputImage, OutputMemory))
		return false;
	ImportedGeneration = Shared->OutputGeneration;
	ImportedWidth = Shared->OutputWidth;
	ImportedHeight = Shared->OutputHeight;
	return true;
}

void TraceClient::ReleaseHud()
{
	if (HudImage) vkDestroyImage(Device->device, HudImage, nullptr);
	if (HudMemory) vkFreeMemory(Device->device, HudMemory, nullptr);
	HudImage = VK_NULL_HANDLE;
	HudMemory = VK_NULL_HANDLE;
	HudImported[0] = HudImported[1] = 0;
}

// The HUD's image, likewise, when the helper has made it anew.
bool TraceClient::ImportHud()
{
	vkDeviceWaitIdle(Device->device);
	ReleaseHud();
	if (!ImportImage(TraceProtocol::HudFormat, TraceProtocol::HudUsage, Shared->HudWidth, Shared->HudHeight,
		Shared->HudMemory, Shared->HudAllocationSize, HudImage, HudMemory))
		return false;
	ImportedHudGeneration = Shared->HudGeneration;
	HudImported[0] = Shared->HudWidth;
	HudImported[1] = Shared->HudHeight;
	return true;
}

bool TraceClient::ImportImage(VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, uint64_t handle, uint64_t size, VkImage& image, VkDeviceMemory& memory)
{
	VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.pNext = &external;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { width, height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(Device->device, &info, nullptr, &image) != VK_SUCCESS)
		return Die("could not create the shared image here");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(Device->device, image, &requirements);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(Device->PhysicalDevice.Device, &properties);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
		if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		{
			type = i;
			break;
		}

	VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	dedicated.image = image;
	VkImportMemoryWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
	import.pNext = &dedicated;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	import.handle = (HANDLE)(uintptr_t)handle;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.pNext = &import;
	allocate.allocationSize = size;
	allocate.memoryTypeIndex = type;
	if (type == UINT32_MAX || vkAllocateMemory(Device->device, &allocate, nullptr, &memory) != VK_SUCCESS)
		return Die("could not import the shared image's memory");
	if (vkBindImageMemory(Device->device, image, memory, 0) != VK_SUCCESS)
		return Die("could not bind the shared image's memory");
	return true;
}
