// Can a 32-bit process show what a 64-bit process ray traced, frame after
// frame, without copying it through the CPU?
//
// The native 32-bit Windows ICD offers no ray tracing (see vkrtcheck), but the
// 64-bit one does. A path traced render device could keep reading the level in
// the game's 32-bit process and hand the tracing to a 64-bit helper, if the two
// can share an image on the GPU and tell each other when it is ready. That needs
// VK_KHR_external_memory_win32 and VK_KHR_external_semaphore_win32 on both
// sides, on the same GPU - and it is the 32-bit ICD's side that is in doubt.
//
// Built twice from this one file. The 32-bit build is the game's side: it
// reports what its own ICD offers, launches the 64-bit build beside it as the
// helper, imports the image and two semaphores the helper exports, and then for
// a run of frames waits for each one on the GPU, reads it back and checks every
// texel against the pattern the helper was asked to write. The helper's side
// reports whether it gets ray tracing, which is the other half of the question.
//
//   vkxshare.exe                         32-bit, the game's side
//   vkxshare64.exe --helper <pid> <uuid> 64-bit, started by the above
//
// Loads the loader by hand so it needs no import library.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint32_t kSize = 256;       // the shared image is kSize x kSize
static const int kFrames = 60;
static const VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The usage a render target shared this way would really have, so the
// question is asked of the image that would be asked of it.
static const VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
static const VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

static bool gHelper = false;
static void say(const char* format, ...)
{
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	// The helper's stdout is the pipe the game's side reads, so its lines are
	// marked and passed on.
	std::printf("%s%s\n", gHelper ? "helper: " : "", line);
	std::fflush(stdout);
}

// --- Vulkan entry points ------------------------------------------------------

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define FN(name) static PFN_##name name
FN(vkCreateInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties);
FN(vkGetPhysicalDeviceProperties2); FN(vkEnumerateDeviceExtensionProperties);
FN(vkGetPhysicalDeviceQueueFamilyProperties); FN(vkGetPhysicalDeviceMemoryProperties);
FN(vkGetPhysicalDeviceImageFormatProperties2); FN(vkGetPhysicalDeviceExternalSemaphoreProperties);
FN(vkCreateDevice); FN(vkGetDeviceProcAddr);
FN(vkGetDeviceQueue); FN(vkCreateImage); FN(vkGetImageMemoryRequirements); FN(vkAllocateMemory);
FN(vkBindImageMemory); FN(vkCreateBuffer); FN(vkGetBufferMemoryRequirements); FN(vkBindBufferMemory);
FN(vkMapMemory); FN(vkCreateSemaphore); FN(vkCreateFence); FN(vkWaitForFences); FN(vkResetFences);
FN(vkCreateCommandPool); FN(vkAllocateCommandBuffers); FN(vkBeginCommandBuffer); FN(vkEndCommandBuffer);
FN(vkResetCommandBuffer); FN(vkCmdPipelineBarrier); FN(vkCmdCopyBufferToImage); FN(vkCmdCopyImageToBuffer);
FN(vkQueueSubmit); FN(vkDeviceWaitIdle); FN(vkDestroyDevice); FN(vkDestroyInstance);
FN(vkGetMemoryWin32HandleKHR); FN(vkGetSemaphoreWin32HandleKHR); FN(vkImportSemaphoreWin32HandleKHR);
#undef FN

#define LOAD_I(inst, name) name = (PFN_##name)vkGetInstanceProcAddr(inst, #name)
#define LOAD_D(dev, name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

struct Gpu
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory = {};
	uint8_t uuid[VK_UUID_SIZE] = {};
	uint8_t luid[VK_LUID_SIZE] = {};
	bool luidValid = false;
	bool externalMemory = false, externalSemaphore = false, rayQuery = false, accelStruct = false;
	bool timeline = false;
};

static std::string hex(const uint8_t* bytes, size_t n)
{
	std::string s;
	char b[3];
	for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof(b), "%02x", bytes[i]); s += b; }
	return s;
}

static bool hasExt(const std::vector<VkExtensionProperties>& exts, const char* name)
{
	for (const auto& e : exts)
		if (!std::strcmp(e.extensionName, name))
			return true;
	return false;
}

