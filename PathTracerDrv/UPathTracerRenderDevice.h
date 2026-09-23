#pragma once

#include "vec.h"
#include "mat.h"
#include "LevelScene.h"
#include "Denoiser.h"
#include "AccelStructure.h"
#include "TextureCache.h"
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

// A path traced render device for Deus Ex.
//
// It ignores almost everything the engine pushes at it. DrawComplexSurface and
// friends only ever describe what survived the engine's own culling, which is
// the wrong half of the level for a tracer - the light in a room comes off the
// walls behind the camera. The scene is read out of UModel instead, once per
// level, and everything after that is rays.
//
// What this costs: the engine's 2D drawing is not implemented yet, so there is
// no HUD and no menus. See the README.
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
	void CreateTracePipeline();
	void CreateTilePipeline();
	void RenderTiles(VulkanCommandBuffer* commands);
	void EnsureSceneBuilt(ULevel* level);
	void UpdateDescriptors();

	std::shared_ptr<VulkanInstance> Instance;
	std::shared_ptr<VulkanSurface> Surface;
	std::shared_ptr<VulkanDevice> Device;

	std::shared_ptr<VulkanSwapChain> SwapChain;
	std::unique_ptr<VulkanCommandPool> CommandPool;
	std::unique_ptr<VulkanCommandBuffer> DrawCommands;
	std::unique_ptr<VulkanFence> RenderFinishedFence;
	std::unique_ptr<VulkanSemaphore> ImageAvailableSemaphore;
	std::unique_ptr<VulkanSemaphore> RenderFinishedSemaphore;

	std::unique_ptr<VulkanDescriptorSetLayout> DescriptorLayout;
	std::unique_ptr<VulkanDescriptorPool> DescriptorPool;
	VulkanDescriptorSet* DescriptorSet = nullptr;
	std::unique_ptr<VulkanDescriptorSet> DescriptorSetOwner;
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanPipeline> TracePipeline;
	std::unique_ptr<VulkanShader> TraceShader;

	std::unique_ptr<VulkanImage> AccumImage;
	std::unique_ptr<VulkanImageView> AccumView;
	std::unique_ptr<VulkanImage> HistoryImage;
	std::unique_ptr<VulkanImageView> HistoryView;
	std::unique_ptr<VulkanImage> OutputImage;
	std::unique_ptr<VulkanImageView> OutputView;

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

	// NRD, and the pass that puts the picture back together from what it
	// returns. PT DENOISE switches it on.
	std::unique_ptr<Denoiser> Denoise;
	bool DenoiseEnabled = false;
	bool MaterialsEnabled = true;
	bool DenoiseRestart = true;
	void EnsureDenoiser();
	std::unique_ptr<VulkanDescriptorSetLayout> CompositeLayout;
	std::unique_ptr<VulkanDescriptorPool> CompositePool;
	std::unique_ptr<VulkanDescriptorSet> CompositeSet;
	std::unique_ptr<VulkanPipelineLayout> CompositePipelineLayout;
	std::unique_ptr<VulkanShader> CompositeShader;
	std::unique_ptr<VulkanPipeline> CompositePipeline;
	void CreateCompositePipeline();
	void WriteCompositeDescriptors();
	// Last frame's camera and each instance's last placement, for motion
	// vectors. Rewritten every frame.
	std::unique_ptr<VulkanBuffer> MotionBuffer;
	size_t MotionCapacity = 0;
	void WriteMotion(const TracePushConstants& previousCamera);
	// Which part of the picture PT VIEW shows in its place, as the trace
	// shader numbers them; 0 for the picture itself.
	int ViewMode = 0;
	int TraceWidth = 0;
	int TraceHeight = 0;

	// The 2D pass.
	std::unique_ptr<TextureCache> Textures;
	std::unique_ptr<VulkanDescriptorSetLayout> TileSetLayout;
	std::unique_ptr<VulkanDescriptorPool> TileDescriptorPool;
	std::unique_ptr<VulkanSampler> TileSampler;

	// Textures the trace samples, in the order LevelScene registered them. The
	// array binding is written as it grows; slots past what the scene uses hold
	// a 1x1 white image so every descriptor is valid whether or not it is read.
	std::unique_ptr<VulkanSampler> SceneSampler;
	// What each of those textures is made of, one vec4 per slot, indexed the
	// same way. Written as the texture array is; slots never written are matte.
	std::unique_ptr<VulkanBuffer> MaterialBuffer;
	size_t WrittenMaterials = 0;
	void WriteMaterials();
	// Staging for this frame's realtime texture uploads, released once the
	// submission that reads them has completed.
	std::vector<std::unique_ptr<VulkanBuffer>> RealtimeStaging;

	// The last frame's submission, not yet known to be finished. Unlock does
	// not wait for the GPU: the next frame's game logic and scene gathering
	// run while it traces, and WaitForPreviousFrame blocks only when
	// something is about to touch memory the GPU may still be reading.
	std::unique_ptr<VulkanCommandBuffer> PendingCommands;
	bool FramePending = false;
	void WaitForPreviousFrame();
	// Sleeps until the next frame is due under FPSLimit.
	void LimitFrameRate();
	std::chrono::steady_clock::time_point NextFrameTime;
	size_t BoundSceneTextures = 0;
	bool SceneTexturesInitialised = false;
	int TextureFailuresLogged = 0;
	bool CanSampleTextures = false;
	void UpdateSceneTextures();
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

	LevelScene Scene;
	std::unique_ptr<AccelStructure> Accel;

	// Accumulation state. The image only converges while nothing moves, so the
	// device has to notice when something has.
	TracePushConstants LastCamera = {};
	uint32_t AccumulatedFrames = 0;
	size_t LastInstanceCount = 0;

	// The engine's screen flash for this frame, from Lock.
	uint32_t DisableBits = 0;
	FPlane FlashScale = FPlane(0.5f, 0.5f, 0.5f, 0.0f);
	FPlane FlashFog = FPlane(0.0f, 0.0f, 0.0f, 0.0f);

	// Where each frame's time goes, averaged and logged every few hundred
	// frames when LogTimings is set.
	struct FrameTimings
	{
		double Collect = 0, Sync = 0, Refresh = 0, TopLevel = 0, Wait = 0, Total = 0;
		int Frames = 0;
		int Logged = 0;
	} Timings;
	uint32_t FrameIndex = 0;
	bool DescriptorsDirty = true;

	TracePushConstants PushConstants = {};
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
};
