#pragma once

#include "vec.h"
#include <memory>
#include <vector>

class VulkanDevice;
class VulkanCommandBuffer;
class VulkanImage;
class VulkanImageView;
class VulkanBuffer;
class VulkanSampler;

// NVIDIA's NRD, running its ReLAX denoiser over the trace's diffuse lighting,
// twice: once for the surfaces seen, and once for the surfaces seen in mirrors.
//
// Not its specular denoiser. Reflections here are mirrors, and ReLAX will not
// blur a mirror - rightly - so it has nothing to work with but its history,
// which it keeps clamping back towards each noisy frame. Instead what a mirror
// shows is described as a surface of its own, where it appears to be behind
// the glass, and denoised as diffuse lighting like any other.
//
// Driven directly in Vulkan rather than through NRD's own integration layer,
// which needs NRI: NRD hands back a list of compute dispatches each frame, and
// this runs them against its own textures and the trace's images.
//
// Only built when the library is there to link (see cmake/build-nrd.sh);
// otherwise Available() says so and nothing else does anything.
class Denoiser
{
public:
	Denoiser(VulkanDevice* device);
	~Denoiser();

	bool Available() const { return Instance != nullptr; }

	// The trace's resolution. Recreates NRD's own textures and restarts its
	// history when it changes.
	void Resize(int width, int height);

	// The camera as the trace sees it: eye position, and the right, up and
	// forward vectors scaled to the edges of the view, with "up" pointing down
	// the screen.
	struct Camera
	{
		vec3 Origin, Right, Up, Forward;
	};

	// The trace's images for one signal, all in GENERAL layout.
	struct Inputs
	{
		VulkanImageView* NormalRoughness = nullptr;   // NRD's own packing
		VulkanImageView* ViewZ = nullptr;             // view z in x
		VulkanImageView* Motion = nullptr;            // motion in uv in xy, 0 in z
		VulkanImageView* Diffuse = nullptr;           // demodulated radiance, hit distance
	};

	// The two signals: what is seen, and what is seen in mirrors.
	static const int SignalCount = 2;

	// Records this frame's denoising. Restarts the history when told to, as
	// after a level change or a camera cut.
	void Denoise(VulkanCommandBuffer* commands, const Inputs (&inputs)[SignalCount], const Camera& now, const Camera& previous, bool restart);

	// Denoised, demodulated radiance for a signal, RGBA16F, GENERAL layout.
	VulkanImageView* Output(int signal) const;

	// Why Available() is false, for the log.
	const char* Problem() const { return Status; }

private:
	struct Impl;
	std::unique_ptr<Impl> I;
	void* Instance = nullptr;
	const char* Status = "not built";
};
