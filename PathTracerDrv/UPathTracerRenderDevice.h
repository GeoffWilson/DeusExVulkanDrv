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

	// Made per cached texture, since a tile draw binds nothing else.
	std::unique_ptr<VulkanDescriptorSet> AllocateTileDescriptorSet(VulkanImageView* view);

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
	// PT NOMATERIALS switches it for the session.
	BITFIELD UseMaterials;

private:
	void CreateSwapChainResources();
	void ReleaseSwapChainResources();
	void CreateTilePipeline();
	void RenderTiles(VulkanCommandBuffer* commands);
	void EnsureSceneBuilt(ULevel* level);

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

	// PT DENOISE and PT NOMATERIALS, for the session; the helper follows.
	bool DenoiseEnabled = false;
	bool MaterialsEnabled = true;
	bool DenoiseRestart = true;
	// Which part of the picture PT VIEW shows in its place, as the trace
	// shader numbers them; 0 for the picture itself.
	int ViewMode = 0;

	// The 2D pass.
	std::unique_ptr<TextureCache> Textures;
	std::unique_ptr<VulkanDescriptorSetLayout> TileSetLayout;
	std::unique_ptr<VulkanDescriptorPool> TileDescriptorPool;
	std::unique_ptr<VulkanSampler> TileSampler;
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
		double GpuBuild = 0, GpuTrace = 0, GpuDenoise = 0, GpuComposite = 0;
		int GpuFrames = 0;
		int Logged = 0;
	} Timings;
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
