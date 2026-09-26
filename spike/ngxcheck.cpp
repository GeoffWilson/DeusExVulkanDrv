// Can the path tracer's 64-bit helper run DLSS Ray Reconstruction here?
//
// NVIDIA's NGX, which DLSS runs on, is 64-bit only: it was never an option
// inside the game's 32-bit process, and it is one now that the tracing lives in
// PathTracerHelper.exe. What is in doubt is the environment. NGX finds its core
// through the driver, which on Windows is the driver's own install and under
// wine or Proton is a Windows DLL the translation layer has to provide.
//
// So this asks, a step at a time, what the helper would have to ask:
//   1. which Vulkan extensions NGX needs, before the instance and device exist;
//   2. a device with those and with everything the helper already asks for;
//   3. NGX itself, and where it found its core;
//   4. whether this GPU and driver offer Ray Reconstruction;
//   5. the render size it wants at each quality, for the game's resolution;
//   6. the feature made and run at each quality on made-up inputs - a flat
//      colour over a flat surface - timed on the GPU, and its output checked
//      against the colour it was given.
//
//   ngxcheck.exe [width height] [--verbose]     (nvngx_dlssd.dll beside it)
//
// --verbose passes NGX's own log through, which says where it looked.
// Built only by the 64-bit configuration, when cmake/fetch-dlss.sh has run.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	printf("[%s] %s\n", typestr, msg.c_str());
}

void VulkanError(const char* text)
{
	throw std::runtime_error(text);
}

static const unsigned long long NgxAppId = 0x5054485244584e47ull;   // any id will do outside a shipping title

static const char* ResultName(NVSDK_NGX_Result r)
{
	switch (r)
	{
	case NVSDK_NGX_Result_Success: return "Success";
	case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
	case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
	case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
	case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
	case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
	case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
	case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
	case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
	case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
	case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
	case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
	case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
	case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
	case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
	case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
	case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
	case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
	case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
	default: return "unknown";
	}
}

static void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
	printf("  ngx: %s", message);
	if (!*message || message[strlen(message) - 1] != '\n')
		printf("\n");
}

// Where NGX's loader finds the driver's half: a registry value the Windows
// driver sets, and under Proton the prefix's System32, which Proton fills
// from the Linux driver when NVAPI is enabled.
static void ReportCore()
{
	wchar_t path[MAX_PATH] = {};
	DWORD size = sizeof(path);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS)
		printf("registry NGXCore FullPath: %ls\n", path);
	else
		printf("registry NGXCore FullPath: not set\n");

	wchar_t system[MAX_PATH] = {};
	GetSystemDirectoryW(system, MAX_PATH);
	for (const wchar_t* name : { L"nvngx.dll", L"_nvngx.dll", L"nvapi64.dll" })
	{
		std::wstring file = std::wstring(system) + L"\\" + name;
		printf("%ls: %s\n", file.c_str(), GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES ? "present" : "absent");
	}
	printf("nvngx_dlssd.dll beside this: %s\n", GetFileAttributesW(L"nvngx_dlssd.dll") != INVALID_FILE_ATTRIBUTES ? "present" : "absent");
}

// NGX_DLSSD_GET_OPTIMAL_SETTINGS, which the SDK only offers from its D3D
// helpers: the render size Ray Reconstruction is trained for at a quality.
static bool OptimalRenderSize(NVSDK_NGX_Parameter* params, uint32_t width, uint32_t height, NVSDK_NGX_PerfQuality_Value quality, uint32_t& outWidth, uint32_t& outHeight)
{
	void* callback = nullptr;
	NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &callback);
	if (!callback)
		return false;
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, width);
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, height);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, quality);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_RTXValue, 0);
	if (NVSDK_NGX_FAILED(((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback)(params)))
		return false;
	unsigned int w = 0, h = 0;
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutWidth, &w);
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutHeight, &h);
	outWidth = w;
	outHeight = h;
	return w > 0 && h > 0;
}

