#pragma once

#include "FsrUpscaler.h"
#include <string>
#include <vector>

// AMD's FSR 4, from FidelityFX SDK v2.4.0 (cmake/fetch-fsr4.sh): the
// machine-learned upscaler, which FsrUpscaler hands its frames to in place of
// FSR 3.1 wherever it runs - an AMD GPU of RDNA 2 or later, its INT8 model on
// RDNA 2 and 3 and FP8 on RDNA 4. It comes only as AMD's signed DLLs,
// amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll, loaded
// from beside the program when the GPU is first asked about; on any other
// GPU, or without them, Available() says why and FSR 3.1 carries on.
//
// Its own file because the SDK's headers and FSR 3.1's (SDK v1.1.4) define
// the same Vulkan names differently.
class Fsr4Upscaler
{
public:
	Fsr4Upscaler(VulkanDevice* device, const std::wstring& dllDir);
	~Fsr4Upscaler();

	bool Available() const { return Ready; }
	const char* Status() const { return StatusText.c_str(); }

	// The device extensions its shaders use, to ask for wherever offered
	// when the device is made: there is no adding them afterwards.
	static void RequiredExtensions(std::vector<std::string>& device);

	// As FsrUpscaler's.
	bool NeedsContext(int view, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight) const;
	bool Evaluate(VulkanCommandBuffer* commands, int view, const FsrUpscaler::Inputs& inputs,
		uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
		vec2 jitter, bool reset, float frameMs, float nearPlane, float fovY, float unitsPerMetre, float sharpness);
	void ReleaseContext(int view);

	struct Impl;

private:
	VulkanDevice* Device = nullptr;
	Impl* I = nullptr;
	bool Ready = false;
	bool LoggedFailure = false;
	std::string StatusText;
};
