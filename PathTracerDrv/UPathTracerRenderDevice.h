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
	// For a headset: 0 the HUD, which follows the head, 1 what aims with the
	// mouse - the crosshair, the accuracy reticle - and 2 the coronas the
	// engine draws over the world, each drawn into its own layer of the HUD's
	// image (HudLayers).
	int Layer = 0;
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
//
// On 469 it is one of OldUnreal's own render devices rather than an old one:
// the engine wraps an old device in a URenderDeviceProxy, whose Exit, as 469f
// shuts down, calls through to a device that is already gone.
#if defined(OLDUNREAL469SDK)
class UPathTracerRenderDevice : public URenderDeviceOldUnreal469
{
public:
	DECLARE_CLASS(UPathTracerRenderDevice, URenderDeviceOldUnreal469, CLASS_Config, PathTracerDrv)
#else
class UPathTracerRenderDevice : public URenderDevice
{
public:
	DECLARE_CLASS(UPathTracerRenderDevice, URenderDevice, CLASS_Config)
#endif

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
#if defined(OLDUNREAL469SDK)
	UBOOL SupportsTextureFormat(ETextureFormat Format) override;
#endif

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
	// AMD's FSR 3.1 ("FSR" in the ini): where Ray Reconstruction cannot run
	// - any AMD or Intel GPU - the trace and NRD run at the size DLSSQuality
	// gives and FSR brings the picture up to the screen's. 0 never, 1 where
	// Ray Reconstruction cannot run (the default), 2 always, NRD and FSR in
	// its place. FSRSharpness, 0 to 1, sharpens what it gives (0.2 by
	// default). PT FSR [OFF | AUTO | ON] switches it for the session.
	INT FsrMode;
	FLOAT FsrSharpness;
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
	// How the level's surfaces take their lights ("Lighting" in the ini).
	// Engine, the default, as Render.dll builds its lightmaps: each light by
	// its falloff and the cosine as displayed, a light baked into the level
	// at twice a dynamic one, added up as displayed colours with the zone's
	// ambient and drawn at twice the texture as every device draws a
	// lightmap - but of the lights the trace finds reaching the point, so
	// the shadows are real ones. Linear, as before 1.2: each light linear
	// to nothing at its radius, in linear light, flatter and dimmer. PT
	// LIGHTING switches it for the session.
	BYTE Lighting;
	// The tone curve: the neutral one, the default, leaves all but the
	// brightest fifth as it is, as the engine's own devices draw it, and
	// bends only that towards white; off, Reinhard's, which compresses
	// everything. PT TONEMAP switches it for the session.
	BITFIELD NeutralToneMap;
	// The light augmentation as a real flashlight: a torch at the eyes with
	// a hotspot, a spill, the square law and traced shadows, and its beam in
	// the air, in place of the patch of light the game moves to wherever the
	// view lands (LevelScene::PlaceFlashlight). FlashlightBrightness and
	// FlashlightHaze, how bright it is and how much of its beam the air
	// shows, are percentages; the beam is off by default, since from beside
	// the lamp it lights the air in the whole of the cone as a veil rather
	// than showing as a beam. PT FLASHLIGHT switches it for the session, PT
	// FLASHLIGHT n sets the brightness, PT BEAM n the haze.
	BITFIELD UseFlashlight;
	INT FlashlightBrightness;
	INT FlashlightHaze;
	// Fog lights' glow held to their shadows: shafts through a grate, a
	// pillar's shadow in a lamp's halo, where the engine's glows through
	// everything (volumetricFog in Shaders.cpp). PT FOGSHADOWS switches it
	// for the session.
	BITFIELD UseFogShadows;
	// How much the level's glowing surfaces - signs, light panels, screens -
	// light what is around them, in percent: 100 as bright as they glow,
	// which is what the trace had always given them and hardly shows, since
	// a sign's texture is no brighter than a lit wall; 1000, the default, for
	// neon that spills its colour onto the walls around it. What they look
	// like is unchanged. PT GLOW n sets it for the session.
	INT GlowLighting;
	// Light through glass takes its colour: stained glass throws coloured
	// patches, a tinted pane tints what it lets in (glassTransmittance in
	// Shaders.cpp). The engine's lightmaps pass light through glass as if
	// it were not there. PT GLASS switches it for the session.
	BITFIELD UseColouredGlass;
	// Wet streets, in percent, 0 - the default - for dry: the ground open to
	// the sky darkened and shining as after rain, with puddles standing on
	// the flat, reflecting the neon (wetnessAt in Shaders.cpp). Not in the
	// original, where it never rains, so off unless asked for. PT WET n
	// sets it for the session.
	INT Wetness;
	// Bump mapping, in percent of the relief the materials give each
	// texture (Materials.h): walls and floors lit by the slope of their
	// texture's brightness, the mortar and the grout sunk between bricks and
	// tiles (reliefNormal in Shaders.cpp). A look the original never had, so
	// 0 - flat, as the engine draws them - unless asked for: 100 is the
	// materials' relief as given. PT BUMP n sets it for the session.
	INT BumpMapping;
	// HDR output, where the display and its compositor take it - scRGB on
	// Windows, HDR10 under a Wayland compositor. The picture's SDR range and
	// the HUD are drawn at HDRPaperWhite nits, and what the tone curve had
	// to squeeze in below white spreads out above it up to HDRPeakNits: the
	// lamps, the neon, the light off a wall (toneMap, and EncodeFrame). Off,
	// the default, is SDR as before; PT HDR switches it for the session.
	BITFIELD Hdr;
	INT HdrPeakNits;
	INT HdrPaperWhite;
	// A headset through OpenXR ("VR" in the ini, off by default): each eye
	// traced from where it is, in a room that is the player's view - its
	// middle straight ahead, the mouse turning the player and the room with
	// them - and the HUD on a panel spanning that view HeadsetHudDistance
	// metres ahead, so what it marks lines up with the world behind it. The
	// screen shows the left eye. HeadsetUnitsPerMetre is the world's scale,
	// for how far apart the eyes are and how far a lean moves them; Deus
	// Ex's is 52.5, a player 94 units tall. HeadsetResolution is the eyes'
	// size in percent of what the headset asks for. PT VR switches it for the
	// session, which starts the helper again (see HeadsetOutput).
	BITFIELD UseHeadset;
	FLOAT HeadsetUnitsPerMetre;
	INT HeadsetResolution;
	FLOAT HeadsetHudDistance;
	// How wide the HUD's box is in the headset, in degrees: it follows the
	// head, and the game projects what it marks in the world through where
	// the head looks, while the crosshair stays with the mouse (see
	// SetSceneNode and DrawTile).
	FLOAT HeadsetHudSize;
	// Whether the HUD shows in the headset ("VRShowHud", off by default):
	// off, the parts the game itself can hide - the object belt, the health
	// display, the ammo, the augmentations' icons, the compass - are hidden
	// while the headset shows the game, and put back as it stops, leaving
	// the crosshair, the messages, conversations and menus. PT VR SHOWHUD
	// switches it for the session (UpdateHeadsetHud).
	BITFIELD HeadsetShowHud;
	// What the headset's pictures are traced with in place of DLSSQuality
	// and Bounces ("VRDLSSQuality", performance by default, and
	// "VRBounces", 1): two eyes, each bigger than the screen, at the
	// headset's refresh, want far fewer samples' worth of work a pixel.
	// Kept apart from the screen's own, which come back as VR goes off. PT
	// DLSS and PT BOUNCES set these while VR is on.
	INT HeadsetDlssQuality;
	INT HeadsetBounces;

private:
	// The switches in DisableBits the ini sets rather than PT alone: the
	// fog's shadows (65536), the flashlight (131072) and glass's colour
	// (1048576).
	static const uint32_t ConfiguredMask = 65536u | 131072u | 1048576u;
	uint32_t ConfiguredBits() const { return (UseFogShadows ? 0u : 65536u) | (UseFlashlight ? 0u : 131072u) | (UseColouredGlass ? 0u : 1048576u); }
	// The flashlight as last traced, to notice it moving or going out.
	vec4 LastFlashlight[3] = {};
	FString DescribeDenoiser() const;
	void CreateSwapChainResources();
	void ReleaseSwapChainResources();
	void CreateTilePipeline();
	// The frame's tiles, drawn into target through framebuffer with one of
	// the sets of pipelines: over the picture, or into the HUD's own image
	// for the headset.
	void RenderTiles(VulkanCommandBuffer* commands, VulkanImage* target, VulkanFramebuffer* framebuffer, std::unique_ptr<VulkanPipeline>* pipelines, bool layered = false);
	// A tile of what aims with the mouse rather than looks with the head.
	bool AimsWithMouse(const FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL) const;
	// The tangent of half the HUD's box's width in the headset.
	float HeadsetHudTangent() const;
	// With the headset: the HUD drawn on its own, into the image the helper
	// shares for it (see TraceProtocol.h).
	void EnsureHudImage();
	void DrawHud(VulkanCommandBuffer* commands);
	// The game's own HUD parts hidden as VRShowHud and the headset ask, and
	// shown again after: see HeadsetShowHud. The player's settings for them
	// are never left changed.
	void UpdateHeadsetHud();
	bool HeadsetShowHudNow = false;
	bool HudPartsHidden = false;
	// PT VR: the helper started again, with or without the headset.
	void SwitchHeadset(bool on, FOutputDevice& Ar);
	FString DescribeHeadset() const;
	void CreateBrightnessPipeline();
	void DescribeLightingOf(AActor* target);
	void DescribeLightingAt(ULevel* level, const FVector& point, const FVector& normal, UTexture* texture, bool specialLit, INT iSurf, FOutputDevice& Ar);
	void ApplyBrightness(VulkanCommandBuffer* commands);
	void CreateEncodePipeline();
	void EnsureEncodeImages();
	// The picture encoded for the swap chain in HDR, or back to SDR for a
	// picture saved: see Shaders::Encode.
	void EncodeFrame(VulkanCommandBuffer* commands, bool forSaving);
	// Where the tone curve levels off, as the helper is told it: 1 in SDR.
	float ToneCeiling() const;
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
		// How many times its frame has been sent anew, for PT LOOK.
		uint32_t Advances = 0;
	};
	std::vector<SentTexture> SentTextures;
	// When PT LOOK last counted the animations' advances.
	double AdvancesSince = 0.0;
	// Whether the helper has the level's shadow masks (Scene.Lightmaps).
	bool LightmapsSent = false;
	bool EmittersSent = false;
	std::vector<uint32_t> Pixels;
	int TextureFailuresLogged = 0;
	uint32_t RefusedSeen = 0;
	int RefusalsLogged = 0;
	bool TracerLost = false;
	// Said once in the log, so it can be seen that the engine's own sprites
	// in the level are arriving and being left to the trace: see DrawTile.
	bool LoggedWorldSprite = false;
	bool LoggedVisionMesh = false;

