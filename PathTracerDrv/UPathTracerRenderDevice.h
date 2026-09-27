#pragma once

#include "vec.h"
#include "mat.h"
#include "LevelScene.h"
#include "TextureCache.h"
#include "TraceClient.h"
#include <chrono>
#include <functional>
#include <memory>

// One corner of a 2D tile, in normalised device coordinates.
struct TileVertex
{
	vec2 Position;
	vec2 TexCoord;
	vec4 Color;
};

// A run of tile vertices that share a texture and a blend mode.
struct TileBatch
{
	CachedTexture* Texture = nullptr;
	int BlendMode = 0;      // 0 alpha, 1 additive, 2 modulated
	int SamplerMode = 0;    // TileSamplers' index
	int FirstVertex = 0;
	int VertexCount = 0;
};

// The view, as the trace shader takes it: the eye, and the right, up and
// forward vectors scaled to the edges of the view, with "up" pointing down the
// screen. w carries the screen flash, filled in as the frame is sent.
struct TraceCamera
{
	vec4 Origin;
	vec4 Right;
	vec4 Up;
	vec4 Forward;
};

// A path traced render device for Deus Ex.
//
// It ignores almost everything the engine pushes at it. DrawComplexSurface and
// friends only ever describe what survived the engine's own culling, which is
// the wrong half of the level for a tracer - the light in a room comes off the
// walls behind the camera. The scene is read out of UModel instead, once per
// level, and everything after that is rays.
//
// The rays are not traced here. This is a 32-bit process, and only 64-bit
// ones are offered ray tracing outside upstream wine, so the scene goes to
// PathTracerHelper.exe on the same GPU and the frame comes back through an
// image the two share (TraceProtocol.h). What stays here is everything that
// reads the engine - the level, the actors, the textures - and the engine's
// 2D, drawn over the traced picture, with the window and the swap chain.
class UPathTracerRenderDevice : public URenderDevice
{
public:
	DECLARE_CLASS(UPathTracerRenderDevice, URenderDevice, CLASS_Config)

	UPathTracerRenderDevice();
	void StaticConstructor();

	UBOOL Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen) override;
	UBOOL SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen) override;
	void Exit() override;
	void Flush(UBOOL AllowPrecache) override;
	UBOOL Exec(const TCHAR* Cmd, FOutputDevice& Ar) override;
	void Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize) override;
	void Unlock(UBOOL Blit) override;
	void SetSceneNode(FSceneNode* Frame) override;

	// The engine's own drawing. Deliberately ignored: the picture comes from
	// the acceleration structure, not from these.
	void DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet) override;
	void DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span) override;
	void DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags) override;
	void Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2) override;
	void Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z) override;
	void ClearZ(FSceneNode* Frame) override;
	void PushHit(const BYTE* Data, INT Count) override;
	void PopHit(INT Count, UBOOL bForce) override;
	void GetStats(TCHAR* Result) override;
	void ReadPixels(FColor* Pixels) override;

	// The set binding a cached texture with one of the tile samplers, made the
	// first time a tile asks for it. mode is TileSamplers' index.
	VulkanDescriptorSet* TileSet(CachedTexture* texture, int mode);

	VulkanDevice* GetDevice() const { return Device.get(); }

	// Runs a command buffer to completion. Used for uploads and structure
	// builds, which happen once per level rather than once per frame.
	void ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn);

	// Whether the window is currently the borderless fullscreen one.
	bool IsFullscreenWindow() const { return FullscreenState.Enabled; }

	// Configuration.
	INT Bounces;
	BYTE Exposure;
	BYTE SkyIntensity;
	INT MaxAccumulatedFrames;
	INT VkDeviceIndex;
	BITFIELD VkDebug;
	// 0 off, 1 paints every instanced shape magenta, 2 shows albedo with no
	// lighting at all - which separates "not there" from "there but unlit".
	INT DebugMode;
	// Animation poses per mesh. 1 builds each character once, like a prop.
	// Percentage applied to every light's brightness. The engine's own
	// brightnesses are faithful but conservative once traced rather than baked.
	INT LightScale;
	BITFIELD UseVSync;
	// Log where each frame's time goes, averaged every few hundred frames.
	BITFIELD LogTimings;
	// Denoise with NRD from the start ("Denoise" in the ini). PT DENOISE
	// switches it for the session.
	BITFIELD UseDenoiser;
	// Frames per second to pace presentation to, or 0 for no limit. The engine
	// only enforces a tick rate for network play, and Deus Ex misbehaves when
	// left to run at several hundred frames a second - conversation audio is
	// cut short, and the intro's dialogue with it.
	INT FPSLimit;
	// How far the reflection off a smooth surface is traced: 1 lights what it
	// shows by the lights and ambient only, more carries on bouncing, 0 traces
	// none and leaves highlights alone.
	INT GlossBounces;
	// Surfaces made of something: see Materials.h. Off, everything is matte
	// and the trace and the denoiser cost what they did before materials.
	// Off by default until the materials have been checked by hand: guessed
	// from texture groups, some come out wrong (Liberty Island's brick path
	// as shiny as glass). PT MATERIALS switches it for the session.
	BITFIELD UseMaterials;
	// Denoise with NVIDIA's DLSS Ray Reconstruction rather than NRD ("DLSS"
	// in the ini, on by default at Quality), wherever it can run - an RTX
	// GPU, and under wine the pieces Proton provides - and with NRD wherever
	// it cannot. It upscales as it denoises: DLSSQuality is 0 DLAA (no
	// upscaling), 1 quality, 2 balanced, 3 performance, 4 ultra performance.
	// PT DLSS switches it for the session and sets the quality.
	BITFIELD UseDLSS;
	INT DLSSQuality;
	// Use the S3TC textures a package carries beside its originals - New
	// Vision's, eight times the size - as OpenGLDrv's UseS3TC does. On by
	// default: a package without them is unaffected. Only the trace reads
	// them; the engine's own pass under this device (SupportsTC stays off)
	// keeps to the originals, so a 32 bit process is not left holding every
	// S3TC set in a level once it has gone to the helper.
	BITFIELD UseS3TC;
	// On a screen wider than 4:3, keep the height of view the game's field of
	// view gives at 4:3 and widen it to the screen ("Hor+"), rather than keep
	// the width and crop the top and bottom as the engine does. See
	// SetSceneNode. PT WIDESCREEN switches it for the session.
	BITFIELD UseWidescreenFOV;
	// In fullscreen, lay the game's own 2D - the HUD, menus, conversations and
	// the pointer - out in a box no wider than this aspect ratio, centred, with
	// the traced world filling the screen either side ("PinnedUI" in the ini:
	// 1.333333 for 4:3, the default, 1.777778 for 16:9, 0 to use the whole
	// width). The picture is the mode chosen: at the screen's own width the
	// world fills the screen around the box, and a narrower mode is
	// letterboxed with the box inside it. See SetRes. PT PINNEDUI switches it
	// for the session.
	FLOAT PinnedUI;
	// How big a light is, for its shadows: the radius, in world units, of the
	// disc around each light that shadows are cast from. 0 casts them from a
	// point, hard to their far ends; larger ones start sharp where something
	// meets its shadow and soften with distance from it, as real ones do.
	// PT LIGHTSIZE n changes it for the session.
	INT LightSize;
	FLOAT MaxAnisotropy;

