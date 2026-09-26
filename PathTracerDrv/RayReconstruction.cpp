#include "TracePrecomp.h"
#include "RayReconstruction.h"

vec2 RayReconstruction::Jitter(uint32_t frame)
{
	auto halton = [](uint32_t index, uint32_t base)
	{
		float f = 1.0f, r = 0.0f;
		for (uint32_t i = index + 1; i > 0; i /= base)
		{
			f /= (float)base;
			r += f * (float)(i % base);
		}
		return r;
	};
	// A cycle long enough to cover a pixel at the smallest render size.
	const uint32_t index = frame % 64;
	return vec2(halton(index, 2) - 0.5f, halton(index, 3) - 0.5f);
}

#ifdef PATHTRACER_DLSS

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

// Any id serves outside a shipping title; NVIDIA uses it for per title tuning.
static const unsigned long long NgxAppId = 0x5054485244584e47ull;

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
	default: return "unknown";
	}
}

static NVSDK_NGX_PerfQuality_Value NgxQuality(int quality)
{
	switch (quality)
	{
	case RayReconstruction::DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
	case RayReconstruction::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
	case RayReconstruction::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	case RayReconstruction::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

// NGX's own log, into the helper's. Only what NGX thinks worth saying by
// default: where it found its core, and why it failed when it did.
static void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
	std::string line = message;
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		line.pop_back();
	HelperLog("NGX: %s", line.c_str());
}

void RayReconstruction::RequiredExtensions(std::vector<std::string>& instance, std::vector<std::string>& device)
{
	unsigned int instanceCount = 0, deviceCount = 0;
	const char** instanceExts = nullptr;
	const char** deviceExts = nullptr;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instanceExts, &deviceCount, &deviceExts)))
		return;
	for (unsigned int i = 0; i < instanceCount; i++)
		instance.push_back(instanceExts[i]);
	for (unsigned int i = 0; i < deviceCount; i++)
		device.push_back(deviceExts[i]);
}

RayReconstruction::RayReconstruction(VulkanDevice* device, const std::wstring& dataPath) : Device(device)
{
	NVSDK_NGX_FeatureCommonInfo info = {};
	info.LoggingInfo.LoggingCallback = NgxLog;
	info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	info.LoggingInfo.DisableOtherLoggingSinks = true;
	NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init(NgxAppId, dataPath.c_str(), device->Instance->Instance, device->PhysicalDevice.Device, device->device,
		vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
	if (NVSDK_NGX_FAILED(r))
	{
		// Under wine the usual reason is a piece of the environment missing:
		// see spike/README.md for which.
		StatusText = std::string("NGX did not start: ") + ResultName(r);
		return;
	}
	Initialised = true;

	NVSDK_NGX_Parameter* params = nullptr;
	r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
	if (NVSDK_NGX_FAILED(r) || !params)
	{
		StatusText = std::string("NGX has no capabilities to report: ") + ResultName(r);
		return;
	}

	int available = 0, needsDriver = 0, initResult = 0;
	unsigned int minMajor = 0, minMinor = 0;
	NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
	if (!available)
	{
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &minMajor);
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minMinor);
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &initResult);
		char why[160];
		if (needsDriver)
			snprintf(why, sizeof(why), "Ray Reconstruction needs driver %u.%u or later", minMajor, minMinor);
		else
			snprintf(why, sizeof(why), "Ray Reconstruction is not offered here (%s; is nvngx_dlssd.dll beside the helper?)", ResultName((NVSDK_NGX_Result)initResult));
		StatusText = why;
		NVSDK_NGX_VULKAN_DestroyParameters(params);
		return;
	}

	Params = params;
	StatusText = "ready";
}

RayReconstruction::~RayReconstruction()
{
	ReleaseFeature();
	if (Params)
		NVSDK_NGX_VULKAN_DestroyParameters((NVSDK_NGX_Parameter*)Params);
	if (Initialised)
		NVSDK_NGX_VULKAN_Shutdown1(Device->device);
}

void RayReconstruction::ReleaseFeature()
{
	if (Handle)
		NVSDK_NGX_VULKAN_ReleaseFeature((NVSDK_NGX_Handle*)Handle);
	Handle = nullptr;
	FeatureQuality = -1;
}

// NGX_DLSSD_GET_OPTIMAL_SETTINGS, which the SDK offers only among its D3D
// helpers. Ray Reconstruction has its own callback: the plain DLSS one answers
// OutOfDate here.
void RayReconstruction::RenderSize(uint32_t width, uint32_t height, int quality, uint32_t& renderWidth, uint32_t& renderHeight)
{
	renderWidth = width;
	renderHeight = height;
	if (!Params)
		return;
	NVSDK_NGX_Parameter* params = (NVSDK_NGX_Parameter*)Params;
	void* callback = nullptr;
	NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &callback);
	if (!callback)
		return;
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, width);
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, height);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, NgxQuality(quality));
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_RTXValue, 0);
	if (NVSDK_NGX_FAILED(((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback)(params)))
		return;
	unsigned int w = 0, h = 0;
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutWidth, &w);
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutHeight, &h);
	if (w > 0 && h > 0)
	{
		renderWidth = w;
		renderHeight = h;
	}
}

bool RayReconstruction::NeedsFeature(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight, int quality) const
{
	return !Handle || FeatureQuality != quality ||
		FeatureRender[0] != renderWidth || FeatureRender[1] != renderHeight ||
		FeatureOutput[0] != outputWidth || FeatureOutput[1] != outputHeight;
}

