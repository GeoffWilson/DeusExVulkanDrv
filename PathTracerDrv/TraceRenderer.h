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
class FsrUpscaler;
class WriteDescriptors;
namespace TraceProtocol { struct TraceCommand; }

// The trace shader's push constants: see Shaders::Trace.
struct TracePushConstants
{
	vec4 CameraOrigin;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	uint32_t Counts[4];   // frame, light count, bounces, accumulated frames
	vec4 Params;          // exposure, sky intensity, 1 for the engine's lighting, debug mode
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
// A frame is traced for one view, the screen's, or for a headset's two eyes,
// each a view of its own with its own images, history and denoiser; the scene,
// the structures and the textures are the same for all of them. With the
// eyes, the picture handed back to the device is the left eye's, cut to the
// screen's shape.
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
	void SetTexture(uint32_t index, uint32_t width, uint32_t height, uint32_t levels, uint32_t format, const uint32_t* pixels, const vec4& material);
	// New pixels for an existing slot, copied in as the next frame starts.
	void SetTexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels);
	void SetAnisotropy(uint32_t samples);

	// One of a headset's eyes, as the trace shader takes a camera: the eye,
	// and its right, down and forward, the first two scaled to the view's
	// half extents - and where the middle of its view is off that forward, in
	// half widths and heights, since an eye sees further to one side than the
	// other (the trace shader's viewShift). The same for last frame, and the
	// size of its picture.
	struct EyeView
	{
		vec4 Camera[4];
		vec4 PreviousCamera[4];
		vec2 Shift = vec2(0.0f, 0.0f);
		vec2 PreviousShift = vec2(0.0f, 0.0f);
		uint32_t Width = 0, Height = 0;
	};
	static const int MaxViews = 2;

	// Records a frame into one of the FramesInFlight slots, whose last frame
	// the caller has waited for: the uploads, the structures, the trace, and
	// when denoising NRD and the composite. The frame before may still be on
	// the GPU, and the recording starts by waiting for it there. The picture
	// ends up in Output, in GENERAL layout. False when there is nothing to
	// trace yet.
	//
	// With eyes, the two of them are traced instead of the view the frame
	// describes, each into EyeOutput; Output is then the left eye's picture
	// at the frame's size - or with sideBySide both, each in its half, which
	// the test harness looks at.
	bool Record(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame, int slot, const EyeView* eyes = nullptr, bool sideBySide = false);

	// The GPU has finished the frame in slot: its timestamps can be read.
	void FrameCompleted(int slot);

	// The picture, at the size the device asked for.
	VulkanImage* Output() const { return TracedEyes ? MirrorImage.get() : Views[0].OutputImage.get(); }
	int Width() const { return TracedEyes ? MirrorWidth : Views[0].OutputWidth; }
	int Height() const { return TracedEyes ? MirrorHeight : Views[0].OutputHeight; }
	// An eye's picture, from the last frame traced for them, GENERAL.
	VulkanImage* EyeOutput(int eye) const { return Views[eye].OutputImage.get(); }
	VulkanImageView* EyeOutputView(int eye) const { return Views[eye].OutputView.get(); }
	bool TracedForEyes() const { return TracedEyes; }
	// What the trace itself ran at.
	int RenderWidth() const { return Views[0].TraceWidth; }
	int RenderHeight() const { return Views[0].TraceHeight; }
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
	// The host's time recording frames, by stage, summed until the helper
	// logs it: setup, shapes and placements, lights and their grid, motion,
	// uploads, structure builds, descriptors, trace, denoiser, insets.
	static const int RecordStages = 10;
	double RecordStageMs[RecordStages] = {};

