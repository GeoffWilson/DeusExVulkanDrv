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

bool TraceClient::Start(VulkanDevice* device, const std::string& helperPath, const std::string& workingDirectory, bool vkDebug)
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
	snprintf(cmdline, sizeof(cmdline), "\"%s\" --parent %lu --name %s --uuid %s%s",
		helperPath.c_str(), GetCurrentProcessId(), name, uuid, vkDebug ? " --vkdebug" : "");

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

void TraceClient::Stop()
{
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

void TraceClient::Texture(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels, const vec4& material, bool animated)
{
	using namespace TraceProtocol;
	if (!pixels)
		width = height = 0;
	const size_t pixelBytes = (size_t)width * height * 4;
	const uint32_t bytes = Rounded(sizeof(TextureCommand) + pixelBytes);
	uint8_t* p = Reserve(bytes);
	if (!p)
		return;
	TextureCommand c = {};
	c.H = { CmdTexture, bytes };
	c.Index = index;
	c.Width = width;
	c.Height = height;
	c.Animated = animated ? 1 : 0;
	c.Material = material;
	memcpy(p, &c, sizeof(c));
	if (pixelBytes)
		memcpy(p + sizeof(c), pixels, pixelBytes);
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

// The helper's new shared image, made here over the same memory. Only ever
// called with nothing of this device's in flight - the render device waits
// for its last frame before it asks for the next - so the old one can go.
bool TraceClient::ImportOutput()
{
	vkDeviceWaitIdle(Device->device);
	ReleaseOutput();

	const uint32_t width = Shared->OutputWidth, height = Shared->OutputHeight;
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
	if (vkCreateImage(Device->device, &info, nullptr, &OutputImage) != VK_SUCCESS)
		return Die("could not create the shared image here");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(Device->device, OutputImage, &requirements);
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
	dedicated.image = OutputImage;
	VkImportMemoryWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
	import.pNext = &dedicated;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	import.handle = (HANDLE)(uintptr_t)Shared->OutputMemory;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.pNext = &import;
	allocate.allocationSize = Shared->OutputAllocationSize;
	allocate.memoryTypeIndex = type;
	if (type == UINT32_MAX || vkAllocateMemory(Device->device, &allocate, nullptr, &OutputMemory) != VK_SUCCESS)
		return Die("could not import the shared image's memory");
	if (vkBindImageMemory(Device->device, OutputImage, OutputMemory, 0) != VK_SUCCESS)
		return Die("could not bind the shared image's memory");

	ImportedGeneration = Shared->OutputGeneration;
	ImportedWidth = width;
	ImportedHeight = height;
	return true;
}
