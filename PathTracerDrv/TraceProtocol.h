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
//     it has copied it out.
//
// Everything here has the same layout in a 32-bit and a 64-bit build: fixed
// size fields, 64-bit ones on 8 byte boundaries, and the checks at the end to
// hold it to that.

namespace TraceProtocol
{
	static const uint32_t Magic = 0x31485450;   // "PTH1"
	static const uint32_t Version = 1;

	// The output image, as both sides must create it for the one allocation to
	// be valid in both.
	static const VkFormat OutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const VkImageUsageFlags OutputUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

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
		// the device must wait on it and signal Released in return.
		uint32_t Traced;

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

		char DeviceName[256];
		char Error[512];
		char DenoiserStatus[128];
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

	// A texture array slot: its pixels as RGBA8 (Width * Height of them, or
	// none for a texture the device could not convert, which is bound white)
	// and what the surfaces using it are made of.
	struct TextureCommand
	{
		CommandHeader H;
		uint32_t Index;
		uint32_t Width;
		uint32_t Height;
		uint32_t Animated;          // its pixels change: keep them uploadable
		vec4 Material;
	};

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
		uint32_t Denoise;
		uint32_t Materials;
		uint32_t RestartDenoiser;   // camera cut or new level
		uint32_t Timing;            // LogTimings: time the GPU's work
		float Time;                 // the level's clock
		float Exposure;
		float SkyIntensity;
		float Pad;
		// Origin, right, up and forward, as the trace shader's push constants
		// carry them - w holding the screen flash - and the same for last frame.
		vec4 Camera[4];
		vec4 PreviousCamera[4];
		vec4 SkyOrigin;
	};

	inline uint32_t Rounded(size_t bytes) { return (uint32_t)((bytes + 7) & ~size_t(7)); }

	static_assert(sizeof(vec4) == 16 && sizeof(vec3) == 12, "vector types must be plain floats");
	static_assert(sizeof(TriangleAttributes) == 96 && sizeof(SceneLight) == 64, "scene records must match in both builds");
	static_assert(offsetof(Header, ReadySemaphore) % 8 == 0 && offsetof(Header, OutputMemory) % 8 == 0, "64-bit fields must be aligned alike");
	static_assert(sizeof(Header) % 8 == 0, "header must keep the command area aligned");
	static_assert(sizeof(WireInstance) == 120, "instance record must match in both builds");
	static_assert(sizeof(TraceCommand) % 8 == 0 && sizeof(GeometryCommand) % 8 == 0 && sizeof(TextureCommand) % 8 == 0, "commands keep 8 byte alignment");
}
