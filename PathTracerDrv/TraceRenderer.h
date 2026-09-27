#pragma once

#include "SceneData.h"
#include "GpuContext.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

class AccelStructure;
class Denoiser;
class FrameUploads;
class RayReconstruction;
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
// structures, the texture array and the materials, the trace, and a denoiser -
// NRD and the pass that puts the picture back together from what it returns,
// or DLSS Ray Reconstruction and the pass that finishes what it returns.
//
// With Ray Reconstruction the trace runs at a render size smaller than the
// picture, which it upscales as it denoises; everything else is at the
// picture's own size.
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
	void SetTexture(uint32_t index, uint32_t width, uint32_t height, uint32_t levels, const uint32_t* pixels, const vec4& material);
	// New pixels for an existing slot, copied in as the next frame starts.
	void SetTexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels);
	void SetAnisotropy(uint32_t samples);

	// Records a frame into one of the FramesInFlight slots, whose last frame
	// the caller has waited for: the uploads, the structures, the trace, and
	// when denoising NRD and the composite. The frame before may still be on
	// the GPU, and the recording starts by waiting for it there. The picture
	// ends up in Output, in GENERAL layout. False when there is nothing to
	// trace yet.
	bool Record(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame, int slot);

	// The GPU has finished the frame in slot: its timestamps can be read.
	void FrameCompleted(int slot);

	// The picture, at the size the device asked for.
	VulkanImage* Output() const { return OutputImage.get(); }
	int Width() const { return OutputWidth; }
	int Height() const { return OutputHeight; }
	// What the trace itself ran at.
	int RenderWidth() const { return TraceWidth; }
	int RenderHeight() const { return TraceHeight; }
	bool SamplesTextures() const { return CanSampleTextures; }

	// About the last frame.
	int LightCount() const;
	int BottomCount() const;
	int TextureCount() const { return (int)BoundTextures; }
	bool DenoiserActive() const;
	const char* DenoiserStatus() const;
	// A TraceProtocol::DenoiserChoice: what the last frame was denoised with.
	uint32_t DenoisedWith() const { return LastDenoiser; }
	const char* DlssStatus() const;
	bool GpuTimed = false;
	float GpuMs[4] = {};    // build, trace, denoise, composite

private:
	void CreateTracePipeline();
	void CreateCompositePipeline();
	void CreateFinishPipeline();
	void Resize(int renderWidth, int renderHeight, int outputWidth, int outputHeight, bool forRayReconstruction);
	void EnsureDenoiser(bool wanted, bool materials);
	void UpdateDescriptors();
	void WriteCompositeDescriptors();
	void WriteFinishDescriptors();
	void WriteMotion(const vec4 (&previousCamera)[4], vec2 jitter, FrameUploads& uploads);
	void RecordTexturePixels(VulkanCommandBuffer* commands, FrameUploads& uploads);
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
	int OutputWidth = 0;
	int OutputHeight = 0;
	// The images are laid out for Ray Reconstruction: the trace writes its
	// picture into RrColorImage at the render size rather than into Output.
	bool TracingForRr = false;

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

	// DLSS Ray Reconstruction, started the first time it is asked for, and
	// only tried the once: where it cannot run, NRD stands in.
	std::unique_ptr<RayReconstruction> Rr;
	bool RrTried = false;
	// Its inputs that NRD's images cannot carry - the picture at the render
	// size, the depth and the motion each on their own - and its output.
	// The depth and motion are always there, since the trace binds them.
	std::unique_ptr<VulkanImage> RrColorImage;
	std::unique_ptr<VulkanImageView> RrColorView;
	std::unique_ptr<VulkanImage> RrDepthImage;
	std::unique_ptr<VulkanImageView> RrDepthView;
	std::unique_ptr<VulkanImage> RrMotionImage;
	std::unique_ptr<VulkanImageView> RrMotionView;
	std::unique_ptr<VulkanImage> RrOutputImage;
	std::unique_ptr<VulkanImageView> RrOutputView;
	std::unique_ptr<VulkanSampler> FogSampler;
	std::unique_ptr<VulkanDescriptorSetLayout> FinishLayout;
	std::unique_ptr<VulkanDescriptorPool> FinishPool;
	std::unique_ptr<VulkanDescriptorSet> FinishSet;
	std::unique_ptr<VulkanPipelineLayout> FinishPipelineLayout;
	std::unique_ptr<VulkanShader> FinishShader;
	std::unique_ptr<VulkanPipeline> FinishPipeline;
	uint32_t LastDenoiser = 0;
	// When the last frame was recorded: Ray Reconstruction scales how hard it
	// denoises by how fast things move, and wants to know how long a frame is.
	std::chrono::steady_clock::time_point LastRecordTime;

	// What each frame in flight stages for the GPU, and what it retires.
	std::unique_ptr<FrameUploads> Uploads[GpuContext::FramesInFlight];

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
		uint32_t Width = 0, Height = 0, Levels = 1;
		vec4 Material;
	};
	std::vector<Slot> Slots;
	size_t BoundTextures = 0;
	std::unique_ptr<VulkanImage> WhiteImage;
	std::unique_ptr<VulkanImageView> WhiteView;
	std::unique_ptr<VulkanSampler> SceneSampler;
	uint32_t SceneAnisotropy = 0;
	// What each slot's surfaces are made of, indexed the same way.
	std::unique_ptr<VulkanBuffer> MaterialBuffer;

	// Pixels waiting to be copied into their slots at the start of the next
	// frame. Their staging is retired with that frame.
	struct PendingPixels
	{
		uint32_t Index;
		std::unique_ptr<VulkanBuffer> Staging;
	};
	std::vector<PendingPixels> Pending;

	// TimestampCount queries for each frame slot.
	std::unique_ptr<VulkanQueryPool> Timestamps;
	double TimestampPeriodMs = 0.0;
	bool TimestampsPending[GpuContext::FramesInFlight] = {};
	static const uint32_t TimestampCount = 5;

	TracePushConstants PushConstants = {};
};