static NVSDK_NGX_Resource_VK Resource(const RayReconstruction::Target& t, bool readWrite)
{
	return NVSDK_NGX_Create_ImageView_Resource_VK(t.View->view, t.Image->image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		(VkFormat)t.Format, (unsigned int)t.Image->width, (unsigned int)t.Image->height, readWrite);
}

bool RayReconstruction::Evaluate(VulkanCommandBuffer* commands, const Inputs& in,
	uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight, int quality,
	vec2 jitter, bool reset, float frameMs)
{
	if (!Params)
		return false;
	NVSDK_NGX_Parameter* params = (NVSDK_NGX_Parameter*)Params;

	if (NeedsFeature(renderWidth, renderHeight, outputWidth, outputHeight, quality))
	{
		ReleaseFeature();
		NVSDK_NGX_DLSSD_Create_Params create = {};
		create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
		// Roughness rides in the normals' w.
		create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
		// There is no rasteriser and so no hardware depth: view z straight
		// from the trace, which it would rather have anyway.
		create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
		create.InWidth = renderWidth;
		create.InHeight = renderHeight;
		create.InTargetWidth = outputWidth;
		create.InTargetHeight = outputHeight;
		create.InPerfQualityValue = NgxQuality(quality);
		// The colour is linear and well above 1 at a lamp; the motion is at
		// the render size.
		create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
		NVSDK_NGX_Handle* handle = nullptr;
		NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(Device->device, commands->buffer, 1, 1, &handle, params, &create);
		if (NVSDK_NGX_FAILED(r))
		{
			StatusText = std::string("Ray Reconstruction could not be made: ") + ResultName(r);
			HelperLog("PathTracer %s", StatusText.c_str());
			NVSDK_NGX_VULKAN_DestroyParameters(params);
			Params = nullptr;
			return false;
		}
		Handle = handle;
		FeatureRender[0] = renderWidth;
		FeatureRender[1] = renderHeight;
		FeatureOutput[0] = outputWidth;
		FeatureOutput[1] = outputHeight;
		FeatureQuality = quality;
		HelperLog("PathTracer Ray Reconstruction: %ux%u to %ux%u", renderWidth, renderHeight, outputWidth, outputHeight);
		// It is always going to reject the history on its first frame.
		reset = true;
	}

	NVSDK_NGX_Resource_VK color = Resource(in.Color, false);
	NVSDK_NGX_Resource_VK diffuse = Resource(in.DiffuseAlbedo, false);
	NVSDK_NGX_Resource_VK specular = Resource(in.SpecularAlbedo, false);
	NVSDK_NGX_Resource_VK normals = Resource(in.NormalRoughness, false);
	NVSDK_NGX_Resource_VK depth = Resource(in.Depth, false);
	NVSDK_NGX_Resource_VK motion = Resource(in.Motion, false);
	NVSDK_NGX_Resource_VK output = Resource(in.Output, true);

	NVSDK_NGX_VK_DLSSD_Eval_Params eval = {};
	eval.pInColor = &color;
	eval.pInDiffuseAlbedo = &diffuse;
	eval.pInSpecularAlbedo = &specular;
	eval.pInNormals = &normals;
	eval.pInDepth = &depth;
	eval.pInMotionVectors = &motion;
	eval.pInOutput = &output;
	// Exactly the offset the primary rays were given: told anything else it
	// puts sub-pixel detail in the wrong place, and nothing says why.
	eval.InJitterOffsetX = jitter.x;
	eval.InJitterOffsetY = jitter.y;
	eval.InRenderSubrectDimensions = { renderWidth, renderHeight };
	eval.InReset = reset ? 1 : 0;
	// The motion is in screen widths and heights; it wants render pixels.
	eval.InMVScaleX = (float)renderWidth;
	eval.InMVScaleY = (float)renderHeight;
	eval.InFrameTimeDeltaInMsec = frameMs;
	eval.InPreExposure = 1.0f;
	eval.InExposureScale = 1.0f;
	NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT(commands->buffer, (NVSDK_NGX_Handle*)Handle, params, &eval);
	if (NVSDK_NGX_FAILED(r))
	{
		if (!LoggedEvaluateFailure)
		{
			LoggedEvaluateFailure = true;
			HelperLog("PathTracer Ray Reconstruction failed to run: %s", ResultName(r));
		}
		return false;
	}
	return true;
}

#else

void RayReconstruction::RequiredExtensions(std::vector<std::string>&, std::vector<std::string>&) {}
RayReconstruction::RayReconstruction(VulkanDevice* device, const std::wstring&) : Device(device) { StatusText = "built without the DLSS SDK (see cmake/fetch-dlss.sh)"; }
RayReconstruction::~RayReconstruction() {}
void RayReconstruction::ReleaseFeature() {}
void RayReconstruction::RenderSize(uint32_t width, uint32_t height, int, uint32_t& renderWidth, uint32_t& renderHeight) { renderWidth = width; renderHeight = height; }
bool RayReconstruction::NeedsFeature(uint32_t, uint32_t, uint32_t, uint32_t, int) const { return false; }
bool RayReconstruction::Evaluate(VulkanCommandBuffer*, const Inputs&, uint32_t, uint32_t, uint32_t, uint32_t, int, vec2, bool, float) { return false; }

#endif
