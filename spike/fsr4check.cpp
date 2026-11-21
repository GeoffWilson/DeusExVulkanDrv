// Does AMD's FSR 4 run from the path tracer's 64-bit helper on this machine?
//
// FidelityFX SDK v2.4.0 brought FSR 4 to Vulkan, as signed DLLs only. Whether
// it runs here - on RADV under Proton in particular, which AMD says nothing
// about - is what this answers, before the helper is taught to use it. It
// makes a device the way the helper would, with the extensions the upscaler
// names, asks the loader which upscalers it offers for that device, then for
// each one makes a context, upscales a flat colour, times the dispatches and
// reads the picture back to see that it came through.
//
//   fsr4check [outputWidth outputHeight [scale [--all]]]
//
// Defaults to 1920x1080 from half that a side (performance). --all enables
// every feature the device has rather than only those FSR 4 is expected to
// want, to tell a missing feature from anything else. The two DLLs must be
// beside it.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <vk/ffx_api_vk.h>
#include <ffx_upscale.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#define VKF(name) static PFN_##name name
VKF(vkGetInstanceProcAddr); VKF(vkCreateInstance); VKF(vkEnumeratePhysicalDevices);
VKF(vkGetPhysicalDeviceProperties2); VKF(vkGetPhysicalDeviceFeatures2); VKF(vkEnumerateDeviceExtensionProperties);
VKF(vkGetPhysicalDeviceQueueFamilyProperties); VKF(vkGetPhysicalDeviceMemoryProperties); VKF(vkCreateDevice);
VKF(vkGetDeviceProcAddr); VKF(vkGetDeviceQueue); VKF(vkCreateImage); VKF(vkGetImageMemoryRequirements);
VKF(vkAllocateMemory); VKF(vkBindImageMemory); VKF(vkCreateBuffer); VKF(vkGetBufferMemoryRequirements);
VKF(vkBindBufferMemory); VKF(vkMapMemory); VKF(vkCreateCommandPool); VKF(vkAllocateCommandBuffers);
VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer); VKF(vkResetCommandBuffer); VKF(vkQueueSubmit);
VKF(vkQueueWaitIdle); VKF(vkCmdPipelineBarrier); VKF(vkCmdClearColorImage); VKF(vkCmdCopyImageToBuffer);
VKF(vkCreateQueryPool); VKF(vkCmdResetQueryPool); VKF(vkCmdWriteTimestamp); VKF(vkGetQueryPoolResults);
VKF(vkDeviceWaitIdle);

static VkInstance Instance;
static VkPhysicalDevice Physical;
static VkDevice Device;
static VkQueue Queue;
static uint32_t Family;
static VkPhysicalDeviceMemoryProperties Memory;

static void Message(uint32_t type, const wchar_t* message)
{
	std::printf("  [ffx %s] %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

static uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < Memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (Memory.memoryTypes[i].propertyFlags & want) == want)
			return i;
	std::printf("FAIL: no memory type\n");
	std::exit(1);
}

