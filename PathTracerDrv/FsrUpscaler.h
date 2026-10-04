#pragma once

#include "vec.h"
#include <cstdint>
#include <memory>
#include <string>

class VulkanDevice;
class VulkanCommandBuffer;
class VulkanImage;

// AMD's FSR 3.1 upscaler, from FidelityFX SDK v1.1.4 (cmake/fetch-fsr.sh):
// for a GPU Ray Reconstruction cannot run on, the trace and NRD run at a
// fraction of the screen's size and this brings the finished picture up to it.
//
// It takes the picture as the composite finished it - tonemapped, the fog and
// the flash over it - with the depth and the motion the composite writes
// beside it (see Shaders::Composite), and the primary rays offset within
// their pixels by the sequence it asks for (Jitter). A context of its own for
// each view, the screen's or a headset's two eyes, each with its history.
//
// Built only when the SDK has been fetched and its shaders compiled; otherwise
// Available() says so.
class FsrUpscaler
{
public:
	explicit FsrUpscaler(VulkanDevice* device);
	~FsrUpscaler();

	bool Available() const { return Ready; }
	const char* Status() const { return StatusText.c_str(); }

	// Qualities as the device's DLSSQuality setting numbers them, which FSR's
	// own follow: 0 native (antialiasing alone), 1 quality (1.5x a side), 2
	// balanced (1.7x), 3 performance (2x), 4 ultra performance (3x).
	static void RenderSize(uint32_t width, uint32_t height, int quality, uint32_t& renderWidth, uint32_t& renderHeight);

	// The offset within its pixel each frame's primary rays are to take, in
	// render pixels, -0.5..0.5: FSR's Halton sequence, as long a cycle as the
	// scaling wants.
	static vec2 Jitter(uint32_t frame, uint32_t renderWidth, uint32_t outputWidth);

	struct Target
	{
		VulkanImage* Image = nullptr;
		int Format = 0;   // VkFormat
	};

	struct Inputs
	{
		Target Color;    // render size: the finished picture
		Target Depth;    // render size: near / view z, so inverted and infinite
		Target Motion;   // render size: screen units, from this frame to the last
		Target Output;   // output size, written
	};

	static const int MaxViews = 2;

	// Records one frame's upscale for a view, its context made (again) first
	// if the sizes have changed - the caller must have waited for the frames
	// in flight when they have (NeedsContext). Every image is in GENERAL and
	// is left there. jitter is what Jitter gave this frame; nearPlane is the near
	// plane the depth was written against; fovY the view's vertical field of
	// view in radians; unitsPerMetre the world's scale.
	bool Evaluate(VulkanCommandBuffer* commands, int view, const Inputs& inputs,
		uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
		vec2 jitter, bool reset, float frameMs, float nearPlane, float fovY, float unitsPerMetre, float sharpness);

	bool NeedsContext(int view, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight) const;

	// What the device needs from FidelityFX's private state, which only
	// FsrUpscaler.cpp sees.
	struct Impl;

private:
	void ReleaseContext(int view);

	VulkanDevice* Device = nullptr;
	std::unique_ptr<Impl> I;
	bool Ready = false;
	std::string StatusText = "not started";
	bool LoggedFailure = false;
};
