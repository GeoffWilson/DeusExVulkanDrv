#pragma once

#include "SceneData.h"
#include "TraceProtocol.h"
#include <string>
#include <vector>

class VulkanDevice;
class TraceHost;

// The render device's end of the channel to PathTracerHelper.exe: starts the
// helper on the same GPU, sends it the scene a command at a time, and asks it
// for frames. See TraceProtocol.h.
//
// A 64-bit game needs no helper: built with PATHTRACER_LOCAL, StartLocal puts
// the helper's TraceHost in this process, on the device's own Vulkan device,
// and each batch is handed to it directly. The commands, the answers in the
// header and the frame's hand over with Ready and Released are as they are
// with the helper; only the process, the shared memory and the exported
// handles are gone.
//
// Commands are written straight into the shared memory. A batch that would
// overflow it is sent as it stands and the command starts a new one, so a
// level load's worth of textures goes over in as many batches as it takes.
//
// Nothing here knows the engine: the test harness uses it as the device does.
class TraceClient
{
public:
	~TraceClient();

	// Starts the helper at helperPath, in workingDirectory - where it writes
	// its log - for the GPU device is on. False, with Error() saying why,
	// when it cannot.
	bool Start(VulkanDevice* device, const std::string& helperPath, const std::string& workingDirectory, bool vkDebug);

#ifdef PATHTRACER_LOCAL
	// Traces here instead, on device, which must have been made with ray
	// query and acceleration structures enabled.
	bool StartLocal(VulkanDevice* device);
#endif
	// Tracing in this process rather than the helper's: the frame's image is
	// then not handed across queue families.
	bool IsLocal() const { return Local != nullptr; }

	// Still there, and not given up.
	bool Alive() const { return Shared && !Dead; }
	const std::string& Error() const { return LastError; }
	// Commands dropped for being bigger than the channel holds, which is
	// otherwise without a word: Error() says what the last one was.
	uint32_t Refused() const { return RefusedCount; }
	const TraceProtocol::Header& Status() const { return *Shared; }

	void ResetScene();
	void Geometry(uint32_t index, const SceneGeometry& geometry);
	void Instances(const std::vector<SceneInstance>& instances, int staticGeometries);
	void Lights(const std::vector<SceneLight>& lights, const std::vector<SceneLight>& fogLights);
	void Lightmaps(const std::vector<uint32_t>& words);
	void Emitters(const std::vector<uint32_t>& words);
	// How many of its top levels a texture went without to fit the channel,
	// or -1 when even its smallest would not and it went as an empty slot.
	int Texture(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels, const vec4& material, bool animated, uint32_t levels = 1, uint32_t format = 0);
	void TexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels);

	// Sends what is queued and a request to trace it, and waits until the
	// helper has submitted the frame. True when it did: the caller's next
	// submission must then wait on Ready() before reading Output() and
	// signal Released() - always, once per traced frame, or the helper waits
	// for ever on the next.
	bool Trace(const TraceProtocol::TraceCommand& frame);

	// Sends what is queued without tracing.
	bool Flush();

	// The frame, in the shared image: GENERAL layout, owned by the helper's
	// queue family until taken over as a VK_QUEUE_FAMILY_EXTERNAL transfer.
	VkImage Output() const { return OutputImage; }
	uint32_t OutputWidth() const { return ImportedWidth; }
	uint32_t OutputHeight() const { return ImportedHeight; }
	VkSemaphore Ready() const { return ReadySemaphore; }
	VkSemaphore Released() const { return ReleasedSemaphore; }

private:
	uint8_t* Reserve(uint32_t bytes);
	bool Send();
	bool ImportOutput();
	void ReleaseOutput();
	void Stop();
	bool Die(const std::string& why);

	VulkanDevice* Device = nullptr;
	// Tracing in this process: the host, and the memory standing in for the
	// shared mapping. Only a PATHTRACER_LOCAL build makes one, and deletes it.
	TraceHost* Local = nullptr;
	std::vector<uint8_t> LocalMemory;
	HANDLE Mapping = nullptr;
	HANDLE RequestEvent = nullptr;
	HANDLE ReplyEvent = nullptr;
	HANDLE HelperProcess = nullptr;
	HANDLE Job = nullptr;
	TraceProtocol::Header* Shared = nullptr;
	uint8_t* Commands = nullptr;
	uint32_t Used = 0;
	bool Dead = false;
	std::string LastError;
	uint32_t RefusedCount = 0;

	VkSemaphore ReadySemaphore = VK_NULL_HANDLE;
	VkSemaphore ReleasedSemaphore = VK_NULL_HANDLE;
	VkImage OutputImage = VK_NULL_HANDLE;
	VkDeviceMemory OutputMemory = VK_NULL_HANDLE;
	uint32_t ImportedGeneration = 0;
	uint32_t ImportedWidth = 0, ImportedHeight = 0;
};
