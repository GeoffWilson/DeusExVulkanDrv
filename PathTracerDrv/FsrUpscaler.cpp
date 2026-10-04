#include "TracePrecomp.h"
#include "FsrUpscaler.h"

void HelperLog(const char* format, ...);

#ifdef PATHTRACER_FSR

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <cmath>
#include <vector>

// The backend's table of functions names frame generation's swap chain,
// which is built with its own sources; nothing here generates frames.
extern "C" FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*)
{
	return FFX_ERROR_INVALID_ARGUMENT;
}

struct FsrUpscaler::Impl
{
	std::vector<uint8_t> Scratch;
	FfxInterface Backend = {};
	struct View
	{
		FfxFsr3UpscalerContext Context = {};
		bool Made = false;
		uint32_t Render[2] = {}, Output[2] = {};
	};
	View Views[MaxViews];
};

static FfxFsr3UpscalerQualityMode QualityMode(int quality)
{
	switch (quality)
	{
	case 0: return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
	case 2: return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
	case 3: return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
	case 4: return FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
	default: return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
	}
}

void FsrUpscaler::RenderSize(uint32_t width, uint32_t height, int quality, uint32_t& renderWidth, uint32_t& renderHeight)
{
	renderWidth = width;
	renderHeight = height;
	if (ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&renderWidth, &renderHeight, width, height, QualityMode(quality)) != FFX_OK)
	{
		renderWidth = width;
		renderHeight = height;
	}
	renderWidth = std::max(renderWidth, 1u);
	renderHeight = std::max(renderHeight, 1u);
}

vec2 FsrUpscaler::Jitter(uint32_t frame, uint32_t renderWidth, uint32_t outputWidth)
{
	const int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount((int32_t)renderWidth, (int32_t)outputWidth);
	float x = 0.0f, y = 0.0f;
	if (phases > 0)
		ffxFsr3UpscalerGetJitterOffset(&x, &y, (int32_t)(frame % (uint32_t)phases), phases);
	return vec2(x, y);
}

FsrUpscaler::FsrUpscaler(VulkanDevice* device) : Device(device), I(new Impl())
{
	VkDeviceContext context = {};
	context.vkDevice = device->device;
	context.vkPhysicalDevice = device->PhysicalDevice.Device;
	context.vkDeviceProcAddr = vkGetDeviceProcAddr;
	const size_t size = ffxGetScratchMemorySizeVK(context.vkPhysicalDevice, FFX_FSR3UPSCALER_CONTEXT_COUNT * MaxViews);
	I->Scratch.resize(size);
	const FfxErrorCode r = ffxGetInterfaceVK(&I->Backend, ffxGetDeviceVK(&context), I->Scratch.data(), size, FFX_FSR3UPSCALER_CONTEXT_COUNT * MaxViews);
	if (r != FFX_OK)
	{
		StatusText = "the FidelityFX Vulkan backend did not start (" + std::to_string((int)r) + ")";
		return;
	}
	const FfxVersionNumber version = ffxFsr3UpscalerGetEffectVersion();
	StatusText = "FSR " + std::to_string((version >> 22) & 1023) + "." + std::to_string((version >> 12) & 1023) + "." + std::to_string(version & 4095) + " ready";
	Ready = true;
}

FsrUpscaler::~FsrUpscaler()
{
	for (int v = 0; v < MaxViews; v++)
		ReleaseContext(v);
}

void FsrUpscaler::ReleaseContext(int view)
{
	Impl::View& v = I->Views[view];
	if (v.Made)
		ffxFsr3UpscalerContextDestroy(&v.Context);
	v.Made = false;
}

bool FsrUpscaler::NeedsContext(int view, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight) const
{
	if (!Ready || view < 0 || view >= MaxViews)
		return false;
	const Impl::View& v = I->Views[view];
	return !v.Made || v.Render[0] != renderWidth || v.Render[1] != renderHeight || v.Output[0] != outputWidth || v.Output[1] != outputHeight;
}