// The instance, the physical device - the one whose UUID is given, else the
// first discrete one - and what it offers.
static bool openGpu(Gpu& g, const char* wantUuid)
{
	HMODULE lib = LoadLibraryA("vulkan-1.dll");
	if (!lib) { say("FAIL vulkan-1.dll would not load"); return false; }
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
	LOAD_I(nullptr, vkCreateInstance);

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "vkxshare";
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkResult r = vkCreateInstance(&ici, nullptr, &g.instance);
	if (r != VK_SUCCESS) { say("FAIL vkCreateInstance returned %d", (int)r); return false; }

	LOAD_I(g.instance, vkEnumeratePhysicalDevices); LOAD_I(g.instance, vkGetPhysicalDeviceProperties);
	LOAD_I(g.instance, vkGetPhysicalDeviceProperties2); LOAD_I(g.instance, vkEnumerateDeviceExtensionProperties);
	LOAD_I(g.instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(g.instance, vkGetPhysicalDeviceMemoryProperties);
	LOAD_I(g.instance, vkGetPhysicalDeviceImageFormatProperties2); LOAD_I(g.instance, vkGetPhysicalDeviceExternalSemaphoreProperties);
	LOAD_I(g.instance, vkCreateDevice); LOAD_I(g.instance, vkGetDeviceProcAddr); LOAD_I(g.instance, vkDestroyInstance);

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(g.instance, &count, devices.data());
	if (!count) { say("FAIL no physical devices"); return false; }

	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	for (VkPhysicalDevice d : devices)
	{
		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		p2.pNext = &id;
		vkGetPhysicalDeviceProperties2(d, &p2);
		if (wantUuid ? hex(id.deviceUUID, VK_UUID_SIZE) == wantUuid
			: (!chosen && p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
			chosen = d;
	}
	if (!chosen && wantUuid) { say("FAIL no device with UUID %s", wantUuid); return false; }
	g.physical = chosen ? chosen : devices[0];

	VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &id;
	vkGetPhysicalDeviceProperties2(g.physical, &p2);
	std::memcpy(g.uuid, id.deviceUUID, VK_UUID_SIZE);
	std::memcpy(g.luid, id.deviceLUID, VK_LUID_SIZE);
	g.luidValid = id.deviceLUIDValid != VK_FALSE;

	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> exts(n);
	vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &n, exts.data());
	g.externalMemory = hasExt(exts, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	g.externalSemaphore = hasExt(exts, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
	g.rayQuery = hasExt(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
	g.accelStruct = hasExt(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
	g.timeline = hasExt(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) || p2.properties.apiVersion >= VK_API_VERSION_1_2;

	say("%d-bit process, %s (api %u.%u.%u, %u device extensions)", (int)(sizeof(void*) * 8), p2.properties.deviceName,
		VK_VERSION_MAJOR(p2.properties.apiVersion), VK_VERSION_MINOR(p2.properties.apiVersion),
		VK_VERSION_PATCH(p2.properties.apiVersion), n);
	say("  UUID %s  LUID %s", hex(g.uuid, VK_UUID_SIZE).c_str(),
		g.luidValid ? hex(g.luid, VK_LUID_SIZE).c_str() : "(not valid)");
	say("  %-40s %s", VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, g.externalMemory ? "YES" : "no");
	say("  %-40s %s", VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, g.externalSemaphore ? "YES" : "no");
	say("  %-40s %s", "timeline semaphores", g.timeline ? "YES" : "no");
	say("  %-40s %s", VK_KHR_RAY_QUERY_EXTENSION_NAME, g.rayQuery ? "YES" : "no");
	say("  %-40s %s", VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, g.accelStruct ? "YES" : "no");
	return true;
}

// Whether the image and the semaphores could be shared at all, as the driver
// answers before anything is made.
static void reportExternalSupport(Gpu& g)
{
	struct { VkFormat format; const char* name; } formats[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" }, { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F" },
	};
	for (const auto& f : formats)
	{
		VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ext.handleType = kMemHandle;
		VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		info.pNext = &ext;
		info.format = f.format;
		info.type = VK_IMAGE_TYPE_2D;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = kUsage;
		VkExternalImageFormatProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		props.pNext = &extProps;
		VkResult r = vkGetPhysicalDeviceImageFormatProperties2(g.physical, &info, &props);
		const VkExternalMemoryFeatureFlags feat = extProps.externalMemoryProperties.externalMemoryFeatures;
		if (r != VK_SUCCESS)
			say("  shared %s storage image: not supported (%d)", f.name, (int)r);
		else
			say("  shared %s storage image: export %s, import %s%s", f.name,
				(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "yes" : "no",
				(feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes" : "no",
				(feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? ", dedicated allocation only" : "");
	}

	VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
	si.handleType = kSemHandle;
	VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(g.physical, &si, &sp);
	say("  shared semaphore: export %s, import %s",
		(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) ? "yes" : "no",
		(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ? "yes" : "no");
}

static bool createDevice(Gpu& g)
{
	uint32_t n = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &n, nullptr);
	std::vector<VkQueueFamilyProperties> families(n);
	vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &n, families.data());
	for (uint32_t i = 0; i < n; i++)
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.family = i; break; }

	std::vector<const char*> enable;
	if (g.externalMemory) enable.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	if (g.externalSemaphore) enable.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g.family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)enable.size();
	dci.ppEnabledExtensionNames = enable.data();
	VkResult r = vkCreateDevice(g.physical, &dci, nullptr, &g.device);
	if (r != VK_SUCCESS) { say("FAIL vkCreateDevice returned %d", (int)r); return false; }

	LOAD_D(g.device, vkGetDeviceQueue); LOAD_D(g.device, vkCreateImage); LOAD_D(g.device, vkGetImageMemoryRequirements);
	LOAD_D(g.device, vkAllocateMemory); LOAD_D(g.device, vkBindImageMemory); LOAD_D(g.device, vkCreateBuffer);
	LOAD_D(g.device, vkGetBufferMemoryRequirements); LOAD_D(g.device, vkBindBufferMemory); LOAD_D(g.device, vkMapMemory);
	LOAD_D(g.device, vkCreateSemaphore); LOAD_D(g.device, vkCreateFence); LOAD_D(g.device, vkWaitForFences);
	LOAD_D(g.device, vkResetFences); LOAD_D(g.device, vkCreateCommandPool); LOAD_D(g.device, vkAllocateCommandBuffers);
	LOAD_D(g.device, vkBeginCommandBuffer); LOAD_D(g.device, vkEndCommandBuffer); LOAD_D(g.device, vkResetCommandBuffer);
	LOAD_D(g.device, vkCmdPipelineBarrier); LOAD_D(g.device, vkCmdCopyBufferToImage); LOAD_D(g.device, vkCmdCopyImageToBuffer);
	LOAD_D(g.device, vkQueueSubmit); LOAD_D(g.device, vkDeviceWaitIdle); LOAD_D(g.device, vkDestroyDevice);
	LOAD_D(g.device, vkGetMemoryWin32HandleKHR); LOAD_D(g.device, vkGetSemaphoreWin32HandleKHR);
	LOAD_D(g.device, vkImportSemaphoreWin32HandleKHR);

	vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
	vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g.family;
	vkCreateCommandPool(g.device, &pci, nullptr, &g.pool);
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	vkAllocateCommandBuffers(g.device, &cai, &g.cmd);
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	vkCreateFence(g.device, &fci, nullptr, &g.fence);
	return true;
}

static int memoryType(const Gpu& g, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < g.memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	return -1;
}

// A host visible buffer for the pattern going in or the frame coming back.
static bool hostBuffer(const Gpu& g, VkBufferUsageFlags usage, VkBuffer& buffer, uint32_t*& mapped)
{
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = kSize * kSize * 4;
	bci.usage = usage;
	vkCreateBuffer(g.device, &bci, nullptr, &buffer);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g.device, buffer, &req);
	int type = memoryType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type < 0) { say("FAIL no host visible memory"); return false; }
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = (uint32_t)type;
	VkDeviceMemory memory;
	vkAllocateMemory(g.device, &mai, nullptr, &memory);
	vkBindBufferMemory(g.device, buffer, memory, 0);
	vkMapMemory(g.device, memory, 0, bci.size, 0, (void**)&mapped);
	return true;
}

static VkImage sharedImage(const Gpu& g)
{
	VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.handleTypes = kMemHandle;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = kFormat;
	ici.extent = { kSize, kSize, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = kUsage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image = VK_NULL_HANDLE;
	vkCreateImage(g.device, &ici, nullptr, &image);
	return image;
}

// Ownership passes between the two devices as a queue family transfer to and
// from VK_QUEUE_FAMILY_EXTERNAL, in GENERAL layout either side of it.
static void barrier(const Gpu& g, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t srcFamily, uint32_t dstFamily,
	VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = srcFamily;
	b.dstQueueFamilyIndex = dstFamily;
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void submit(const Gpu& g, VkSemaphore wait, VkSemaphore signal)
{
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = wait ? 1 : 0;
	si.pWaitSemaphores = &wait;
	si.pWaitDstStageMask = &stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g.cmd;
	si.signalSemaphoreCount = signal ? 1 : 0;
	si.pSignalSemaphores = &signal;
	vkQueueSubmit(g.queue, 1, &si, g.fence);
	vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
	vkResetFences(g.device, 1, &g.fence);
}

static uint32_t pattern(uint32_t x, uint32_t y, int frame)
{
	return ((x + frame) & 255) | ((y & 255) << 8) | (((x ^ y ^ frame) & 255) << 16) | (255u << 24);
}

static void beginCommands(const Gpu& g)
{
	vkResetCommandBuffer(g.cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(g.cmd, &bi);
}

static VkBufferImageCopy wholeImage()
{
	VkBufferImageCopy c{};
	c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	c.imageExtent = { kSize, kSize, 1 };
	return c;
}

// --- the 64-bit helper -------------------------------------------------------

static int helperMain(DWORD clientPid, const char* uuid)
{
	gHelper = true;
	Gpu g;
	if (!openGpu(g, uuid))
		return 1;
	if (!g.externalMemory || !g.externalSemaphore) { say("FAIL the helper's device cannot export"); return 1; }
	if (!createDevice(g))
		return 1;

	HANDLE client = OpenProcess(PROCESS_DUP_HANDLE, FALSE, clientPid);
	if (!client) { say("FAIL could not open the game's process (%lu)", GetLastError()); return 1; }

	// The image, in memory made to be exported.
	VkImage image = sharedImage(g);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, image, &req);
	int type = memoryType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image;
	VkExportMemoryWin32HandleInfoKHR win32{VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
	win32.pNext = &dedicated;
	win32.dwAccess = GENERIC_ALL;
	VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	exportInfo.pNext = &win32;
	exportInfo.handleTypes = kMemHandle;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &exportInfo;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = (uint32_t)type;
	VkDeviceMemory memory;
	VkResult r = vkAllocateMemory(g.device, &mai, nullptr, &memory);
	if (r != VK_SUCCESS) { say("FAIL exportable allocation returned %d", (int)r); return 1; }
	vkBindImageMemory(g.device, image, memory, 0);

	VkMemoryGetWin32HandleInfoKHR getMem{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
	getMem.memory = memory;
	getMem.handleType = kMemHandle;
	HANDLE memHandle = nullptr;
	r = vkGetMemoryWin32HandleKHR(g.device, &getMem, &memHandle);
	if (r != VK_SUCCESS || !memHandle) { say("FAIL vkGetMemoryWin32HandleKHR returned %d", (int)r); return 1; }

	// "ready", signalled here once a frame is in the image, and "released",
	// signalled by the game's side once it has finished reading it.
	VkSemaphore semaphores[2];
	HANDLE semHandles[2];
	for (int i = 0; i < 2; i++)
	{
		VkExportSemaphoreWin32HandleInfoKHR sw{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
		sw.dwAccess = GENERIC_ALL;
		VkExportSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		se.pNext = &sw;
		se.handleTypes = kSemHandle;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		sci.pNext = &se;
		vkCreateSemaphore(g.device, &sci, nullptr, &semaphores[i]);
		VkSemaphoreGetWin32HandleInfoKHR gs{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
		gs.semaphore = semaphores[i];
		gs.handleType = kSemHandle;
		r = vkGetSemaphoreWin32HandleKHR(g.device, &gs, &semHandles[i]);
		if (r != VK_SUCCESS) { say("FAIL vkGetSemaphoreWin32HandleKHR returned %d", (int)r); return 1; }
	}

	// Into the game's process. NT handle values fit 32 bits, which is what
	// lets a 32-bit process hold one a 64-bit process made.
	HANDLE theirs[3] = {};
	HANDLE ours[3] = { memHandle, semHandles[0], semHandles[1] };
	for (int i = 0; i < 3; i++)
		if (!DuplicateHandle(GetCurrentProcess(), ours[i], client, &theirs[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			say("FAIL DuplicateHandle (%lu)", GetLastError());
			return 1;
		}

	VkBuffer staging;
	uint32_t* pixels = nullptr;
	if (!hostBuffer(g, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, pixels))
		return 1;

	std::printf("HANDLES %llx %llx %llx %llu\n", (unsigned long long)(uintptr_t)theirs[0], (unsigned long long)(uintptr_t)theirs[1],
		(unsigned long long)(uintptr_t)theirs[2], (unsigned long long)req.size);
	std::fflush(stdout);

	char line[256];
	for (int frame = 0; frame < kFrames; frame++)
	{
		for (uint32_t y = 0; y < kSize; y++)
			for (uint32_t x = 0; x < kSize; x++)
				pixels[y * kSize + x] = pattern(x, y, frame);

		beginCommands(g);
		if (frame == 0)
			barrier(g, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		else
			barrier(g, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_QUEUE_FAMILY_EXTERNAL, g.family, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy c = wholeImage();
		vkCmdCopyBufferToImage(g.cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		barrier(g, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			g.family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
		vkEndCommandBuffer(g.cmd);
		submit(g, frame ? semaphores[1] : VK_NULL_HANDLE, semaphores[0]);

		std::printf("FRAME %d\n", frame);
		std::fflush(stdout);
		if (!std::fgets(line, sizeof(line), stdin) || std::atoi(line + 3) != frame || std::strncmp(line, "OK ", 3))
			return 1;   // the game's side gave up, and has said why
	}
	std::fgets(line, sizeof(line), stdin);   // DONE
	vkDeviceWaitIdle(g.device);
	return 0;
}

// --- the 32-bit game's side --------------------------------------------------

struct Pipe
{
	HANDLE read = nullptr, write = nullptr;
	std::string pending;

	// A line from the helper, or false after timeoutMs with nothing.
	bool readLine(std::string& out, DWORD timeoutMs)
	{
		const DWORD start = GetTickCount();
		for (;;)
		{
			size_t nl = pending.find('\n');
			if (nl != std::string::npos)
			{
				out = pending.substr(0, nl);
				if (!out.empty() && out.back() == '\r')
					out.pop_back();
				pending.erase(0, nl + 1);
				return true;
			}
			DWORD avail = 0;
			if (!PeekNamedPipe(read, nullptr, 0, nullptr, &avail, nullptr))
				return false;   // the helper has gone
			if (avail)
			{
				char buf[4096];
				DWORD got = 0;
				ReadFile(read, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, nullptr);
				pending.append(buf, got);
				continue;
			}
			if (GetTickCount() - start > timeoutMs)
				return false;
			SwitchToThread();
		}
	}
	void writeLine(const char* format, ...)
	{
		char line[256];
		va_list args;
		va_start(args, format);
		int n = vsnprintf(line, sizeof(line) - 1, format, args);
		va_end(args);
		line[n++] = '\n';
		DWORD wrote = 0;
		WriteFile(write, line, (DWORD)n, &wrote, nullptr);
	}
};

static int clientMain()
{
	Gpu g;
	if (!openGpu(g, nullptr))
		return 1;
	reportExternalSupport(g);
	if (!g.externalMemory || !g.externalSemaphore)
	{
		say("=> this process cannot import a shared image: NOT AVAILABLE");
		return 1;
	}
	if (!createDevice(g))
		return 1;

	// The helper, beside this executable, on the same GPU.
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* slash = std::strrchr(path, '\\');
	std::strcpy(slash ? slash + 1 : path, "vkxshare64.exe");
	char cmdline[MAX_PATH + 128];
	std::snprintf(cmdline, sizeof(cmdline), "\"%s\" --helper %lu %s", path, GetCurrentProcessId(),
		hex(g.uuid, VK_UUID_SIZE).c_str());

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	Pipe fromHelper, toHelper;
	HANDLE helperOut = nullptr, helperIn = nullptr;
	CreatePipe(&fromHelper.read, &helperOut, &sa, 0);
	CreatePipe(&helperIn, &toHelper.write, &sa, 0);
	SetHandleInformation(fromHelper.read, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(toHelper.write, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOA si{sizeof(si)};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = helperOut;
	si.hStdError = helperOut;
	si.hStdInput = helperIn;
	PROCESS_INFORMATION pi{};
	say("");
	say("starting %s", path);
	if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
	{
		say("FAIL could not start the helper (%lu)", GetLastError());
		return 1;
	}
	CloseHandle(helperOut);
	CloseHandle(helperIn);

	// Its report, then the handles.
	std::string line;
	unsigned long long handles[3] = {};
	unsigned long long size = 0;
	for (;;)
	{
		if (!fromHelper.readLine(line, 20000)) { say("FAIL the helper said nothing more"); return 1; }
		if (!std::strncmp(line.c_str(), "HANDLES ", 8))
		{
			std::sscanf(line.c_str() + 8, "%llx %llx %llx %llu", &handles[0], &handles[1], &handles[2], &size);
			break;
		}
		std::printf("%s\n", line.c_str());
		if (line.find("FAIL") != std::string::npos)
			return 1;
	}
	say("");
	say("imported: memory %llx (%llu bytes), semaphores %llx %llx", handles[0], size, handles[1], handles[2]);

	// The same image, in the helper's memory.
	VkImage image = sharedImage(g);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, image, &req);
	int type = memoryType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image;
	VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
	import.pNext = &dedicated;
	import.handleType = kMemHandle;
	import.handle = (HANDLE)(uintptr_t)handles[0];
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &import;
	mai.allocationSize = size;
	mai.memoryTypeIndex = (uint32_t)type;
	VkDeviceMemory memory;
	VkResult r = vkAllocateMemory(g.device, &mai, nullptr, &memory);
	if (r != VK_SUCCESS) { say("FAIL importing the image's memory returned %d", (int)r); return 1; }
	r = vkBindImageMemory(g.device, image, memory, 0);
	if (r != VK_SUCCESS) { say("FAIL binding the imported memory returned %d", (int)r); return 1; }

	VkSemaphore semaphores[2];
	for (int i = 0; i < 2; i++)
	{
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		vkCreateSemaphore(g.device, &sci, nullptr, &semaphores[i]);
		VkImportSemaphoreWin32HandleInfoKHR is{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
		is.semaphore = semaphores[i];
		is.handleType = kSemHandle;
		is.handle = (HANDLE)(uintptr_t)handles[1 + i];
		r = vkImportSemaphoreWin32HandleKHR(g.device, &is);
		if (r != VK_SUCCESS) { say("FAIL importing semaphore %d returned %d", i, (int)r); return 1; }
	}

	VkBuffer readback;
	uint32_t* pixels = nullptr;
	if (!hostBuffer(g, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, pixels))
		return 1;

	// Each frame: wait for "ready" on the GPU, take the image, read it back,
	// hand it back and signal "released".
	int good = 0;
	long long worstMismatch = 0;
	double gpuMs = 0.0;
	for (int frame = 0; frame < kFrames; frame++)
	{
		if (!fromHelper.readLine(line, 20000) || std::strncmp(line.c_str(), "FRAME ", 6) || std::atoi(line.c_str() + 6) != frame)
		{
			say("FAIL frame %d never arrived (%s)", frame, line.c_str());
			break;
		}
		const auto start = std::chrono::steady_clock::now();
		beginCommands(g);
		barrier(g, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_QUEUE_FAMILY_EXTERNAL, g.family, 0, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy c = wholeImage();
		vkCmdCopyImageToBuffer(g.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &c);
		barrier(g, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			g.family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
		vkEndCommandBuffer(g.cmd);
		submit(g, semaphores[0], semaphores[1]);
		gpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

		long long mismatched = 0;
		for (uint32_t y = 0; y < kSize; y++)
			for (uint32_t x = 0; x < kSize; x++)
				mismatched += pixels[y * kSize + x] != pattern(x, y, frame);
		if (mismatched == 0)
			good++;
		else if (mismatched > worstMismatch)
			worstMismatch = mismatched;
		if (frame == 0 || mismatched)
			say("frame %d: %lld of %u texels wrong%s", frame, mismatched, kSize * kSize,
				mismatched ? "" : " - the helper's image arrived intact");
		toHelper.writeLine("OK %d", frame);
	}
	toHelper.writeLine("DONE");
	WaitForSingleObject(pi.hProcess, 5000);
	DWORD exitCode = 0;
	GetExitCodeProcess(pi.hProcess, &exitCode);

	say("");
	say("%d of %d frames intact, helper exited %lu", good, kFrames, exitCode);
	say("GPU side of each handoff: %.3f ms on average (submit to fence, waiting on the helper's semaphore)",
		good ? gpuMs / kFrames : 0.0);
	say("=> a 64-bit helper's image, shared with this 32-bit process: %s",
		good == kFrames ? "WORKS" : "DOES NOT WORK");
	return good == kFrames ? 0 : 1;
}

int main(int argc, char** argv)
{
	if (argc >= 4 && !std::strcmp(argv[1], "--helper"))
		return helperMain((DWORD)std::strtoul(argv[2], nullptr, 10), argv[3]);
	std::printf("pointer size: %d bits\n\n", (int)(sizeof(void*) * 8));
	return clientMain();
}