	// PT DENOISE, PT DLSS, PT MATERIALS, PT WIDESCREEN and PT PINNEDUI, for
	// the session; the helper follows.
	bool DenoiseEnabled = false;
	bool DlssEnabled = true;
	int DlssQualityNow = 1;
	int FsrModeNow = 1;
	// What a frame is traced with: VR's own (HeadsetDlssQuality,
	// HeadsetBounces) while it is on, otherwise the screen's.
	int DlssQualityInUse() const { return Clamp(HeadsetNow ? (int)HeadsetDlssQuality : DlssQualityNow, 0, 4); }
	int BouncesInUse() const { return Clamp(HeadsetNow ? (int)HeadsetBounces : (int)Bounces, 1, 255); }
	bool MaterialsEnabled = true;
	bool WidescreenFovEnabled = true;
	float PinnedAspect = 4.0f / 3.0f;
	int LightSizeNow = 4;
	// PT LIGHTING and PT TONEMAP, for the session.
	bool EngineLightingNow = true;
	bool NeutralToneMapNow = true;
	// PT LOOK's probe of the engine's own lightmap: the surface and point it
	// hit, read from the lightmap the engine hands DrawComplexSurface for
	// that surface over the next few frames.
	INT LightmapProbeSurf = -1;
	FVector LightmapProbePoint;
	uint32_t LightmapProbeUntil = 0;
	// PT LOOK TIME: the engine's lightmap at the spot every frame for a while.
	INT LightmapSeriesSurf = -1;
	FVector LightmapSeriesPoint;
	int LightmapSeriesFrames = 0;
	uint32_t LightmapSeriesLastFrame = 0;
	// PT TILES: log what the canvas hands the device in the frame after,
	// armed by the command and live from that frame's Lock.
	bool LogDrawsArmed = false;
	bool LogDraws = false;
	int LoggedDraws = 0;
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
	std::unique_ptr<VulkanPipeline> TilePipelines[4];
	std::unique_ptr<VulkanShader> TileVertexShader;
	std::unique_ptr<VulkanShader> TileFragmentShader;
	std::unique_ptr<VulkanFramebuffer> TileFramebuffer;
	std::unique_ptr<VulkanBuffer> TileVertexBuffer;
	size_t TileVertexCapacity = 0;
	bool TilesUploaded = false;
	// The HUD on its own for the headset: the tiles drawn into a clear image
	// the trace's size, premultiplied, and copied to the helper's. Modulated
	// and translucent tiles blend differently there, having no picture to
	// blend over (Shaders::TileFragmentModulatedHud).
	std::unique_ptr<VulkanImage> HudImage;
	std::unique_ptr<VulkanImageView> HudView;
	std::unique_ptr<VulkanFramebuffer> HudFramebuffer;
	std::unique_ptr<VulkanShader> HudModulatedShader;
	std::unique_ptr<VulkanPipeline> HudTilePipelines[4];
	// The helper's image last drawn into, which the next frame tells it, and
	// which way the head faced when the HUD drawn into it was projected.
	uint32_t HudDrawnGeneration = 0;
	bool HudDrawn = false;
	float HudDrawnHead[4] = {};
	// Which way the head faced when this frame's HUD was projected through
	// it, when it was (SetSceneNode).
	float ProjectedHead[4] = {};
	bool HeadProjected = false;
	// The HUD image's layers: the HUD, what aims with the mouse, the coronas.
	static const int HudLayers = 3;
	// The engine drawing the player's view, from its scene node until the
	// depth is cleared once that view has drawn something of its own: the
	// sky's view, drawn first, ends with a clear too. Tiles drawn meanwhile
	// are the lights' coronas, Render.dll drawing them through the canvas
	// once the world is done; the weapon and the HUD come after the clear.
	bool WorldPass = false;
	bool WorldPassDrawn = false;
	FSceneNode* WorldPassFrame = nullptr;
	// PT VR, for the session; PT VR RECENTER, for the next frame; and
	// whether the last frame went to a headset showing it, when the screen's
	// vsync and FPSLimit step aside for the headset's own pace.
	bool HeadsetNow = false;
	bool HeadsetRecenterAsked = false;
	bool HeadsetShowing = false;
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
	// The picture as the HDR swap chain takes it, and as SDR for a picture
	// saved while HDR is on: the output image's size, made when first wanted.
	std::unique_ptr<VulkanShader> EncodeShader;
	std::unique_ptr<VulkanDescriptorSetLayout> EncodeSetLayout;
	std::unique_ptr<VulkanDescriptorPool> EncodeDescriptorPool;
	std::unique_ptr<VulkanDescriptorSet> PresentSet, SdrSet;
	std::unique_ptr<VulkanPipelineLayout> EncodePipelineLayout;
	std::unique_ptr<VulkanPipeline> EncodePipeline;
	std::unique_ptr<VulkanImage> PresentImage, SdrImage;
	std::unique_ptr<VulkanImageView> PresentView, SdrView;

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

