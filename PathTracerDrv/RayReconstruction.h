#pragma once

#include "vec.h"
#include <cstdint>
#include <string>
#include <vector>

class VulkanDevice;
class VulkanCommandBuffer;
class VulkanImage;
class VulkanImageView;

// NVIDIA's DLSS Ray Reconstruction: a denoiser and an upscaler in one network,
// in place of NRD and the composite after it.
//
// It wants the noisy picture as the trace composed it, not the lighting split
// from the colour as NRD does, and it works it back out from the albedos it
// is given. So the trace writes a different set of images for it (see
// Shaders::Trace, Disable bit 256), at a render size smaller than the output
// when upscaling, and a finish pass tonemaps what comes back and puts the fog
// and the screen flash over it at the output's size.
//
// NGX, which it runs on, is 64-bit only: this exists because the tracing is in
// the helper. Under wine it also needs the driver's NGX core, dxvk-nvapi and
// DXVK, which Proton provides; see spike/README.md. Built only when
// cmake/fetch-dlss.sh has fetched the SDK; otherwise Available() says so.
class RayReconstruction
{
public:
	// NGX's Vulkan extensions, asked before the device exists, and enabled
	// wherever they are offered.
	static void RequiredExtensions(std::vector<std::string>& instance, std::vector<std::string>& device);

	// Starts NGX on the device. Its feature DLL, nvngx_dlssd.dll, is looked
	// for beside the executable; dataPath is where NGX may write.
	RayReconstruction(VulkanDevice* device, const std::wstring& dataPath);
	~RayReconstruction();

	bool Available() const { return Params != nullptr; }
	const char* Status() const { return StatusText.c_str(); }

	// Qualities as the device's DLSSQuality setting numbers them.
	enum Quality { DLAA, MaxQuality, Balanced, Performance, UltraPerformance };

	// The size to trace at for an output of width x height: not a choice, but
	// what the network was trained for at that quality.
	void RenderSize(uint32_t width, uint32_t height, int quality, uint32_t& renderWidth, uint32_t& renderHeight);

	struct Target
	{
		VulkanImage* Image = nullptr;
		VulkanImageView* View = nullptr;
		int Format = 0;   // VkFormat
	};

	struct Inputs
	{
		Target Color;           // noisy, linear, exposure applied, render size
		Target DiffuseAlbedo;
		Target SpecularAlbedo;
		Target NormalRoughness; // world normal, roughness in w
		Target Depth;           // linear view z
		Target Motion;          // screen units, from this frame to the last
		Target Output;          // output size, written
	};

	// Records one frame's denoise and upscale, made (again) first if the sizes
	// or the quality have changed - the caller must have waited for the
	// frames in flight when they have. jitter is the offset the primary rays
	// were given, in render pixels.
	bool Evaluate(VulkanCommandBuffer* commands, const Inputs& inputs,
		uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight, int quality,
		vec2 jitter, bool reset, float frameMs);

	// Whether Evaluate would have to make the feature again for these.
	bool NeedsFeature(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight, int quality) const;

	// The Halton (2, 3) sequence it expects the jitter to follow, in -0.5..0.5.
	static vec2 Jitter(uint32_t frame);

private:
	void ReleaseFeature();

	VulkanDevice* Device = nullptr;
	void* Params = nullptr;    // NVSDK_NGX_Parameter*
	void* Handle = nullptr;    // NVSDK_NGX_Handle*
	bool Initialised = false;
	uint32_t FeatureRender[2] = {}, FeatureOutput[2] = {};
	int FeatureQuality = -1;
	std::string StatusText = "not started";
	bool LoggedEvaluateFailure = false;
};
