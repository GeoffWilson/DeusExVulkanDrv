#pragma once

#include "SceneData.h"
#include "GpuContext.h"
#include <memory>
#include <vector>

class AccelStructure;
class Denoiser;
namespace TraceProtocol { struct TraceCommand; }

// The trace shader's push constants: see Shaders::Trace.
struct TracePushConstants
{
	vec4 CameraOrigin;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	uint32_t Counts[4];   // frame, light count, bounces, accumulated frames
	vec4 Params;          // exposure, sky intensity, ray epsilon, debug mode
	// How many slots of the texture array hold a real texture. Zero means the
	// device could not offer descriptor indexing, and every surface falls back
	// to the single averaged colour it carries.
	uint32_t TextureCount;
	// The ceiling on how many samples one pixel may average. Sent separately
	// from the frame counter, which only says whether history is valid at all.
	uint32_t MaxSamples;
	// The level's clock in seconds, wrapped so it keeps its precision.
	float Time;
	// Diagnostic switches, set from the console with PT: see Exec.
	uint32_t Disable;
	// xyz where the sky zone is seen from; w is 1 when there is one.
	vec4 SkyOrigin;
};

// Everything that turns the scene into a picture on the GPU: the acceleration
// structures, the texture array and the materials, the trace, NRD and the pass
// that puts the picture back together from what it returns.
//
// It runs in the 64-bit helper, which keeps Scene as the render device sends
// it and asks for a frame at a time; nothing here knows the engine exists.
class TraceRenderer
{
public:
	TraceRenderer(GpuContext* context);
	~TraceRenderer();

	// The scene as the render device has described it so far.
	SceneData Scene;

	// A new level: every shape, texture and material goes.
	void ResetScene();

	// A texture array slot, made or remade: RGBA8 pixels, or none for one that
	// is bound white, and what surfaces using it are made of.
	void SetTexture(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels, const vec4& material);
	// New pixels for an existing slot, copied in as the next frame starts.
	void SetTexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels);

	// Records a frame: pending uploads, the top level structure, the trace,
	// and when denoising NRD and the composite. The picture ends up in Output,
	// in GENERAL layout. False when there is nothing to trace yet.
	bool Record(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame);

	// The GPU has finished what Record recorded: its staging can go, and its
	// timestamps can be read.
	void FrameCompleted();

	VulkanImage* Output() const { return OutputImage.get(); }
	int Width() const { return TraceWidth; }
	int Height() const { return TraceHeight; }
	bool SamplesTextures() const { return CanSampleTextures; }

	// About the last frame.
	int LightCount() const;
	int BottomCount() const;
	int TextureCount() const { return (int)BoundTextures; }
	bool DenoiserActive() const;
	const char* DenoiserStatus() const;
	bool GpuTimed = false;
	float GpuMs[4] = {};    // build, trace, denoise, composite

private:
	void CreateTracePipeline();
	void CreateCompositePipeline();
	void Resize(int width, int height);
	void EnsureDenoiser(bool wanted, bool materials);
	void UpdateDescriptors();
	void WriteCompositeDescriptors();
	void WriteMotion(const vec4 (&previousCamera)[4]);
	void RecordTexturePixels(VulkanCommandBuffer* commands);
	void BindWhite(uint32_t index);

	GpuContext* Context = nullptr;
	VulkanDevice* Device = nullptr;
	bool CanSampleTextures = false;

	std::unique_ptr<AccelStructure> Accel;

	std::unique_ptr<VulkanDescriptorSetLayout> DescriptorLayout;
	std::unique_ptr<VulkanDescriptorPool> DescriptorPool;
	std::unique_ptr<VulkanDescriptorSet> DescriptorSet;
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanShader> TraceShader;
	std::unique_ptr<VulkanPipeline> TracePipeline;
	bool DescriptorsDirty = true;

	std::unique_ptr<VulkanImage> AccumImage;
	std::unique_ptr<VulkanImageView> AccumView;
	std::unique_ptr<VulkanImage> HistoryImage;
	std::unique_ptr<VulkanImageView> HistoryView;
	std::unique_ptr<VulkanImage> OutputImage;
	std::unique_ptr<VulkanImageView> OutputView;
	int TraceWidth = 0;
	int TraceHeight = 0;

	// A denoiser's inputs, written by the trace at bindings 9 to 15 when asked
	// for, the fog at 17, what mirrors show at 18 and 19, and the glossy
	// reflection off a surface and its colour at 21 and 22: see the trace
	// shader.
	static const int GuideImageCount = 12;
	static int GuideBinding(int i) { return i < 7 ? 9 + i : (i < 10 ? 10 + i : 11 + i); }
	static bool GuideIsDepth(int i) { return i == 1 || i == 9; }
	std::unique_ptr<VulkanImage> GuideImages[GuideImageCount];
	std::unique_ptr<VulkanImageView> GuideViews[GuideImageCount];
	// The depth and motion images again, their motion moved to where NRD
	// reads it: the surfaces seen, and those seen in mirrors.
	std::unique_ptr<VulkanImageView> MotionView;
	std::unique_ptr<VulkanImageView> ReflectionMotionView;

	std::unique_ptr<Denoiser> Denoise;
	bool DenoiseFailed = false;
	bool DenoiseRestart = true;
	std::unique_ptr<VulkanDescriptorSetLayout> CompositeLayout;
	std::unique_ptr<VulkanDescriptorPool> CompositePool;
	std::unique_ptr<VulkanDescriptorSet> CompositeSet;
	std::unique_ptr<VulkanPipelineLayout> CompositePipelineLayout;
	std::unique_ptr<VulkanShader> CompositeShader;
	std::unique_ptr<VulkanPipeline> CompositePipeline;

	// Last frame's camera and each instance's last placement, for motion
	// vectors. Rewritten every frame.
	std::unique_ptr<VulkanBuffer> MotionBuffer;
	size_t MotionCapacity = 0;

	// The texture array: an image per slot the device has sent, the rest a 1x1
	// white image so every descriptor is valid whether or not it is read.
	static const int MaxTextures = 1024;
	struct Slot
	{
		std::unique_ptr<VulkanImage> Image;
		std::unique_ptr<VulkanImageView> View;
		uint32_t Width = 0, Height = 0;
	};
	std::vector<Slot> Slots;
	size_t BoundTextures = 0;
	std::unique_ptr<VulkanImage> WhiteImage;
	std::unique_ptr<VulkanImageView> WhiteView;
	std::unique_ptr<VulkanSampler> SceneSampler;
	// What each slot's surfaces are made of, indexed the same way.
	std::unique_ptr<VulkanBuffer> MaterialBuffer;

	// Pixels waiting to be copied into their slots at the start of the next
	// frame, and the staging of the frame in flight, kept until it completes.
	struct PendingPixels
	{
		uint32_t Index;
		std::unique_ptr<VulkanBuffer> Staging;
	};
	std::vector<PendingPixels> Pending;
	std::vector<std::unique_ptr<VulkanBuffer>> InFlightStaging;

	std::unique_ptr<VulkanQueryPool> Timestamps;
	double TimestampPeriodMs = 0.0;
	bool TimestampsPending = false;
	static const uint32_t TimestampCount = 5;

	TracePushConstants PushConstants = {};
};