	// The views the HUD draws in windows of their own this frame - a security
	// computer's cameras, the spy drone's, the targeting augmentation's zoom -
	// in the order it draws them, with the rectangle each fills in the trace's
	// pixels (see AddInsetView). Each window's last camera and how many frames
	// it has held still, by its place in that order.
	struct InsetView
	{
		TraceCamera Camera;
		int X = 0, Y = 0, Width = 0, Height = 0;
	};
	std::vector<InsetView> InsetViews;
	InsetView LastInsetViews[TraceProtocol::MaxInsets];
	uint32_t InsetAccumulated[TraceProtocol::MaxInsets] = {};
	// The traced picture again, for the tiles to draw the windows' views back
	// from at their place among the HUD's drawing.
	std::unique_ptr<CachedTexture> InsetSource;
	bool InsetSourceFresh = true;
	// Whether the scene was gathered this frame, for a frame that draws only
	// the windows' views.
	bool CollectedThisFrame = false;
	void AddInsetView(FSceneNode* Frame);

	// The engine's screen flash for this frame, from Lock.
	uint32_t DisableBits = 0;
	FPlane FlashScale = FPlane(0.5f, 0.5f, 0.5f, 0.0f);
	FPlane FlashFog = FPlane(0.0f, 0.0f, 0.0f, 0.0f);

