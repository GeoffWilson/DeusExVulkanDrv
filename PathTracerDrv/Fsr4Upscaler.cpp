#include "TracePrecomp.h"
#include "Fsr4Upscaler.h"

void HelperLog(const char* format, ...);

#ifdef PATHTRACER_FSR4

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <vk/ffx_api_vk.h>
#include <ffx_upscale.h>
#include <algorithm>
#include <cstdlib>

void Fsr4Upscaler::RequiredExtensions(std::vector<std::string>& device)
{
	// What amd_fidelityfx_upscaler_vk.dll names: the matrix maths its models
	// run on, int8 (RDNA 2 and 3) or fp8 (RDNA 4), and the wave sizes it
	// picks. ZVulkan turns on all of each one's features the device has.
	for (const char* name : {
		VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
		VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
		VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
		VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
		VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
		VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
		VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
		VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME })
		device.push_back(name);
}

struct Fsr4Upscaler::Impl
{
	HMODULE Loader = nullptr;
	ffxFunctions Ffx = {};
	VkDeviceContext DeviceContext = {};
	uint64_t VersionId = 0;
	std::string VersionName;
	struct View
	{
		ffxContext Context = nullptr;
		uint32_t Render[2] = {}, Output[2] = {};
	};
	View Views[FsrUpscaler::MaxViews];
};