static float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
	float value;
	if (exponent == 0)
		value = std::ldexp((float)mantissa, -24);
	else if (exponent == 31)
		value = mantissa ? NAN : INFINITY;
	else
		value = std::ldexp((float)(mantissa | 1024), (int)exponent - 25);
	return sign ? -value : value;
}

static float Halton(uint32_t index, uint32_t base)
{
	float f = 1.0f, r = 0.0f;
	for (uint32_t i = index + 1; i > 0; i /= base)
	{
		f /= (float)base;
		r += f * (float)(i % base);
	}
	return r;
}

struct Target
{
	std::unique_ptr<VulkanImage> Image;
	std::unique_ptr<VulkanImageView> View;
	VkFormat Format;
	uint32_t Width, Height;

	NVSDK_NGX_Resource_VK Resource(bool readWrite) const
	{
		return NVSDK_NGX_Create_ImageView_Resource_VK(View->view, Image->image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, Format, Width, Height, readWrite);
	}
};

static Target MakeTarget(VulkanDevice* device, VkFormat format, uint32_t width, uint32_t height, const char* name)
{
	Target t;
	t.Format = format;
	t.Width = width;
	t.Height = height;
	t.Image = ImageBuilder()
		.Format(format)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName(name)
		.Create(device);
	t.View = ImageViewBuilder().Image(t.Image.get(), format).DebugName(name).Create(device);
	return t;
}