	// Where each frame's time goes, averaged and logged every few hundred
	// frames when LogTimings is set: here, and on the GPU in the helper.
	struct FrameTimings
	{
		double Collect = 0, Send = 0, Textures = 0, Wait = 0, Total = 0, Limit = 0;
		// Waiting for a swap chain image, and handing one to be presented.
		double Acquire = 0, Present = 0;
		int Frames = 0;
		// The helper's side of Send: waiting for the GPU, taking in the scene,
		// recording the frame.
		double HelperWait = 0, HelperApply = 0, HelperRecord = 0;
		int HelperStalls = 0;
		double GpuBuild = 0, GpuTrace = 0, GpuDenoise = 0, GpuComposite = 0;
		int GpuFrames = 0;
		int Logged = 0;
	} Timings;
	// PT BENCH: the costlier features switched off one at a time while the
	// view holds still, and what the frame took with each, to the log. Step
	// is -1 when it is not running.
	struct BenchState
	{
		int Step = -1;
		int Frame = 0;
		// What was set when it started, put back before every step and at
		// the end.
		uint32_t DisableBits = 0;
		bool EngineLighting = true, Detail = true, Vsync = false;
		FLOAT Anisotropy = 0.0f;
		int LightSize = 0;
		INT Bounces = 1;
		INT Wetness = 0;
		INT BumpMapping = 0;
		// This step's sums, from when it has settled.
		double Gpu = 0, Trace = 0, Collect = 0, FrameSum = 0;
		int GpuFrames = 0, Frames = 0;
		struct Result { const TCHAR* Name; double Gpu, Trace, Collect, Frame; };
		std::vector<Result> Results;
		// The first step settles four times as long: straight after a PT
		// DLSS switch, or a level loading, the first row measured 1.4 ms
		// slow against the same settings measured again at the end.
		int SettleFrames() const { return Step == 0 ? 4 * Settle : Settle; }
		bool Measuring() const { return Step >= 0 && Frame >= SettleFrames(); }
		static const int Settle = 60, Measure = 240;
	} Bench;
	void StartBench();
	bool ApplyBenchStep(int step);
	void AdvanceBench();
	void LogBench();