private:
	// A denoiser's inputs, written by the trace at bindings 9 to 15 when asked
	// for, the fog at 17, what mirrors show at 18 and 19, and the glossy
	// reflection off a surface and its colour at 21 and 22: see the trace
	// shader.
	static const int GuideImageCount = 12;
	static int GuideBinding(int i) { return i < 7 ? 9 + i : (i < 10 ? 10 + i : 11 + i); }
	static bool GuideIsDepth(int i) { return i == 1 || i == 9; }

	// What one view traces into and denoises from, and the descriptor sets
	// that point at it: set 0 of the trace, whose scene bindings every view
	// shares, and set 1, its own images.
	struct View
	{
		std::unique_ptr<VulkanDescriptorSet> SceneSet;
		std::unique_ptr<VulkanDescriptorSet> ImageSet;

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
		// Or for FSR: the trace and NRD at the render size, the composite
		// finishing the picture into FsrColorImage there, with the depth and
		// motion FSR reads in the Rr images, and FSR bringing it up to Output.
		bool TracingForFsr = false;
		std::unique_ptr<VulkanImage> FsrColorImage;
		std::unique_ptr<VulkanImageView> FsrColorView;
		// The offset the last frame's primary rays took within their pixels,
		// which NRD is told along with this frame's.
		vec2 LastJitter = vec2(0.0f, 0.0f);

		std::unique_ptr<VulkanImage> GuideImages[GuideImageCount];
		std::unique_ptr<VulkanImageView> GuideViews[GuideImageCount];
		// The depth and motion images again, their motion moved to where NRD
		// reads it: the surfaces seen, and those seen in mirrors.
		std::unique_ptr<VulkanImageView> MotionView;
		std::unique_ptr<VulkanImageView> ReflectionMotionView;

		std::unique_ptr<Denoiser> Denoise;
		bool DenoiseRestart = true;
		std::unique_ptr<VulkanDescriptorSet> CompositeSet;

		// Ray Reconstruction's inputs that NRD's images cannot carry - the
		// picture at the render size, the depth and the motion each on their
		// own - and its output. The depth and motion are always there, since
		// the trace binds them.
		std::unique_ptr<VulkanImage> RrColorImage;
		std::unique_ptr<VulkanImageView> RrColorView;
		std::unique_ptr<VulkanImage> RrDepthImage;
		std::unique_ptr<VulkanImageView> RrDepthView;
		std::unique_ptr<VulkanImage> RrMotionImage;
		std::unique_ptr<VulkanImageView> RrMotionView;
		std::unique_ptr<VulkanImage> RrOutputImage;
		std::unique_ptr<VulkanImageView> RrOutputView;
		std::unique_ptr<VulkanDescriptorSet> FinishSet;

		// Last frame's camera and each instance's last placement, for motion
		// vectors. Rewritten every frame.
		std::unique_ptr<VulkanBuffer> MotionBuffer;
		size_t MotionCapacity = 0;
	};

	void CreateTracePipeline();
	void CreateCompositePipeline();
	void CreateFinishPipeline();
	void Resize(View& view, int renderWidth, int renderHeight, int outputWidth, int outputHeight, bool forRayReconstruction, bool forFsr);
	void EnsureDenoiser(View& view, bool wanted, bool materials);
	void UpdateDescriptors();
	void WriteCompositeDescriptors(View& view);
	void WriteFinishDescriptors(View& view);
	void WriteMotion(View& view, const TraceProtocol::TraceCommand& frame, const vec4* previousCamera, vec4 shift, vec2 jitter, FrameUploads& uploads);
	void EnsureFogShadows(uint32_t count, FrameUploads& uploads);
	void RecordTexturePixels(VulkanCommandBuffer* commands, FrameUploads& uploads);
	void BindWhite(uint32_t index);
	void WriteTextureSlot(WriteDescriptors& writes, int index, VulkanImageView* image);
	void RecordMirror(VulkanCommandBuffer* commands, int width, int height, bool sideBySide);
	void RecordInsets(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame, VulkanImage* target, int targetWidth, int targetHeight);

	GpuContext* Context = nullptr;
	VulkanDevice* Device = nullptr;
	bool CanSampleTextures = false;

	std::unique_ptr<AccelStructure> Accel;

	std::unique_ptr<VulkanDescriptorSetLayout> DescriptorLayout;
	std::unique_ptr<VulkanDescriptorPool> DescriptorPool;
	// The views' own images, set 1 of the trace: one set for each view, and
	// one for each window of the HUD's that shows a view of its own.
	std::unique_ptr<VulkanDescriptorSetLayout> ViewLayout;
	struct Inset
	{
		int Width = 0;
		int Height = 0;
		std::unique_ptr<VulkanImage> Accum, Out, History;
		std::unique_ptr<VulkanImageView> AccumView, OutView, HistoryView;
		std::unique_ptr<VulkanDescriptorSet> Set;
	};
	static const int MaxInsets = 4;   // TraceProtocol::MaxInsets
	Inset Insets[MaxInsets];
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanShader> TraceShader;
	std::unique_ptr<VulkanPipeline> TracePipeline;
	std::unique_ptr<VulkanShader> FogShadowShader;
	std::unique_ptr<VulkanPipeline> FogShadowPipeline;
	bool DescriptorsDirty = true;

	// The screen's view, or a headset's left and right eyes, and how many of
	// them the last frame traced.
	View Views[MaxViews];
	int ActiveViews = 1;
	bool TracedEyes = false;
	// With the eyes, the picture the device is handed: the left eye's, at the
	// size it asked for, with the HUD's windows' views in it.
	std::unique_ptr<VulkanImage> MirrorImage;
	int MirrorWidth = 0;
	int MirrorHeight = 0;
	bool MirrorFresh = true;

	bool DenoiseFailed = false;
	std::unique_ptr<VulkanDescriptorSetLayout> CompositeLayout;
	std::unique_ptr<VulkanDescriptorPool> CompositePool;
	std::unique_ptr<VulkanPipelineLayout> CompositePipelineLayout;
	std::unique_ptr<VulkanShader> CompositeShader;
	std::unique_ptr<VulkanPipeline> CompositePipeline;

	// DLSS Ray Reconstruction, started the first time it is asked for, and
	// only tried the once: where it cannot run, NRD stands in.
	std::unique_ptr<RayReconstruction> Rr;
	bool RrTried = false;
	// AMD's FSR 3.1, started the first time it is asked for, for the GPUs
	// Ray Reconstruction cannot run on.
	std::unique_ptr<FsrUpscaler> Fsr;
	bool FsrTried = false;
	std::unique_ptr<VulkanSampler> FogSampler;
	std::unique_ptr<VulkanDescriptorSetLayout> FinishLayout;
	std::unique_ptr<VulkanDescriptorPool> FinishPool;
	std::unique_ptr<VulkanPipelineLayout> FinishPipelineLayout;
	std::unique_ptr<VulkanShader> FinishShader;
	std::unique_ptr<VulkanPipeline> FinishPipeline;
	uint32_t LastDenoiser = 0;
	// When the last frame was recorded: Ray Reconstruction scales how hard it
	// denoises by how fast things move, and wants to know how long a frame is.
	std::chrono::steady_clock::time_point LastRecordTime;

	// What each frame in flight stages for the GPU, and what it retires.
	std::unique_ptr<FrameUploads> Uploads[GpuContext::FramesInFlight];

	// Each fog light's shadow cube, in floats: see Shaders::FogShadows. The
	// size and the most lights given one are the shader's own.
	static const uint32_t FogShadowSize = 64;
	static const size_t MaxFogShadows = 128;
	std::unique_ptr<VulkanBuffer> FogShadowBuffer;
	size_t FogShadowCapacity = 0;

	// The texture array: an image per slot the device has sent, the rest a 1x1
	// white image so every descriptor is valid whether or not it is read.
	static const int MaxTextures = 1024;
	struct Slot
	{
		std::unique_ptr<VulkanImage> Image;
		std::unique_ptr<VulkanImageView> View;
		uint32_t Width = 0, Height = 0, Levels = 1, Format = 0;
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
