#include "TracePrecomp.h"
#include "TraceProtocol.h"
#include "TraceHost.h"
#include "RayReconstruction.h"
#include "HeadsetOutput.h"
#include <cstdarg>
#include <numeric>
#include <stdexcept>

// PathTracerHelper.exe: the 64-bit half of the path tracer.
//
// Started by the render device in the game's 32-bit process, on the same GPU,
// because only a 64-bit client is offered ray tracing everywhere but upstream
// wine. It keeps a copy of the scene the device sends, traces it when asked,
// and hands each frame back through an image and two semaphores the two
// processes share. See TraceProtocol.h for the channel between them.
//
//   PathTracerHelper.exe --parent <pid> --name <base> --uuid <hex> [--vkdebug] [--headset [sim]]
//
// --headset opens a session with the OpenXR runtime for the frames the device
// asks to have traced for it (see HeadsetOutput); "sim" is the test harness's
// simulated one.
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

// The helper's own part: the channel to the game's process and a device on
// the same GPU. The tracing is TraceHost's, which hands each frame to that
// process through memory and semaphores it exports there.
class Helper
{
public:
	~Helper();

	int Run(DWORD parentPid, const std::string& name, const std::string& uuid, bool vkDebug, int headset);

private:
	bool OpenChannel(DWORD parentPid, const std::string& name);
	void CreateDevice(const std::string& uuid, bool vkDebug);
	void StartHeadset();
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
	// Made before the instance when asked for, since the runtime has
	// extensions of its own to ask for; its session once the device is made.
	std::unique_ptr<HeadsetOutput> Headset;
	std::vector<std::string> HeadsetInstanceExtensions, HeadsetDeviceExtensions;
	bool HeadsetStarted = false;
	std::unique_ptr<TraceHost> Host;
};