	// PT PHOTO: the world held still and a camera flown free through it,
	// the picture refined sample by sample while the camera rests, and
	// saved to a file. See PhotoMode.cpp.
	struct PhotoState
	{
		// Frames the camera holds still before the preview gives way to the
		// picture refined sample by sample, so that stopping to look around
		// does not start it over and over; the most samples a pixel averages,
		// as the trace shader's history keeps count; and the bounces a
		// photo's paths are allowed, more than a frame can afford.
		static const int SettleFrames = 8;
		static const uint32_t MaxSamples = 4095;
		static const int PathBounces = 8;
		static const int GlossyBounces = 3;

		bool Active = false;
		// The player and level it began in, and what it changed on them,
		// put back as it ends.
		APlayerPawn* Pawn = nullptr;
		ULevel* Level = nullptr;
		FName State;
		BYTE Physics = 0;
		FVector Velocity = FVector(0, 0, 0), Acceleration = FVector(0, 0, 0);
		FRotator Rotation = FRotator(0, 0, 0), ViewRotation = FRotator(0, 0, 0);
		FLOAT EyeHeight = 0.0f;
		bool PlayersOnly = false;
		// The body's pose when it began, drawn in place of the one the
		// flying plays.
		FName AnimSequence;
		FLOAT AnimFrame = 0.0f, AnimRate = 0.0f, TweenRate = 0.0f, AnimLast = 0.0f, AnimMinRate = 0.0f, OldAnimRate = 0.0f;
		FPlane SimAnim = FPlane(0, 0, 0, 0);
		// The key bindings set aside, by key, and the input they came from.
		UInput* Input = nullptr;
		std::vector<std::pair<int, FString>> Bindings;
		// The free camera: where the body's eyes were, and where the camera
		// is now.
		FVector Eye = FVector(0, 0, 0);
		FVector Position = FVector(0, 0, 0);
		double LastMs = 0.0;
		// The level's clock and the flashlight, held as they were.
		float Time = 0.0f;
		vec4 Flashlight[3] = {};
		// Frames the camera has held still, and whether the picture is
		// being refined rather than denoised.
		int StillFrames = 0;
		bool Accumulating = false;
		// The lens, for the session: the aperture's radius in world units,
		// 0 a pinhole, and the distance in focus, 0 for whatever the middle
		// of the view meets (the trace shader's photoFocus).
		float Aperture = 0.0f;
		float Focus = 0.0f;
		// The picture on its way to a file: asked for, then copied out of
		// the frame after the Brightness, then written once the frame is.
		bool SavePending = false;
		bool SaveRecorded = false;
		int SaveWidth = 0, SaveHeight = 0;
		uint32_t SaveSamples = 0;
		std::unique_ptr<VulkanImage> SaveImage;
		std::unique_ptr<VulkanBuffer> SaveBuffer;
	} Photo;
	void PhotoCommand(const TCHAR* Cmd, FOutputDevice& Ar);
	bool StartPhoto(FOutputDevice& Ar);
	void EndPhoto(const TCHAR* why);
	void CheckPhoto();
	void RepairPhotoSave();
	void MovePhotoCamera();
	void SwapPhotoPose();
	bool PhotoConsoleOpen();
	void RecordPhotoSave(VulkanCommandBuffer* commands, VulkanImage* source);
	void WritePhoto();
	// Whether the 2D is left out of this frame: in photo mode, unless the
	// console is open to type into, and never in the frame a photo is taken
	// from.
	bool PhotoHidesTiles() { return Photo.Active && (Photo.SavePending || !PhotoConsoleOpen()); }

	// Every frame's length, Unlock to Unlock, for the frame rate and the
	// slowest frames in the timings log.
	std::vector<float> FrameIntervals;
	double LastUnlockMs = 0.0;
	uint32_t FrameIndex = 0;
	// The last frame a light glowed in fog, for PT BENCH's fog row.
	uint32_t LastFogFrame = 0x80000000u;

	bool HaveCamera = false;
	UBOOL UsingVsync = 0;
	// What the swap chain was made for, and what it took: 0 SDR, 1 scRGB,
	// 2 HDR10.
	UBOOL UsingHdr = 0;
	int HdrMode = 0;

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