static void Everything(VulkanCommandBuffer* commands)
{
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

int main(int argc, char** argv)
{
	uint32_t displayWidth = 1920, displayHeight = 1440;
	bool verbose = false;
	std::vector<uint32_t> sizes;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--verbose"))
			verbose = true;
		else
			sizes.push_back((uint32_t)atoi(argv[i]));
	}
	if (sizes.size() >= 2)
	{
		displayWidth = sizes[0];
		displayHeight = sizes[1];
	}
	setvbuf(stdout, nullptr, _IONBF, 0);

	// NGX looks for its feature DLLs beside the executable, and writes its
	// logs and caches to the data path.
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string dir = exe;
	dir = dir.substr(0, dir.find_last_of('\\'));
	SetCurrentDirectoryA(dir.c_str());
	printf("ngxcheck, %d-bit, DLSS SDK headers %x\n", (int)(sizeof(void*) * 8), (unsigned)NVSDK_NGX_Version_API);
	ReportCore();

	try
	{
		// 1. What NGX needs from Vulkan, asked before there is anything to ask
		// it through.
		unsigned int instanceCount = 0, deviceCount = 0;
		const char** instanceExts = nullptr;
		const char** deviceExts = nullptr;
		NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instanceExts, &deviceCount, &deviceExts);
		printf("\n1. required extensions: %s\n", ResultName(r));
		if (NVSDK_NGX_FAILED(r))
			return 1;
		for (unsigned int i = 0; i < instanceCount; i++)
			printf("   instance %s\n", instanceExts[i]);
		for (unsigned int i = 0; i < deviceCount; i++)
			printf("   device   %s\n", deviceExts[i]);

		// 2. The helper's device, plus those. Whatever the environment does not
		// offer is reported rather than failed on, so the rest still runs.
		VulkanInstanceBuilder instanceBuilder;
		for (unsigned int i = 0; i < instanceCount; i++)
			instanceBuilder.OptionalExtension(instanceExts[i]);
		auto instance = instanceBuilder.Create();
		for (unsigned int i = 0; i < instanceCount; i++)
			if (!instance->EnabledExtensions.count(instanceExts[i]))
				printf("   MISSING instance extension %s\n", instanceExts[i]);

		VulkanDeviceBuilder deviceBuilder;
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
		deviceBuilder.OptionalRayQuery();
		deviceBuilder.OptionalDescriptorIndexing();
		for (unsigned int i = 0; i < deviceCount; i++)
			deviceBuilder.OptionalExtension(deviceExts[i]);
		auto device = deviceBuilder.Create(instance);
		const auto& props = device->PhysicalDevice.Properties.Properties;
		printf("\n2. device: %s, driver %u.%u, ray query %s\n", props.deviceName,
			props.driverVersion >> 22, (props.driverVersion >> 14) & 0xff,
			device->EnabledFeatures.RayQuery.rayQuery ? "yes" : "no");
		bool missing = false;
		for (unsigned int i = 0; i < deviceCount; i++)
			if (!device->EnabledDeviceExtensions.count(deviceExts[i]))
			{
				printf("   MISSING device extension %s\n", deviceExts[i]);
				missing = true;
			}
		if (!missing)
			printf("   every extension NGX asked for is enabled\n");

		// 3. NGX itself.
		NVSDK_NGX_FeatureCommonInfo info = {};
		info.LoggingInfo.LoggingCallback = NgxLog;
		info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_OFF;
		info.LoggingInfo.DisableOtherLoggingSinks = true;
		std::wstring dataPath(dir.begin(), dir.end());
		r = NVSDK_NGX_VULKAN_Init(NgxAppId, dataPath.c_str(), instance->Instance, device->PhysicalDevice.Device, device->device,
			vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
		printf("\n3. NGX init: %s\n", ResultName(r));
		if (NVSDK_NGX_FAILED(r))
		{
			printf("=> NO: NGX did not start%s\n", verbose ? "" : " (--verbose says why)");
			return 1;
		}

		// 4. Ray Reconstruction on this GPU and driver.
		NVSDK_NGX_Parameter* params = nullptr;
		r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
		int available = 0, needsDriver = 0, initResult = 0;
		unsigned int minMajor = 0, minMinor = 0;
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &minMajor);
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minMinor);
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &initResult);
		printf("\n4. capabilities: %s; Ray Reconstruction %s", ResultName(r), available ? "AVAILABLE" : "not available");
		if (needsDriver)
			printf(", needs driver %u.%u", minMajor, minMinor);
		if (!available)
			printf(", init result %s", ResultName((NVSDK_NGX_Result)initResult));
		printf("\n");
		if (!available)
		{
			printf("=> NO: NGX runs, but not Ray Reconstruction\n");
			NVSDK_NGX_VULKAN_DestroyParameters(params);
			NVSDK_NGX_VULKAN_Shutdown1(device->device);
			return 1;
		}

		// 5. and 6., at each quality.
		struct Level { const char* Name; NVSDK_NGX_PerfQuality_Value Value; };
		const Level levels[] = {
			{ "DLAA", NVSDK_NGX_PerfQuality_Value_DLAA },
			{ "Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality },
			{ "Balanced", NVSDK_NGX_PerfQuality_Value_Balanced },
			{ "Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf },
		};
		printf("\n5./6. at %ux%u:\n", displayWidth, displayHeight);

		auto pool = CommandPoolBuilder().QueueFamily(device->GraphicsFamily).Create(device.get());
		auto fence = FenceBuilder().Create(device.get());
		auto timestamps = QueryPoolBuilder().QueryType(VK_QUERY_TYPE_TIMESTAMP, 2).Create(device.get());
		const double tickMs = props.limits.timestampPeriod * 1.0e-6;
		auto readback = BufferBuilder().Size(8).Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU).Create(device.get());

		auto run = [&](const std::function<void(VulkanCommandBuffer*)>& record)
		{
			auto commands = pool->createBuffer();
			commands->begin();
			record(commands.get());
			commands->end();
			QueueSubmit().AddCommandBuffer(commands.get()).Execute(device.get(), device->GraphicsQueue, fence.get());
			vkWaitForFences(device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
			vkResetFences(device->device, 1, &fence->fence);
		};

		// The colour it is given, over a surface of half that albedo.
		const float colour[4] = { 0.6f, 0.4f, 0.2f, 1.0f };
		bool allGood = true;
		for (const Level& level : levels)
		{
			uint32_t renderWidth = displayWidth, renderHeight = displayHeight;
			if (!OptimalRenderSize(params, displayWidth, displayHeight, level.Value, renderWidth, renderHeight))
			{
				printf("   %-11s no render size offered\n", level.Name);
				continue;
			}

			Target color = MakeTarget(device.get(), VK_FORMAT_R16G16B16A16_SFLOAT, renderWidth, renderHeight, "ngxColor");
			Target diffuse = MakeTarget(device.get(), VK_FORMAT_R16G16B16A16_SFLOAT, renderWidth, renderHeight, "ngxDiffuseAlbedo");
			Target specular = MakeTarget(device.get(), VK_FORMAT_R16G16B16A16_SFLOAT, renderWidth, renderHeight, "ngxSpecularAlbedo");
			Target normals = MakeTarget(device.get(), VK_FORMAT_R16G16B16A16_SFLOAT, renderWidth, renderHeight, "ngxNormalRoughness");
			Target depth = MakeTarget(device.get(), VK_FORMAT_R32_SFLOAT, renderWidth, renderHeight, "ngxDepth");
			Target motion = MakeTarget(device.get(), VK_FORMAT_R16G16_SFLOAT, renderWidth, renderHeight, "ngxMotion");
			Target output = MakeTarget(device.get(), VK_FORMAT_R16G16B16A16_SFLOAT, displayWidth, displayHeight, "ngxOutput");

			run([&](VulkanCommandBuffer* commands)
			{
				PipelineBarrier barrier;
				for (Target* t : { &color, &diffuse, &specular, &normals, &depth, &motion, &output })
					barrier.AddImage(t->Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
				barrier.Execute(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				auto clear = [&](Target& t, float r, float g, float b, float a)
				{
					VkClearColorValue value = {};
					value.float32[0] = r; value.float32[1] = g; value.float32[2] = b; value.float32[3] = a;
					VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
					vkCmdClearColorImage(commands->buffer, t.Image->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
				};
				clear(color, colour[0], colour[1], colour[2], colour[3]);
				clear(diffuse, 0.5f, 0.5f, 0.5f, 1.0f);
				clear(specular, 0.04f, 0.04f, 0.04f, 1.0f);
				clear(normals, 0.0f, 0.0f, 1.0f, 0.5f);   // facing the camera, roughness in w
				clear(depth, 500.0f, 0.0f, 0.0f, 0.0f);   // linear view depth
				clear(motion, 0.0f, 0.0f, 0.0f, 0.0f);
				clear(output, 0.0f, 0.0f, 0.0f, 0.0f);
				Everything(commands);
			});

			NVSDK_NGX_Handle* handle = nullptr;
			NVSDK_NGX_DLSSD_Create_Params create = {};
			create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
			create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
			create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
			create.InWidth = renderWidth;
			create.InHeight = renderHeight;
			create.InTargetWidth = displayWidth;
			create.InTargetHeight = displayHeight;
			create.InPerfQualityValue = level.Value;
			create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
			NVSDK_NGX_Result created = NVSDK_NGX_Result_Fail;
			run([&](VulkanCommandBuffer* commands)
			{
				created = NGX_VULKAN_CREATE_DLSSD_EXT1(device->device, commands->buffer, 1, 1, &handle, params, &create);
			});
			if (NVSDK_NGX_FAILED(created))
			{
				printf("   %-11s render %ux%u: create failed, %s\n", level.Name, renderWidth, renderHeight, ResultName(created));
				allGood = false;
				continue;
			}

			// Warm up, then time: one evaluation a submission, as a frame would.
			const int warmup = 10, timed = 60;
			double totalMs = 0.0;
			NVSDK_NGX_Result evaluated = NVSDK_NGX_Result_Success;
			for (int frame = 0; frame < warmup + timed; frame++)
			{
				NVSDK_NGX_Resource_VK inColor = color.Resource(false), inDiffuse = diffuse.Resource(false), inSpecular = specular.Resource(false);
				NVSDK_NGX_Resource_VK inNormals = normals.Resource(false), inDepth = depth.Resource(false), inMotion = motion.Resource(false);
				NVSDK_NGX_Resource_VK outColor = output.Resource(true);
				NVSDK_NGX_VK_DLSSD_Eval_Params eval = {};
				eval.pInColor = &inColor;
				eval.pInDiffuseAlbedo = &inDiffuse;
				eval.pInSpecularAlbedo = &inSpecular;
				eval.pInNormals = &inNormals;
				eval.pInDepth = &inDepth;
				eval.pInMotionVectors = &inMotion;
				eval.pInOutput = &outColor;
				eval.InJitterOffsetX = Halton(frame, 2) - 0.5f;
				eval.InJitterOffsetY = Halton(frame, 3) - 0.5f;
				eval.InRenderSubrectDimensions = { renderWidth, renderHeight };
				eval.InReset = frame == 0 ? 1 : 0;
				eval.InMVScaleX = 1.0f;
				eval.InMVScaleY = 1.0f;
				eval.InFrameTimeDeltaInMsec = 8.0f;
				eval.InPreExposure = 1.0f;
				eval.InExposureScale = 1.0f;
				run([&](VulkanCommandBuffer* commands)
				{
					commands->resetQueryPool(timestamps.get(), 0, 2);
					Everything(commands);
					commands->writeTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps.get(), 0);
					NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(commands->buffer, handle, params, &eval);
					if (NVSDK_NGX_FAILED(result))
						evaluated = result;
					commands->writeTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps.get(), 1);
				});
				if (NVSDK_NGX_FAILED(evaluated))
					break;
				uint64_t t[2] = {};
				if (frame >= warmup && timestamps->getResults(0, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT))
					totalMs += (double)(t[1] - t[0]) * tickMs;
			}

			float out[4] = {};
			if (!NVSDK_NGX_FAILED(evaluated))
			{
				// The middle texel, which should be the colour it was given.
				run([&](VulkanCommandBuffer* commands)
				{
					Everything(commands);
					VkBufferImageCopy region = {};
					region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
					region.imageOffset = { (int32_t)displayWidth / 2, (int32_t)displayHeight / 2, 0 };
					region.imageExtent = { 1, 1, 1 };
					vkCmdCopyImageToBuffer(commands->buffer, output.Image->image, VK_IMAGE_LAYOUT_GENERAL, readback->buffer, 1, &region);
				});
				const uint16_t* half = (const uint16_t*)readback->Map(0, 8);
				for (int c = 0; c < 4; c++)
					out[c] = HalfToFloat(half[c]);
				readback->Unmap();
			}

			bool close = true;
			for (int c = 0; c < 3; c++)
				close &= std::isfinite(out[c]) && std::fabs(out[c] - colour[c]) < 0.1f * colour[c] + 0.02f;
			if (NVSDK_NGX_FAILED(evaluated))
				printf("   %-11s render %ux%u: evaluate failed, %s\n", level.Name, renderWidth, renderHeight, ResultName(evaluated));
			else
				printf("   %-11s render %4ux%-4u  %.2f ms a frame on the GPU, output %.3f %.3f %.3f (given %.3f %.3f %.3f) %s\n",
					level.Name, renderWidth, renderHeight, totalMs / timed, out[0], out[1], out[2], colour[0], colour[1], colour[2],
					close ? "ok" : "WRONG");
			allGood &= !NVSDK_NGX_FAILED(evaluated) && close;

			vkDeviceWaitIdle(device->device);
			NVSDK_NGX_VULKAN_ReleaseFeature(handle);
		}

		NVSDK_NGX_VULKAN_DestroyParameters(params);
		NVSDK_NGX_VULKAN_Shutdown1(device->device);
		printf("\n=> %s\n", allGood ? "YES: Ray Reconstruction runs here from a 64-bit process" : "PARTLY: see above");
		return allGood ? 0 : 1;
	}
	catch (const std::exception& e)
	{
		printf("=> FAILED: %s\n", e.what());
		return 1;
	}
}