struct Image
{
	VkImage Handle = VK_NULL_HANDLE;
	VkImageCreateInfo Info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

static Image MakeImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage)
{
	Image image;
	image.Info.imageType = VK_IMAGE_TYPE_2D;
	image.Info.format = format;
	image.Info.extent = {w, h, 1};
	image.Info.mipLevels = 1;
	image.Info.arrayLayers = 1;
	image.Info.samples = VK_SAMPLE_COUNT_1_BIT;
	image.Info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.Info.usage = usage;
	if (vkCreateImage(Device, &image.Info, nullptr, &image.Handle) != VK_SUCCESS) { std::printf("FAIL: vkCreateImage\n"); std::exit(1); }
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(Device, image.Handle, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem;
	vkAllocateMemory(Device, &ai, nullptr, &mem);
	vkBindImageMemory(Device, image.Handle, mem, 0);
	return image;
}

static void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool Has(const std::vector<VkExtensionProperties>& exts, const char* name)
{
	for (const auto& e : exts)
		if (!std::strcmp(e.extensionName, name))
			return true;
	return false;
}

int main(int argc, char** argv)
{
	uint32_t outW = argc > 2 ? (uint32_t)std::atoi(argv[1]) : 1920;
	uint32_t outH = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 1080;
	float scale = argc > 3 ? (float)std::atof(argv[3]) : 2.0f;
	bool all = argc > 4 && !std::strcmp(argv[4], "--all");
	uint32_t renW = (uint32_t)std::lround(outW / scale), renH = (uint32_t)std::lround(outH / scale);

	HMODULE vk = LoadLibraryA("vulkan-1.dll");
	if (!vk) { std::printf("FAIL: vulkan-1.dll would not load\n"); return 1; }
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
#define INST(name) name = (PFN_##name)vkGetInstanceProcAddr(Instance, #name)
	INST(vkCreateInstance);

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "fsr4check";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (vkCreateInstance(&ici, nullptr, &Instance) != VK_SUCCESS) { std::printf("FAIL: vkCreateInstance\n"); return 1; }
	INST(vkEnumeratePhysicalDevices); INST(vkGetPhysicalDeviceProperties2); INST(vkGetPhysicalDeviceFeatures2);
	INST(vkEnumerateDeviceExtensionProperties); INST(vkGetPhysicalDeviceQueueFamilyProperties);
	INST(vkGetPhysicalDeviceMemoryProperties); INST(vkCreateDevice); INST(vkGetDeviceProcAddr);

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(Instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(Instance, &count, devices.data());
	if (!count) { std::printf("FAIL: no Vulkan device\n"); return 1; }
	Physical = devices[0];

	VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
	vkGetPhysicalDeviceProperties2(Physical, &props);
	std::printf("device: %s, %s %s, vendor 0x%04x device 0x%04x\n", props.properties.deviceName,
		driver.driverName, driver.driverInfo, props.properties.vendorID, props.properties.deviceID);
	vkGetPhysicalDeviceMemoryProperties(Physical, &Memory);

	uint32_t nexts = 0;
	vkEnumerateDeviceExtensionProperties(Physical, nullptr, &nexts, nullptr);
	std::vector<VkExtensionProperties> exts(nexts);
	vkEnumerateDeviceExtensionProperties(Physical, nullptr, &nexts, exts.data());

	// The extensions amd_fidelityfx_upscaler_vk.dll names; float8 is RDNA4's
	// and this header predates it, so it is only reported.
	const char* wanted[] = {
		VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
		VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
		VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
		VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME,
		VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
	};
	std::vector<const char*> enable;
	for (const char* name : wanted)
	{
		bool have = Has(exts, name);
		std::printf("  %-36s %s\n", name, have ? "yes" : "no");
		if (have)
			enable.push_back(name);
	}
	std::printf("  %-36s %s (not enabled)\n", "VK_EXT_shader_float8", Has(exts, "VK_EXT_shader_float8") ? "yes" : "no");
	bool coopmat = Has(exts, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);

	// Features: what the device has, then either all of it or the few FSR 4
	// is expected to want.
	VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopHave{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
	VkPhysicalDeviceVulkan13Features v13Have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, coopmat ? &coopHave : nullptr};
	VkPhysicalDeviceVulkan12Features v12Have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13Have};
	VkPhysicalDeviceVulkan11Features v11Have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12Have};
	VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11Have};
	vkGetPhysicalDeviceFeatures2(Physical, &have);
	std::printf("  shaderInt8 %d shaderFloat16 %d shaderInt16 %d cooperativeMatrix %d subgroupSizeControl %d computeFullSubgroups %d vulkanMemoryModel %d\n",
		v12Have.shaderInt8, v12Have.shaderFloat16, have.features.shaderInt16, coopHave.cooperativeMatrix,
		v13Have.subgroupSizeControl, v13Have.computeFullSubgroups, v12Have.vulkanMemoryModel);

	VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
	VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, coopmat ? &coop : nullptr};
	VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
	VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
	VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
	if (all)
	{
		coop = coopHave; coop.pNext = nullptr;
		v13 = v13Have; v13.pNext = coopmat ? &coop : nullptr;
		v12 = v12Have; v12.pNext = &v13;
		v11 = v11Have; v11.pNext = &v12;
		feats = have; feats.pNext = &v11;
		feats.features.robustBufferAccess = VK_FALSE;
		v13.robustImageAccess = VK_FALSE;
		std::printf("enabling every feature the device has\n");
	}
	else
	{
		feats.features.shaderInt16 = have.features.shaderInt16;
		feats.features.shaderInt64 = have.features.shaderInt64;
		v11.storageBuffer16BitAccess = v11Have.storageBuffer16BitAccess;
		v11.uniformAndStorageBuffer16BitAccess = v11Have.uniformAndStorageBuffer16BitAccess;
		v12.shaderFloat16 = v12Have.shaderFloat16;
		v12.shaderInt8 = v12Have.shaderInt8;
		v12.storageBuffer8BitAccess = v12Have.storageBuffer8BitAccess;
		v12.uniformAndStorageBuffer8BitAccess = v12Have.uniformAndStorageBuffer8BitAccess;
		v12.vulkanMemoryModel = v12Have.vulkanMemoryModel;
		v12.vulkanMemoryModelDeviceScope = v12Have.vulkanMemoryModelDeviceScope;
		v12.bufferDeviceAddress = v12Have.bufferDeviceAddress;
		v12.descriptorIndexing = v12Have.descriptorIndexing;
		v12.timelineSemaphore = v12Have.timelineSemaphore;
		v12.hostQueryReset = v12Have.hostQueryReset;
		v13.subgroupSizeControl = v13Have.subgroupSizeControl;
		v13.computeFullSubgroups = v13Have.computeFullSubgroups;
		v13.synchronization2 = v13Have.synchronization2;
		v13.shaderIntegerDotProduct = v13Have.shaderIntegerDotProduct;
		v13.maintenance4 = v13Have.maintenance4;
		coop.cooperativeMatrix = coopHave.cooperativeMatrix;
		std::printf("enabling only the features FSR 4 is expected to want\n");
	}

	uint32_t nfam = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(Physical, &nfam, nullptr);
	std::vector<VkQueueFamilyProperties> fams(nfam);
	vkGetPhysicalDeviceQueueFamilyProperties(Physical, &nfam, fams.data());
	for (Family = 0; Family < nfam; Family++)
		if ((fams[Family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
			break;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = Family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feats};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)enable.size();
	dci.ppEnabledExtensionNames = enable.data();
	VkResult r = vkCreateDevice(Physical, &dci, nullptr, &Device);
	if (r != VK_SUCCESS) { std::printf("FAIL: vkCreateDevice returned %d\n", (int)r); return 1; }
#define DEV(name) name = (PFN_##name)vkGetDeviceProcAddr(Device, #name)
	DEV(vkGetDeviceQueue); DEV(vkCreateImage); DEV(vkGetImageMemoryRequirements); DEV(vkAllocateMemory);
	DEV(vkBindImageMemory); DEV(vkCreateBuffer); DEV(vkGetBufferMemoryRequirements); DEV(vkBindBufferMemory);
	DEV(vkMapMemory); DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers); DEV(vkBeginCommandBuffer);
	DEV(vkEndCommandBuffer); DEV(vkResetCommandBuffer); DEV(vkQueueSubmit); DEV(vkQueueWaitIdle);
	DEV(vkCmdPipelineBarrier); DEV(vkCmdClearColorImage); DEV(vkCmdCopyImageToBuffer); DEV(vkCreateQueryPool);
	DEV(vkCmdResetQueryPool); DEV(vkCmdWriteTimestamp); DEV(vkGetQueryPoolResults); DEV(vkDeviceWaitIdle);
	vkGetDeviceQueue(Device, Family, 0, &Queue);

	// The loader, from beside this program.
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string dir(path);
	dir = dir.substr(0, dir.find_last_of("\\/") + 1);
	HMODULE loader = LoadLibraryA((dir + "amd_fidelityfx_loader_vk.dll").c_str());
	if (!loader) { std::printf("FAIL: amd_fidelityfx_loader_vk.dll would not load (%lu)\n", GetLastError()); return 1; }
	ffxFunctions ffx = {};
	ffxLoadFunctions(&ffx, loader);
	if (!ffx.CreateContext || !ffx.Query || !ffx.Dispatch) { std::printf("FAIL: loader lacks the ffx-api entry points\n"); return 1; }

	ffxConfigureDescGlobalDebug debug{};
	debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
	debug.effectId = FFX_API_EFFECT_ID_UPSCALE;
	debug.fpMessage = Message;
	debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
	ffx.Configure(nullptr, &debug.header);

	VkDeviceContext deviceContext{Device, Physical, vkGetDeviceProcAddr};
	std::vector<uint64_t> ids;
	for (int withDevice = 0; withDevice < 2; withDevice++)
	{
		ffxQueryDescGetVersions q{};
		q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		q.device = withDevice ? &deviceContext : nullptr;
		uint64_t n = 0;
		q.outputCount = &n;
		ffxReturnCode_t rc = ffx.Query(nullptr, &q.header);
		std::vector<uint64_t> got(n);
		std::vector<const char*> names(n);
		q.versionIds = got.data();
		q.versionNames = names.data();
		if (n)
			rc = ffx.Query(nullptr, &q.header);
		std::printf("upscalers offered %s the device (%d):", withDevice ? "with" : "without", (int)rc);
		for (uint64_t i = 0; i < n; i++)
			std::printf(" [%s]", names[i]);
		std::printf("\n");
		if (withDevice)
			ids = got;
	}

	// Inputs: a flat colour, depth at the far plane, no motion.
	VkImageUsageFlags read = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	Image color = MakeImage(renW, renH, VK_FORMAT_R16G16B16A16_SFLOAT, read);
	Image depth = MakeImage(renW, renH, VK_FORMAT_R32_SFLOAT, read);
	Image motion = MakeImage(renW, renH, VK_FORMAT_R16G16_SFLOAT, read);
	Image output = MakeImage(outW, outH, VK_FORMAT_R16G16B16A16_SFLOAT, read | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	VkBuffer readback;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = (VkDeviceSize)outW * outH * 8;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vkCreateBuffer(Device, &bci, nullptr, &readback);
	VkMemoryRequirements breq;
	vkGetBufferMemoryRequirements(Device, readback, &breq);
	VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	bai.allocationSize = breq.size;
	bai.memoryTypeIndex = MemoryType(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bmem;
	vkAllocateMemory(Device, &bai, nullptr, &bmem);
	vkBindBufferMemory(Device, readback, bmem, 0);
	uint16_t* mapped = nullptr;
	vkMapMemory(Device, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&mapped);

	VkCommandPool pool;
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = Family;
	vkCreateCommandPool(Device, &pci, nullptr, &pool);
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	vkAllocateCommandBuffers(Device, &cai, &cb);
	VkQueryPool queries;
	VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qpci.queryCount = 2;
	vkCreateQueryPool(Device, &qpci, nullptr, &queries);

	auto submit = [&]() -> bool {
		vkEndCommandBuffer(cb);
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		VkResult sr = vkQueueSubmit(Queue, 1, &si, VK_NULL_HANDLE);
		VkResult wr = sr == VK_SUCCESS ? vkQueueWaitIdle(Queue) : sr;
		if (wr != VK_SUCCESS)
			std::printf("  FAIL: submit/wait returned %d%s\n", (int)wr, wr == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
		vkResetCommandBuffer(cb, 0);
		return wr == VK_SUCCESS;
	};
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

	// Fill the inputs and leave them where FSR wants them: read-only for the
	// inputs, general for the output.
	vkBeginCommandBuffer(cb, &begin);
	VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkClearColorValue grey = {{0.25f, 0.5f, 0.75f, 1.0f}}, zero = {{0, 0, 0, 0}};
	for (Image* img : {&color, &depth, &motion})
	{
		Barrier(cb, img->Handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdClearColorImage(cb, img->Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, img == &color ? &grey : &zero, 1, &range);
		Barrier(cb, img->Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	Barrier(cb, output.Handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	if (!submit())
		return 1;

	auto resource = [](const Image& img, uint32_t state) {
		return ffxApiGetResourceVK((void*)img.Handle, ffxApiGetImageResourceDescriptionVK(img.Handle, img.Info, 0), state);
	};

	std::printf("\n%ux%u from %ux%u (%.2fx)\n", outW, outH, renW, renH, scale);
	int working = 0;
	for (uint64_t id : ids)
	{
		ffxQueryDescGetVersions q{};
		q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		q.device = &deviceContext;

		ffxCreateBackendVKDesc backend{};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
		backend.deviceContext = deviceContext;
		ffxCreateContextDescUpscaleVersion version{};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.header.pNext = &backend.header;
		version.version = FFX_UPSCALER_VERSION;
		ffxOverrideVersion override{};
		override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		override.header.pNext = &version.header;
		override.versionId = id;
		ffxCreateContextDescUpscale create{};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		create.header.pNext = &override.header;
		create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		create.maxRenderSize = {renW, renH};
		create.maxUpscaleSize = {outW, outH};
		create.fpMessage = Message;

		ffxContext context = nullptr;
		DWORD t0 = GetTickCount();
		ffxReturnCode_t rc = ffx.CreateContext(&context, &create.header, nullptr);
		DWORD made = GetTickCount() - t0;
		ffxQueryGetProviderVersion which{};
		which.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (rc == FFX_API_RETURN_OK)
			ffx.Query(&context, &which.header);
		std::printf("version id 0x%llx: context %s (%d) in %lu ms, provider %s\n", (unsigned long long)id,
			rc == FFX_API_RETURN_OK ? "made" : "FAILED", (int)rc, made, which.versionName ? which.versionName : "?");
		if (rc != FFX_API_RETURN_OK)
			continue;

		const int warm = 10, timed = 60;
		std::vector<double> ms;
		bool ok = true;
		for (int frame = 0; frame < warm + timed && ok; frame++)
		{
			vkBeginCommandBuffer(cb, &begin);
			vkCmdResetQueryPool(cb, queries, 0, 2);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
			ffxDispatchDescUpscale d{};
			d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
			d.commandList = cb;
			d.color = resource(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
			d.depth = resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
			d.motionVectors = resource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
			d.output = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
			d.jitterOffset = {0.0f, 0.0f};
			d.motionVectorScale = {(float)renW, (float)renH};
			d.renderSize = {renW, renH};
			d.upscaleSize = {outW, outH};
			d.frameTimeDelta = 16.7f;
			d.preExposure = 1.0f;
			d.reset = frame == 0;
			d.cameraNear = 1.0f;
			d.cameraFar = INFINITY;
			d.cameraFovAngleVertical = 1.2f;
			d.viewSpaceToMetersFactor = 1.0f / 52.5f;
			rc = ffx.Dispatch(&context, &d.header);
			if (rc != FFX_API_RETURN_OK) { std::printf("  FAIL: dispatch returned %d\n", (int)rc); vkEndCommandBuffer(cb); vkResetCommandBuffer(cb, 0); ok = false; break; }
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
			ok = submit();
			uint64_t ts[2] = {};
			if (ok && vkGetQueryPoolResults(Device, queries, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && frame >= warm)
				ms.push_back((ts[1] - ts[0]) * props.properties.limits.timestampPeriod / 1e6);
		}
		if (!ok)
			continue;

		// Read the middle of the output back: it ought to be the colour that went in.
		vkBeginCommandBuffer(cb, &begin);
		Barrier(cb, output.Handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {outW, outH, 1};
		vkCmdCopyImageToBuffer(cb, output.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
		Barrier(cb, output.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		if (!submit())
			continue;
		auto half = [](uint16_t h) {
			int e = (h >> 10) & 31, m = h & 1023;
			float v = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, e - 15);
			return (h & 0x8000) ? -v : v;
		};
		const uint16_t* px = mapped + ((size_t)(outH / 2) * outW + outW / 2) * 4;
		std::sort(ms.begin(), ms.end());
		std::printf("  dispatch: median %.3f ms, min %.3f, max %.3f over %zu; centre pixel %.3f %.3f %.3f (sent 0.25 0.5 0.75)\n",
			ms[ms.size() / 2], ms.front(), ms.back(), ms.size(), half(px[0]), half(px[1]), half(px[2]));
		bool right = std::fabs(half(px[0]) - 0.25f) < 0.05f && std::fabs(half(px[1]) - 0.5f) < 0.05f && std::fabs(half(px[2]) - 0.75f) < 0.05f;
		std::printf("  => %s\n", right ? "WORKS" : "ran, but the picture is wrong");
		working += right;
		ffx.DestroyContext(&context, nullptr);
	}
	vkDeviceWaitIdle(Device);
	std::printf("\n%d of %zu upscalers work\n", working, ids.size());
	return 0;
}