Helper::~Helper()
{
	Host.reset();
	Headset.reset();
	Device.reset();
	Instance.reset();
	if (Shared) UnmapViewOfFile(Shared);
	if (Mapping) CloseHandle(Mapping);
	if (RequestEvent) CloseHandle(RequestEvent);
	if (ReplyEvent) CloseHandle(ReplyEvent);
	if (Parent) CloseHandle(Parent);
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

// An extension name Proton's wineopenxr gives in place of the runtime's own
// list - VK_WINE_openxr_instance_extensions, VK_WINE_openxr_device_extensions
// - which winevulkan expands as the instance or the device is made.
static bool IsPlaceholder(const std::string& name)
{
	return name.compare(0, 8, "VK_WINE_") == 0;
}

// What a placeholder stands for, as Proton wrote it into the registry as the
// game started, in the Windows names this side asks by: the instance's list,
// or a GPU's by its PCI ids. winevulkan expands the placeholder itself under
// Proton's X11 driver but not under its Wayland one (PROTON_ENABLE_WAYLAND),
// where a device asked for it is refused, so the helper does it. Empty where
// there is no such value.
static std::vector<std::string> ProtonExtensions(const char* value)
{
	char list[4096] = {};
	DWORD size = sizeof(list) - 1;
	std::vector<std::string> names;
	if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Wine\\VR", value, RRF_RT_REG_SZ, nullptr, list, &size) != ERROR_SUCCESS)
		return names;
	std::string all = list;
	for (size_t start = 0; start < all.size();)
	{
		size_t end = all.find(' ', start);
		if (end == std::string::npos)
			end = all.size();
		if (end > start)
			names.push_back(all.substr(start, end - start));
		start = end + 1;
	}
	return names;
}

// The list with any placeholder in it replaced by what it stands for, where
// the registry says; kept as it is, to be handed through, where it does not.
static void ExpandPlaceholders(std::vector<std::string>& names, const char* value)
{
	std::vector<std::string> expanded;
	for (const std::string& name : names)
	{
		std::vector<std::string> real = IsPlaceholder(name) ? ProtonExtensions(value) : std::vector<std::string>();
		if (real.empty())
			expanded.push_back(name);
		else
		{
			HelperLog("headset: %s stands for%s", name.c_str(), std::accumulate(real.begin(), real.end(), std::string(),
				[](const std::string& a, const std::string& b) { return a + " " + b; }).c_str());
			expanded.insert(expanded.end(), real.begin(), real.end());
		}
	}
	names.swap(expanded);
}

// The same GPU as the device, by UUID: the image they share is only
// meaningful on the one that made it.
void Helper::CreateDevice(const std::string& uuid, bool vkDebug)
{
	// DLSS Ray Reconstruction's extensions have to be asked for now, before
	// there is a device - and so before anyone has asked for it - or not at
	// all. Wherever they are offered they are enabled.
	std::vector<std::string> ngxInstance, ngxDevice;
	RayReconstruction::RequiredExtensions(ngxInstance, ngxDevice);

	ExpandPlaceholders(HeadsetInstanceExtensions, "openxr_vulkan_instance_extensions");
	HelperLog("creating the Vulkan instance");
	VulkanInstanceBuilder instanceBuilder;
	instanceBuilder.DebugLayer(vkDebug);
	for (const std::string& name : ngxInstance)
		instanceBuilder.OptionalExtension(name);
	// The headset's too, only ever optional: where one is not offered, the
	// headset goes without and the screen carries on. Under Proton the
	// runtime names a placeholder instead, which winevulkan replaces with
	// the real ones as the instance and the device are made, and which
	// nothing offers by that name: it is handed through as it is.
	for (const std::string& name : HeadsetInstanceExtensions)
	{
		if (IsPlaceholder(name))
			instanceBuilder.RequireExtension(name);
		else
			instanceBuilder.OptionalExtension(name);
	}
	Instance = instanceBuilder.Create();
	HelperLog("instance created, %d physical devices", (int)Instance->PhysicalDevices.size());

	// The device's placeholder stands for the list Proton wrote for this GPU,
	// found by its PCI ids.
	for (const VulkanPhysicalDevice& physical : Instance->PhysicalDevices)
	{
		VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		props.pNext = &id;
		vkGetPhysicalDeviceProperties2(physical.Device, &props);
		char hex[VK_UUID_SIZE * 2 + 1] = {};
		for (int b = 0; b < VK_UUID_SIZE; b++)
			snprintf(hex + b * 2, 3, "%02x", id.deviceUUID[b]);
		if (uuid != hex)
			continue;
		char value[32];
		snprintf(value, sizeof(value), "PCIID:%04x:%04x", props.properties.vendorID, props.properties.deviceID);
		ExpandPlaceholders(HeadsetDeviceExtensions, value);
	}

	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.OptionalDescriptorIndexing();
	builder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	builder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
	for (const std::string& name : ngxDevice)
		builder.OptionalExtension(name);
	for (const std::string& name : HeadsetDeviceExtensions)
	{
		if (IsPlaceholder(name))
			builder.PassThroughExtension(name);
		else
			builder.OptionalExtension(name);
	}

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
}

// The headset's session on the device just made: on this GPU, with what it
// asked of Vulkan. Anything missing leaves it without a session - the device
// is told why - and the helper traces for the screen as ever.
void Helper::StartHeadset()
{
	if (!Headset || !HeadsetStarted)
		return;
	for (const std::string& name : HeadsetInstanceExtensions)
		if (!IsPlaceholder(name) && !Instance->EnabledExtensions.count(name))
		{
			Headset->Fail("Vulkan here does not offer the headset's " + name);
			return;
		}
	for (const std::string& name : HeadsetDeviceExtensions)
		if (!IsPlaceholder(name) && !Device->EnabledDeviceExtensions.count(name))
		{
			Headset->Fail("the GPU does not offer the headset's " + name);
			return;
		}
	if (!Headset->UsesDevice(Instance->Instance, Device->PhysicalDevice.Device))
	{
		Headset->Fail(Headset->Status());
		return;
	}
	try
	{
		if (!Headset->CreateSession(Device.get()))
			Headset->Fail(Headset->Status());
	}
	catch (const std::exception& e)
	{
		Headset->Fail(std::string("the headset could not be set up: ") + e.what());
	}
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

int Helper::Run(DWORD parentPid, const std::string& name, const std::string& uuid, bool vkDebug, int headset)
{
	if (!OpenChannel(parentPid, name))
		return 2;

	// The runtime first, for the extensions it wants. Not finding one is not
	// a failure: the device is told why, and traces for the screen.
	if (headset)
	{
		Headset.reset(new HeadsetOutput(headset == 2));
		HeadsetStarted = Headset->Start();
		if (HeadsetStarted)
		{
			Headset->RequiredExtensions(HeadsetInstanceExtensions, HeadsetDeviceExtensions);
			std::string instance, device;
			for (const std::string& name : HeadsetInstanceExtensions)
				instance += " " + name;
			for (const std::string& name : HeadsetDeviceExtensions)
				device += " " + name;
			HelperLog("headset: Vulkan wanted, instance:%s; device:%s", instance.empty() ? " none" : instance.c_str(), device.empty() ? " none" : device.c_str());
		}
		else
			HelperLog("no headset: %s", Headset->Status());
	}

	try
	{
		// The headset's extensions are the one thing here that might stop
		// Vulkan starting at all, and the screen does not need them.
		try
		{
			CreateDevice(uuid, vkDebug);
		}
		catch (const std::exception& e)
		{
			if (!HeadsetStarted || (HeadsetInstanceExtensions.empty() && HeadsetDeviceExtensions.empty()))
				throw;
			HelperLog("headset: Vulkan would not start with the headset's extensions (%s); starting without them", e.what());
			Headset->Fail(std::string("Vulkan would not start with the headset's extensions: ") + e.what());
			HeadsetStarted = false;
			HeadsetInstanceExtensions.clear();
			HeadsetDeviceExtensions.clear();
			Device.reset();
			Instance.reset();
			CreateDevice(uuid, vkDebug);
		}
		StartHeadset();
		Host.reset(new TraceHost(Device.get(), Shared, Parent, Headset.get()));
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
			const bool carryOn = Host->Batch(Commands, std::min(Shared->CommandBytes, Shared->CommandCapacity));
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
	int headset = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--parent") && i + 1 < argc) parent = (DWORD)strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--uuid") && i + 1 < argc) uuid = argv[++i];
		else if (!strcmp(argv[i], "--vkdebug")) vkDebug = true;
		else if (!strcmp(argv[i], "--headset"))
		{
			headset = 1;
			if (i + 1 < argc && !strcmp(argv[i + 1], "sim"))
			{
				headset = 2;
				i++;
			}
		}
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
		result = helper.Run(parent, name, uuid, vkDebug, headset);
	}
	HelperLog("exiting, %d", result);
	if (LogFile)
		fclose(LogFile);
	return result;
}