// An image of ours as FidelityFX takes one: in GENERAL, which it calls
// unordered access, and which it leaves it in after the dispatch.
static FfxResource Resource(const FsrUpscaler::Target& target, uint32_t width, uint32_t height, bool written, const wchar_t* name)
{
	FfxResourceDescription description = {};
	description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
	description.format = ffxGetSurfaceFormatVK((VkFormat)target.Format);
	description.width = width;
	description.height = height;
	description.depth = 1;
	description.mipCount = 1;
	description.flags = FFX_RESOURCE_FLAGS_NONE;
	description.usage = written ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
	return ffxGetResourceVK((void*)target.Image->image, description, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool FsrUpscaler::Evaluate(VulkanCommandBuffer* commands, int view, const Inputs& inputs,
	uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
	vec2 jitter, bool reset, float frameMs, float nearPlane, float fovY, float unitsPerMetre, float sharpness)
{
	if (!Ready || view < 0 || view >= MaxViews || !inputs.Color.Image || !inputs.Depth.Image || !inputs.Motion.Image || !inputs.Output.Image)
		return false;

	Impl::View& v = I->Views[view];
	if (NeedsContext(view, renderWidth, renderHeight, outputWidth, outputHeight))
	{
		ReleaseContext(view);
		// The picture comes finished, tonemapped into 0..1; the depth as
		// near / z, so nearer is larger and there is no far plane; the
		// motion with the jitter left out.
		FfxFsr3UpscalerContextDescription description = {};
		description.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
		description.maxRenderSize = { renderWidth, renderHeight };
		description.maxUpscaleSize = { outputWidth, outputHeight };
		description.backendInterface = I->Backend;
		const FfxErrorCode r = ffxFsr3UpscalerContextCreate(&v.Context, &description);
		if (r != FFX_OK)
		{
			if (!LoggedFailure)
				HelperLog("PathTracer FSR: no context for %ux%u to %ux%u (%d)", renderWidth, renderHeight, outputWidth, outputHeight, (int)r);
			LoggedFailure = true;
			return false;
		}
		v.Made = true;
		v.Render[0] = renderWidth;
		v.Render[1] = renderHeight;
		v.Output[0] = outputWidth;
		v.Output[1] = outputHeight;
		reset = true;
		HelperLog("PathTracer FSR: %ux%u to %ux%u", renderWidth, renderHeight, outputWidth, outputHeight);
	}

	FfxFsr3UpscalerDispatchDescription dispatch = {};
	dispatch.commandList = ffxGetCommandListVK(commands->buffer);
	dispatch.color = Resource(inputs.Color, renderWidth, renderHeight, false, L"PathTracerFsrColor");
	dispatch.depth = Resource(inputs.Depth, renderWidth, renderHeight, false, L"PathTracerFsrDepth");
	dispatch.motionVectors = Resource(inputs.Motion, renderWidth, renderHeight, false, L"PathTracerFsrMotion");
	dispatch.output = Resource(inputs.Output, outputWidth, outputHeight, true, L"PathTracerFsrOutput");
	// Told the picture moved the other way from the rays, as DLSS is: both
	// take the jitter as the projection's offset. Measured: a still scene
	// changed 0.0002 a frame this way round and 0.0005 the other
	// (PathTracerHelperTest --fsr 1 --still).
	dispatch.jitterOffset = { -jitter.x, -jitter.y };
	// The motion is in screens; FSR wants pixels.
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
	const FfxErrorCode r = ffxFsr3UpscalerContextDispatch(&v.Context, &dispatch);
	if (r != FFX_OK)
	{
		if (!LoggedFailure)
			HelperLog("PathTracer FSR failed to run (%d)", (int)r);
		LoggedFailure = true;
		return false;
	}
	return true;
}

#else

struct FsrUpscaler::Impl {};
FsrUpscaler::FsrUpscaler(VulkanDevice* device) : Device(device) { StatusText = "built without the FidelityFX SDK (see cmake/fetch-fsr.sh)"; }
FsrUpscaler::~FsrUpscaler() {}
void FsrUpscaler::ReleaseContext(int) {}
void FsrUpscaler::RenderSize(uint32_t width, uint32_t height, int, uint32_t& renderWidth, uint32_t& renderHeight) { renderWidth = width; renderHeight = height; }
vec2 FsrUpscaler::Jitter(uint32_t, uint32_t, uint32_t) { return vec2(0.0f, 0.0f); }
bool FsrUpscaler::NeedsContext(int, uint32_t, uint32_t, uint32_t, uint32_t) const { return false; }
bool FsrUpscaler::Evaluate(VulkanCommandBuffer*, int, const Inputs&, uint32_t, uint32_t, uint32_t, uint32_t, vec2, bool, float, float, float, float, float) { return false; }

#endif