private:
	FString DescribeDenoiser() const;
	void CreateSwapChainResources();
	void ReleaseSwapChainResources();
	void CreateTilePipeline();
	void RenderTiles(VulkanCommandBuffer* commands);
	void CreateBrightnessPipeline();
	void DescribeLightingOf(AActor* target);
	void DescribeLightingAt(ULevel* level, const FVector& point, const FVector& normal, UTexture* texture, bool specialLit, FOutputDevice& Ar);
	void ApplyBrightness(VulkanCommandBuffer* commands);
	void EnsureSceneBuilt(ULevel* level);
	// Whether the engine has collected garbage since last asked, told by a
	// transient object nothing refers to, which every collection destroys.
	bool GarbageCollected();
	UObject* GcSentinel = nullptr;
	INT GcSentinelIndex = INDEX_NONE;
	FName GcSentinelName;

	std::shared_ptr<VulkanInstance> Instance;
	std::shared_ptr<VulkanSurface> Surface;
	std::shared_ptr<VulkanDevice> Device;

	std::shared_ptr<VulkanSwapChain> SwapChain;
	std::unique_ptr<VulkanCommandPool> CommandPool;
	std::unique_ptr<VulkanFence> RenderFinishedFence;
	std::unique_ptr<VulkanSemaphore> ImageAvailableSemaphore;
	std::unique_ptr<VulkanSemaphore> RenderFinishedSemaphore;

	// The picture, at the trace's size: the helper's frame copied in, the 2D
	// drawn over it, then blitted to the window.
	std::unique_ptr<VulkanImage> OutputImage;
	std::unique_ptr<VulkanImageView> OutputView;
	int TraceWidth = 0;
	int TraceHeight = 0;
	// Where the engine's view sits across the trace: wider than it when the
	// UI is pinned and the world fills the screen around it.
	int UiOffsetX = 0;
	// The mode the player chose, when the pin gave the engine a narrower one:
	// what the trace fills out to. 0 when the engine has the mode chosen.
	int PinnedModeWidth = 0;
	int PinnedModeHeight = 0;

	// The helper that traces, and what it has been sent so far: which
	// geometries, at which versions, and which textures - with what is needed
	// to send an animated one's next frame. See SendScene.
	std::unique_ptr<TraceClient> Tracer;
	bool StartTracer();
	bool SendScene();
	bool SceneReset = true;
	std::vector<uint32_t> SentVersions;
	struct SentTexture
	{
		UTexture* Source = nullptr;
		bool Masked = false;
		bool Animated = false;
		int Width = 0, Height = 0;
		UTexture* LastFrame = nullptr;
	};
	std::vector<SentTexture> SentTextures;
	std::vector<uint32_t> Pixels;
	int TextureFailuresLogged = 0;
	bool TracerLost = false;
	// Said once in the log, so it can be seen that the engine's own sprites
	// in the level are arriving and being left to the trace: see DrawTile.
	bool LoggedWorldSprite = false;

	// PT DENOISE, PT DLSS, PT MATERIALS, PT WIDESCREEN and PT PINNEDUI, for
	// the session; the helper follows.
	bool DenoiseEnabled = false;
	bool DlssEnabled = true;
	int DlssQualityNow = 1;
	bool MaterialsEnabled = true;
	bool WidescreenFovEnabled = true;
	float PinnedAspect = 4.0f / 3.0f;
	int LightSizeNow = 4;
	bool DenoiseRestart = true;
	// Which part of the picture PT VIEW shows in its place, as the trace
	// shader numbers them; 0 for the picture itself.
	int ViewMode = 0;

	// The 2D pass.
	std::unique_ptr<TextureCache> Textures;
	std::unique_ptr<VulkanDescriptorSetLayout> TileSetLayout;
	std::unique_ptr<VulkanDescriptorPool> TileDescriptorPool;
	// As the other devices sample the engine's 2D: bit 1 nearest rather than
	// linear, for art drawn with PF_NoSmooth - which is most of Deus Ex's
	// interface, and filtering it anyway is what made the HUD soft - and bit 2
	// clamped rather than repeating, for a tile that shows its whole texture,
	// so linear filtering does not pull in the opposite edge.
	std::unique_ptr<VulkanSampler> TileSamplers[4];
	std::unique_ptr<VulkanPipelineLayout> TilePipelineLayout;
	std::unique_ptr<VulkanRenderPass> TileRenderPass;
	std::unique_ptr<VulkanPipeline> TilePipelines[3];
	std::unique_ptr<VulkanShader> TileVertexShader;
	std::unique_ptr<VulkanShader> TileFragmentShader;
	std::unique_ptr<VulkanFramebuffer> TileFramebuffer;
	std::unique_ptr<VulkanBuffer> TileVertexBuffer;
	size_t TileVertexCapacity = 0;
	std::vector<TileVertex> TileVertices;
	std::vector<TileBatch> TileBatches;

	// The game's Brightness setting, over the finished picture: see
	// ApplyBrightness.
	std::unique_ptr<VulkanShader> BrightnessShader;
	std::unique_ptr<VulkanDescriptorSetLayout> BrightnessSetLayout;
	std::unique_ptr<VulkanDescriptorPool> BrightnessDescriptorPool;
	std::unique_ptr<VulkanDescriptorSet> BrightnessSet;
	std::unique_ptr<VulkanPipelineLayout> BrightnessPipelineLayout;
	std::unique_ptr<VulkanPipeline> BrightnessPipeline;

	// The last frame's submission, not yet known to be finished. Unlock does
	// not wait for the GPU: the next frame's game logic and scene gathering
	// run while it presents, and WaitForPreviousFrame blocks only when
	// something is about to touch memory the GPU may still be reading.
	std::unique_ptr<VulkanCommandBuffer> PendingCommands;
	bool FramePending = false;
	void WaitForPreviousFrame();
	void WriteTimingLine(const char* line);
	// Sleeps until the next frame is due under FPSLimit.
	void LimitFrameRate();
	std::chrono::steady_clock::time_point NextFrameTime;

	LevelScene Scene;

	// Accumulation state. The image only converges while nothing moves, so the
	// device has to notice when something has.
	TraceCamera Camera = {};
	TraceCamera LastCamera = {};
	uint32_t AccumulatedFrames = 0;
	size_t LastInstanceCount = 0;

	// The engine's screen flash for this frame, from Lock.
	uint32_t DisableBits = 0;
	FPlane FlashScale = FPlane(0.5f, 0.5f, 0.5f, 0.0f);
	FPlane FlashFog = FPlane(0.0f, 0.0f, 0.0f, 0.0f);

	// Where each frame's time goes, averaged and logged every few hundred
	// frames when LogTimings is set: here, and on the GPU in the helper.
	struct FrameTimings
	{
		double Collect = 0, Send = 0, Textures = 0, Wait = 0, Total = 0, Limit = 0;
		int Frames = 0;
		// The helper's side of Send: waiting for the GPU, taking in the scene,
		// recording the frame.
		double HelperWait = 0, HelperApply = 0, HelperRecord = 0;
		int HelperStalls = 0;
		double GpuBuild = 0, GpuTrace = 0, GpuDenoise = 0, GpuComposite = 0;
		int GpuFrames = 0;
		int Logged = 0;
	} Timings;
	// Every frame's length, Unlock to Unlock, for the frame rate and the
	// slowest frames in the timings log.
	std::vector<float> FrameIntervals;
	double LastUnlockMs = 0.0;
	uint32_t FrameIndex = 0;

	bool HaveCamera = false;
	UBOOL UsingVsync = 0;

	// The windowed style and rectangle to return to. Fullscreen here is a
	// borderless window rather than a display mode change.
	struct
	{
		RECT WindowPos = {};
		LONG Style = 0;
		LONG ExStyle = 0;
		bool Enabled = false;
	} FullscreenState;

	bool InSetResCall = false;
	// The engine's window, while its messages pass through the events log.
	HWND SubclassedWindow = nullptr;
};
