#pragma once

#include "SceneData.h"
#include <cstddef>
#include <cstdint>

// How the render device in the game's 32-bit process talks to the 64-bit
// helper that traces for it.
//
// The native 32-bit Windows driver offers no ray tracing, and neither does
// Proton to a 32-bit program; every 64-bit client is offered it. So the device
// keeps everything that reads the engine - the level, the actors, the textures,
// the 2D - and hands the tracing to PathTracerHelper.exe on the same GPU (see
// spike/vkxshare.cpp for the measurements that established this works).
//
// Between them:
//   - a shared memory section: this header, then a command area the device
//     fills with a batch of commands - a new level, geometry, textures,
//     instances, lights, and a request to trace a frame;
//   - two auto-reset events: Request, set by the device when a batch is
//     written, and Reply, set by the helper once it has dealt with it;
//   - an image the helper traces into and the device presents from, and two
//     binary semaphores ordering them on the GPU: Ready, signalled by the helper
//     once a frame is in the image, and Released, signalled by the device once
//     it has copied it out;
//   - with a headset, a second image going the other way: the device draws
//     the HUD into it after taking a frame, before signalling Released, and
//     the helper puts it on the headset's panel in its next frame, after
//     waiting for Released.
//
// Everything here has the same layout in a 32-bit and a 64-bit build: fixed
// size fields, 64-bit ones on 8 byte boundaries, and the checks at the end to
// hold it to that.

namespace TraceProtocol
{
	static const uint32_t Magic = 0x31485450;   // "PTH1"
	static const uint32_t Version = 23;

	// TraceCommand::Denoise.
	enum DenoiserChoice : uint32_t
	{
		DenoiseOff = 0,
		DenoiseNrd = 1,
		// DLSS Ray Reconstruction, and NRD wherever it cannot run.
		DenoiseDlss = 2,
		// Reported only (Header's DenoisedWith): NRD at the render size, and
		// FSR 3.1 bringing the picture up to the output's.
		DenoiseNrdFsr = 3,
	};

	// TraceCommand::Upscaler: whether NRD's picture is traced at a fraction
	// of the output's size and AMD's FSR 3.1 brings it up to it, at the
	// DlssQuality's ratio.
	enum UpscalerChoice : uint32_t
	{
		UpscaleNone = 0,
		// Where Ray Reconstruction was asked for and cannot run: an AMD or
		// Intel GPU, a missing NGX.
		UpscaleWithoutRr = 1,
		// Always, NRD and FSR in place of Ray Reconstruction.
		UpscaleAlways = 2,
	};

	// The output image, as both sides must create it for the one allocation to
	// be valid in both.
	static const VkFormat OutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const VkImageUsageFlags OutputUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	// The HUD image likewise: the device copies the HUD in, and the helper
	// reads it in a compute pass on its way to the headset. Premultiplied
	// alpha, gamma encoded as the picture is.
	static const VkFormat HudFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const VkImageUsageFlags HudUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

	// Header::Headset.
	enum HeadsetState : uint32_t
	{
		HeadsetOff = 0,         // not asked for: the helper was started without one
		HeadsetMissing = 1,     // asked for, but there is none to show on (HeadsetStatus says why)
		HeadsetIdle = 2,        // there is one, but it is not showing this program: asleep, or off the head
		HeadsetShowing = 3,     // frames are going to it
	};

	// How long the device waits on the helper before giving up on it. Loading a
	// level uploads every texture and builds the static world, which is
	// seconds, not frames.
	static const uint32_t StartupTimeoutMs = 30000;
	static const uint32_t BatchTimeoutMs = 20000;

#pragma pack(push, 8)
	struct Header
	{
		uint32_t Magic;
		uint32_t Version;
		uint32_t CommandCapacity;   // bytes of command area after this header
		uint32_t CommandBytes;      // written in the current batch

		// Set by the device for each batch; the helper copies it to ReplySerial
		// once the batch is done, so a reply can be matched to its batch.
		uint32_t BatchSerial;
		uint32_t ReplySerial;

		// Written by the helper. Status is 0 while all is well; otherwise the
		// helper has given up and Error says why.
		int32_t Status;
		uint32_t Ready;             // start-up is complete
		uint32_t RayTracing;        // the helper's device offers it
		uint32_t CanSampleTextures; // and indexes the texture array per ray

		// The semaphores, as handles already duplicated into the device's
		// process.
		uint64_t ReadySemaphore;
		uint64_t ReleasedSemaphore;

		// The output image. A new generation whenever the helper has had to
		// make a new one - the trace size changed - with its memory as a handle
		// in the device's process and the allocation's size, which an import
		// must match.
		uint64_t OutputMemory;
		uint64_t OutputAllocationSize;
		uint32_t OutputGeneration;
		uint32_t OutputWidth;
		uint32_t OutputHeight;