static void FfxMessage(uint32_t type, const wchar_t* message)
{
	char text[1024];
	size_t n = 0;
	wcstombs_s(&n, text, sizeof(text), message, _TRUNCATE);
	HelperLog("PathTracer FSR 4 %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

Fsr4Upscaler::Fsr4Upscaler(VulkanDevice* device, const std::wstring& dllDir) : Device(device), I(new Impl())
{
	if (!device->SupportsExtension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
	{
		StatusText = "this GPU has no cooperative matrices (VK_KHR_cooperative_matrix)";
		return;
	}
	const std::wstring path = dllDir + L"\\amd_fidelityfx_loader_vk.dll";
	I->Loader = LoadLibraryW(path.c_str());
	if (!I->Loader)
	{
		StatusText = "amd_fidelityfx_loader_vk.dll is not beside the program (" + std::to_string((unsigned long)GetLastError()) + ")";
		return;
	}
	ffxLoadFunctions(&I->Ffx, I->Loader);
	if (!I->Ffx.CreateContext || !I->Ffx.DestroyContext || !I->Ffx.Query || !I->Ffx.Dispatch || !I->Ffx.Configure)
	{
		StatusText = "amd_fidelityfx_loader_vk.dll lacks the FidelityFX API";
		return;
	}

	I->DeviceContext.vkDevice = device->device;
	I->DeviceContext.vkPhysicalDevice = device->PhysicalDevice.Device;
	I->DeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

	// The upscalers the DLLs - or AMD's driver - offer for this GPU, newest
	// first. FSR 4 is among them only when asked about the device.
	ffxQueryDescGetVersions query = {};
	query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	query.device = &I->DeviceContext;
	uint64_t count = 0;
	query.outputCount = &count;
	I->Ffx.Query(nullptr, &query.header);
	std::vector<uint64_t> ids((size_t)count);
	std::vector<const char*> names((size_t)count);
	query.versionIds = ids.data();
	query.versionNames = names.data();
	if (count == 0 || I->Ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK)
	{
		StatusText = "AMD's DLLs offer no upscaler for this GPU";
		return;
	}
	std::string offered;
	for (size_t i = 0; i < (size_t)count; i++)
	{
		const char* name = names[i] ? names[i] : "?";
		offered += (i ? ", " : "") + std::string(name);
		if (!I->VersionId && std::atoi(name) >= 4)
		{
			I->VersionId = ids[i];
			I->VersionName = name;
		}
	}
	if (!I->VersionId)
	{
		StatusText = "AMD's DLLs offer no FSR 4 for this GPU (only " + offered + ")";
		return;
	}
	HelperLog("PathTracer FSR 4: AMD's DLLs offer %s", offered.c_str());
	StatusText = "FSR " + I->VersionName + " ready";
	Ready = true;
}

Fsr4Upscaler::~Fsr4Upscaler()
{
	for (int v = 0; v < FsrUpscaler::MaxViews; v++)
		ReleaseContext(v);
	// The loader is left loaded: its effect DLL may still be tidying up
	// threads of its own, and the process is ending soon enough.
	delete I;
}

void Fsr4Upscaler::ReleaseContext(int view)
{
	Impl::View& v = I->Views[view];
	if (v.Context)
		I->Ffx.DestroyContext(&v.Context, nullptr);
	v.Context = nullptr;
}

bool Fsr4Upscaler::NeedsContext(int view, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight) const
{
	if (!Ready || view < 0 || view >= FsrUpscaler::MaxViews)
		return false;
	const Impl::View& v = I->Views[view];
	return !v.Context || v.Render[0] != renderWidth || v.Render[1] != renderHeight || v.Output[0] != outputWidth || v.Output[1] != outputHeight;
}

// An image of ours as the FidelityFX API takes one. Its inputs it wants in
// SHADER_READ_ONLY_OPTIMAL, which it calls compute read, and the output in
// GENERAL, unordered access; it leaves each as it found it.
static FfxApiResource Resource(const FsrUpscaler::Target& target, uint32_t width, uint32_t height, bool written)
{
	FfxApiResourceDescription description = {};
	description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format = ffxApiGetSurfaceFormatVK((VkFormat)target.Format);
	description.width = width;
	description.height = height;
	description.depth = 1;
	description.mipCount = 1;
	description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	description.usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
	return ffxApiGetResourceVK((void*)target.Image->image, description,
		written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

bool Fsr4Upscaler::Evaluate(VulkanCommandBuffer* commands, int view, const FsrUpscaler::Inputs& inputs,
	uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
	vec2 jitter, bool reset, float frameMs, float nearPlane, float fovY, float unitsPerMetre, float sharpness)
{
	if (!Ready || view < 0 || view >= FsrUpscaler::MaxViews || !inputs.Color.Image || !inputs.Depth.Image || !inputs.Motion.Image || !inputs.Output.Image)
		return false;

	Impl::View& v = I->Views[view];
	if (NeedsContext(view, renderWidth, renderHeight, outputWidth, outputHeight))
	{
		ReleaseContext(view);
		// As FSR 3.1 is told: the picture comes finished, tonemapped; the
		// depth as near / z, so nearer is larger and there is no far plane.
		ffxCreateBackendVKDesc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
		backend.deviceContext = I->DeviceContext;
		ffxCreateContextDescUpscaleVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.header.pNext = &backend.header;
		version.version = FFX_UPSCALER_VERSION;
		ffxOverrideVersion which = {};
		which.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		which.header.pNext = &version.header;
		which.versionId = I->VersionId;
		ffxCreateContextDescUpscale create = {};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		create.header.pNext = &which.header;
		create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
		create.maxRenderSize = { renderWidth, renderHeight };
		create.maxUpscaleSize = { outputWidth, outputHeight };
		create.fpMessage = FfxMessage;
		const ffxReturnCode_t r = I->Ffx.CreateContext(&v.Context, &create.header, nullptr);
		if (r != FFX_API_RETURN_OK)
		{
			v.Context = nullptr;
			if (!LoggedFailure)
				HelperLog("PathTracer FSR 4: no context for %ux%u to %ux%u (%u)", renderWidth, renderHeight, outputWidth, outputHeight, (unsigned)r);
			LoggedFailure = true;
			return false;
		}
		v.Render[0] = renderWidth;
		v.Render[1] = renderHeight;
		v.Output[0] = outputWidth;
		v.Output[1] = outputHeight;
		reset = true;
		HelperLog("PathTracer FSR %s: %ux%u to %ux%u", I->VersionName.c_str(), renderWidth, renderHeight, outputWidth, outputHeight);
	}

	// Ours live in GENERAL; FSR 4 reads its inputs read-only.
	VulkanImage* read[] = { inputs.Color.Image, inputs.Depth.Image, inputs.Motion.Image };
	PipelineBarrier before;
	for (VulkanImage* image : read)
		before.AddImage(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	before.Execute(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	ffxDispatchDescUpscale dispatch = {};
	dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch.commandList = commands->buffer;
	dispatch.color = Resource(inputs.Color, renderWidth, renderHeight, false);
	dispatch.depth = Resource(inputs.Depth, renderWidth, renderHeight, false);
	dispatch.motionVectors = Resource(inputs.Motion, renderWidth, renderHeight, false);
	dispatch.output = Resource(inputs.Output, outputWidth, outputHeight, true);
	// As FSR 3.1: the jitter as the projection's offset, the motion in
	// screens, which FSR wants in pixels.
	dispatch.jitterOffset = { -jitter.x, -jitter.y };
	dispatch.motionVectorScale = { (float)renderWidth, (float)renderHeight };
	dispatch.renderSize = { renderWidth, renderHeight };
	dispatch.upscaleSize = { outputWidth, outputHeight };
	dispatch.enableSharpening = sharpness > 0.0f;
	dispatch.sharpness = std::min(std::max(sharpness, 0.0f), 1.0f);
	dispatch.frameTimeDelta = frameMs;
	dispatch.preExposure = 1.0f;
	dispatch.reset = reset;
	dispatch.cameraNear = nearPlane;
	dispatch.cameraFar = 1.0e7f;
	dispatch.cameraFovAngleVertical = fovY;
	dispatch.viewSpaceToMetersFactor = unitsPerMetre > 0.0f ? 1.0f / unitsPerMetre : 1.0f;
	const ffxReturnCode_t r = I->Ffx.Dispatch(&v.Context, &dispatch.header);

	PipelineBarrier after;
	for (VulkanImage* image : read)
		after.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	after.Execute(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	if (r != FFX_API_RETURN_OK)
	{
		if (!LoggedFailure)
			HelperLog("PathTracer FSR 4 failed to run (%u)", (unsigned)r);
		LoggedFailure = true;
		return false;
	}
	return true;
}

#else

struct Fsr4Upscaler::Impl {};
void Fsr4Upscaler::RequiredExtensions(std::vector<std::string>&) {}
Fsr4Upscaler::Fsr4Upscaler(VulkanDevice* device, const std::wstring&) : Device(device) { StatusText = "built without FSR 4 (see cmake/fetch-fsr4.sh)"; }
Fsr4Upscaler::~Fsr4Upscaler() {}
void Fsr4Upscaler::ReleaseContext(int) {}
bool Fsr4Upscaler::NeedsContext(int, uint32_t, uint32_t, uint32_t, uint32_t) const { return false; }
bool Fsr4Upscaler::Evaluate(VulkanCommandBuffer*, int, const FsrUpscaler::Inputs&, uint32_t, uint32_t, uint32_t, uint32_t, vec2, bool, float, float, float, float, float) { return false; }

#endif