		// The last batch submitted a traced frame: Ready will be signalled, and
		// the device must wait on it and signal Released in return. With a
		// headset every frame is handed over like that, traced or not, to
		// carry the HUD back; OutputFresh says whether the output image holds
		// a new picture to copy out.
		uint32_t Traced;
		uint32_t OutputFresh;
		// The headset, a HeadsetState.
		uint32_t Headset;

		// The HUD image, made by the helper when a frame asks for it at a new
		// size, as the output image is.
		uint64_t HudMemory;
		uint64_t HudAllocationSize;
		uint32_t HudGeneration;
		uint32_t HudWidth;
		uint32_t HudHeight;

		// Each eye's picture's size, with the headset showing.
		uint32_t HeadsetEyeWidth;
		uint32_t HeadsetEyeHeight;
		// Which way the head faced in the seat's space at the last frame the
		// headset showing - a quaternion, x, y, z, w, in OpenXR's terms -
		// for the device to have the HUD projected through; HeadValid 1 once
		// there has been one.
		float HeadOrientation[4];
		uint32_t HeadValid;
		uint32_t HeadPad;

		// About the last traced frame, for the device's log.
		uint32_t LightCount;
		uint32_t TextureCount;
		uint32_t BottomCount;
		uint32_t InstanceCount;
		float GpuBuildMs;           // top level structure and uploads
		float GpuTraceMs;
		float GpuDenoiseMs;
		float GpuCompositeMs;
		uint32_t GpuTimed;          // the four figures above are from this frame
		uint32_t DenoiserActive;

		// The helper's own time on the last batch: waiting for the GPU, taking
		// in the scene, and recording and submitting the frame. Stalls counts
		// the waits for a frame still in flight, which only a new level, a
		// new texture or a buffer outgrowing itself should cause.
		float HelperWaitMs;
		float HelperApplyMs;
		float HelperRecordMs;
		uint32_t HelperStalls;

		// What the last frame was traced at and denoised with: the render size,
		// smaller than the output when DLSS upscales, and a DenoiserChoice.
		uint32_t RenderWidth;
		uint32_t RenderHeight;
		uint32_t DenoisedWith;
		uint32_t Pad;

		char DeviceName[256];
		char Error[512];
		char DenoiserStatus[128];
		char DlssStatus[160];       // "ready", or why Ray Reconstruction cannot run
		char HeadsetStatus[256];    // what it is and runs at, or why there is none
	};
#pragma pack(pop)

	enum CommandType : uint32_t
	{
		CmdResetScene = 1,
		CmdGeometry,
		CmdInstances,
		CmdLights,
		CmdTexture,
		CmdTexturePixels,
		CmdTrace,
		CmdQuit,
		CmdLightmaps,
		CmdEmitters,
	};

	// Every command starts with this, and Bytes covers the header and whatever
	// follows it, rounded up to a multiple of 8.
	struct CommandHeader
	{
		uint32_t Type;
		uint32_t Bytes;
	};

	// Replaces geometry Index, or adds it. Followed by PositionCount vec3 and
	// then TriangleCount TriangleAttributes.
	struct GeometryCommand
	{
		CommandHeader H;
		uint32_t Index;
		uint32_t Dynamic;
		uint32_t HasMasked;
		uint32_t Version;
		uint32_t PositionCount;
		uint32_t TriangleCount;
	};

	// One placement. SceneInstance with its bool made a word, so it lays out
	// the same everywhere.
	struct WireInstance
	{
		int32_t GeometryIndex;
		uint32_t HasPrevious;
		uint32_t Mask;
		uint32_t Pad;
		float Transform[12];
		float PreviousTransform[12];
		vec4 Ambient;
	};

	// The whole placement list for the frame. Followed by Count WireInstance.
	struct InstancesCommand
	{
		CommandHeader H;
		uint32_t Count;
		uint32_t StaticGeometries;
	};

	// The lights, then the fog lights. Followed by LightCount + FogCount
	// SceneLight.
	struct LightsCommand
	{
		CommandHeader H;
		uint32_t LightCount;
		uint32_t FogCount;
	};

	// The engine's shadow masks on the level's surfaces, SceneData's
	// Lightmaps. Followed by Words words.
	struct LightmapsCommand
	{
		CommandHeader H;
		uint32_t Words;
		uint32_t Pad;
	};

	// The level's glowing surfaces, SceneData's Emitters, laid out as
	// EmitterGrid.h has them. Followed by Words words.
	struct EmittersCommand
	{
		CommandHeader H;
		uint32_t Words;
		uint32_t Pad;
	};

	// How a texture's pixels come: RGBA8, or S3TC's 4x4 blocks as they were
	// stored (BC1) - New Vision's, which unpacked would be eight times the
	// size.
	enum TextureFormat : uint32_t
	{
		TextureRgba8 = 0,
		TextureBc1 = 1,
	};

	// A texture array slot: its pixels, MipLevels levels of them end to end
	// from Width * Height down, each half the last (or none for a texture the
	// device could not convert, which is bound white), in Format, and what the
	// surfaces using it are made of.
	struct TextureCommand
	{
		CommandHeader H;
		uint32_t Index;
		uint32_t Width;
		uint32_t Height;
		uint32_t Animated;          // its pixels change: keep them uploadable
		uint32_t MipLevels;
		uint32_t Format;            // a TextureFormat
		vec4 Material;
	};

	// How many bytes one mip of width x height holds in a format.
	inline size_t MipBytes(uint32_t format, uint32_t width, uint32_t height)
	{
		if (format == TextureBc1)
			return (size_t)((width + 3) / 4 ? (width + 3) / 4 : 1) * ((height + 3) / 4 ? (height + 3) / 4 : 1) * 8;
		return (size_t)width * height * 4;
	}

	// How many bytes a chain of mips from width x height holds.
	inline size_t MipChainBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t levels)
	{
		size_t total = 0;
		for (uint32_t i = 0; i < levels; i++)
			total += MipBytes(format, width >> i ? width >> i : 1, height >> i ? height >> i : 1);
		return total;
	}

	// New pixels for a slot that already exists, at the size it has: a frame
	// of fire, water, a screen.
	struct TexturePixelsCommand
	{
		CommandHeader H;
		uint32_t Index;
		uint32_t Width;
		uint32_t Height;
		uint32_t Pad;
	};

	// Trace a frame of the scene as it now stands, at Width x Height.
	// A view the HUD draws inside a window of its own - a security camera,
	// the spy drone, the targeting augmentation's zoom - traced from its own
	// viewpoint after the player's and written into the output at its
	// rectangle, in output pixels. Its camera as TraceCommand's; its samples
	// averaged over AccumulatedFrames frames, 0 when it has moved.
	static const uint32_t MaxInsets = 4;
	struct TraceInset
	{
		vec4 Camera[4];
		uint32_t X, Y, Width, Height;
		uint32_t AccumulatedFrames;
		uint32_t Pad[3];
	};

	struct TraceCommand
	{
		CommandHeader H;
		uint32_t Width;
		uint32_t Height;
		uint32_t Frame;             // counts up by one every traced frame
		uint32_t AccumulatedFrames; // 0 when the device says history is invalid
		uint32_t MaxSamples;
		uint32_t Bounces;
		uint32_t GlossBounces;
		uint32_t DisableBits;       // PT's switches, as the device keeps them
		uint32_t ViewMode;          // PT VIEW, 0 for the picture
		uint32_t DebugMode;
		uint32_t Denoise;           // a DenoiserChoice
		uint32_t Materials;
		uint32_t RestartDenoiser;   // camera cut or new level
		uint32_t Timing;            // LogTimings: time the GPU's work
		float Time;                 // the level's clock
		float Exposure;
		float SkyIntensity;
		uint32_t DlssQuality;       // 0 DLAA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
		uint32_t Upscaler;          // an UpscalerChoice
		float UpscaleSharpness;     // FSR's sharpening, 0 none to 1
		uint32_t LightSize;         // radius shadows are cast from around each light, in world units; 0 a point
		uint32_t MaxAnisotropy;     // most samples the texture filter takes along a footprint; 1 or less, none
		// 1 when the level's surfaces take their lights as the engine's
		// lightmaps do, 0 each linearly. The device's Lighting.
		uint32_t Lighting;
		uint32_t InsetCount;        // how many of Insets are in use
		// How much the level's glowing surfaces light what is around them,
		// 1 as they glow: the device's GlowLighting.
		float GlowLighting;
		// How wet the level's ground is where rain would reach it, 0 dry to
		// 1 soaked: the device's Wetness.
		float Wetness;
		// Origin, right, up and forward, as the trace shader's push constants
		// carry them - w holding the screen flash - and the same for last frame.
		vec4 Camera[4];
		vec4 PreviousCamera[4];
		vec4 SkyOrigin;
		// Which way the sky zone faces, as the trace shader's skyAxes has it:
		// where a direction in the level along x, y and z looks in the sky
		// zone, w unused. The engine's skybox camera is the view's turned by
		// the SkyZoneInfo's rotation, so a view along the level's x looks
		// into the skybox along the first.
		vec4 SkyAxes[3];
		// The light augmentation as a flashlight, as the trace shader's
		// flashlight[] has it: xyz where it shines from, w 1 while it is on;
		// xyz which way, w its intensity; its colour, linear, w how much of
		// it the air scatters back. All zero while it is off.
		vec4 Flashlight[3];
		// Photo mode's lens, as the trace shader's photoLens has it: x the
		// aperture's radius in world units, 0 for a pinhole; y how far ahead
		// it is focused, 0 for whatever the middle of the view meets (the
		// shader's photoFocus). Read only while Disable bit 524288 is set.
		vec4 PhotoLens;
		// How deep the textures' relief is drawn, 1 as the materials give
		// it and 0 for none: the device's BumpMapping.
		float BumpMapping;
		// Where the tone curve levels off, in the white the picture's SDR
		// range is drawn at: 1 for an SDR display, and for an HDR one its
		// peak over that white - 1000 nits over 200 is 5 - so what the SDR
		// curve squeezed into the last fifth below white spreads out above
		// it. 0 is taken as 1.
		float ToneCeiling;

		// For the headset, when the helper was started with one: 1 to trace
		// the frame for it rather than the screen, each eye from where it is
		// in the room around the view Camera describes, whose middle is
		// straight ahead where the player's seat faces. The screen is then
		// sent the left eye's picture, at Width x Height. NoWorld is a frame
		// with nothing to trace - a menu over no level - when the headset
		// keeps the last one, turned as the head turns, under the HUD.
		uint32_t Headset;
		uint32_t NoWorld;
		// How many world units make a metre, for how far apart the eyes are
		// and how far the head moves the view.
		float UnitsPerMetre;
		// The eyes' pictures as a share of the size the headset asks for.
		float HeadsetResolution;
		// The HUD's panel: how far in front of the seat, in metres. It spans
		// the view Camera describes, so what the HUD marks lines up with the
		// world behind it.
		float HudDistance;
		// The game's Brightness, as the gamma the device puts over the
		// screen's picture: the headset's pictures have it put on by the
		// helper.
		float HeadsetGamma;
		// The HUD image the device drew into last frame: its generation, and 1
		// when it did draw into it (Header's HudGeneration). HudWidth and
		// HudHeight are the size it wants from now on, 0 for none.
		uint32_t HudDrawnGeneration;
		uint32_t HudDrawn;
		uint32_t HudWidth;
		uint32_t HudHeight;
		// The HUD comes in HudLayers layers, one above the other in its
		// image: the HUD itself at the top, which goes on a panel in front of
		// the head; below it what aims with the mouse - the crosshair and the
		// accuracy reticle - on a panel along the view Camera describes; and
		// below that, where there are three, the lights' coronas, projected
		// through the head as the HUD is, but on a panel far out, so the eyes
		// meet on them out where the lights are rather than at the HUD.
		// HudRect is the part of each layer to show, in pixels from the top
		// of its layer, and HudTangents the tangents of its half extents
		// across and down, which every panel spans. HudHead is which way the
		// head faced when the HUD drawn last frame was projected (Header's
		// HeadOrientation), which its panels are placed along, so what the
		// HUD marks lines up with the world behind it.
		uint32_t HudRect[4];
		float HudTangents[2];
		float HudHead[4];
		// 1 to make wherever the head is now straight ahead, at the middle of
		// the view (PT VR RECENTER).
		uint32_t HeadsetRecenter;
		uint32_t HudLayers;
		TraceInset Insets[MaxInsets];
	};

	inline uint32_t Rounded(size_t bytes) { return (uint32_t)((bytes + 7) & ~size_t(7)); }

	static_assert(sizeof(vec4) == 16 && sizeof(vec3) == 12, "vector types must be plain floats");
	static_assert(sizeof(TriangleAttributes) == 128 && sizeof(SceneLight) == 80, "scene records must match in both builds");
	static_assert(offsetof(Header, ReadySemaphore) % 8 == 0 && offsetof(Header, OutputMemory) % 8 == 0 && offsetof(Header, HudMemory) == 96, "64-bit fields must be aligned alike");
	static_assert(sizeof(Header) % 8 == 0, "header must keep the command area aligned");
	static_assert(sizeof(WireInstance) == 128, "instance record must match in both builds");
	static_assert(sizeof(TraceInset) == 96, "inset record must match in both builds");
	static_assert(sizeof(TraceCommand) % 8 == 0 && sizeof(GeometryCommand) % 8 == 0 && sizeof(TextureCommand) % 8 == 0, "commands keep 8 byte alignment");
}
