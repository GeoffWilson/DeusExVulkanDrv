#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;
class VulkanBuffer;
class VulkanCommandBuffer;
class VulkanAccelerationStructure;

// What a frame writes from the host for the GPU to read - the instances, the
// lights and their grid, the motion, the shapes that animate - staged here and
// copied into the buffers the GPU reads by the frame's own command buffer,
// rather than written into those buffers directly.
//
// The helper records a frame while the one before is still on the GPU, reading
// the same buffers. A host write would change what that frame sees half way
// through; a copy on the GPU lands after it, in queue order. Each frame in
// flight stages into its own FrameUploads, reused once that frame is done.
//
// It also keeps whatever a frame replaced - a buffer outgrown, a structure
// rebuilt bigger - alive until the frames that might still read it are done.
class FrameUploads
{
public:
	FrameUploads(VulkanDevice* device);
	~FrameUploads();

	// The frame last staged here has finished on the GPU: its staging can be
	// written again and what it retired can go.
	void Begin();

	// Room for bytes, copied into dst at dstOffset when Record runs.
	void* Write(VulkanBuffer* dst, size_t dstOffset, size_t bytes);
	void Upload(VulkanBuffer* dst, size_t dstOffset, const void* data, size_t bytes);

	// Freed once this frame, and so every frame before it, has finished.
	void Retire(std::unique_ptr<VulkanBuffer> buffer);
	void Retire(std::unique_ptr<VulkanAccelerationStructure> structure);

	// The copies, then a barrier making them visible to the structure builds
	// and the shaders after them.
	void Record(VulkanCommandBuffer* commands);

private:
	struct Chunk
	{
		std::unique_ptr<VulkanBuffer> Buffer;
		uint8_t* Mapped = nullptr;
		size_t Capacity = 0;
		size_t Used = 0;
	};
	struct Copy
	{
		size_t Chunk;
		VulkanBuffer* Destination;
		uint64_t SourceOffset;
		uint64_t DestinationOffset;
		uint64_t Size;
	};

	Chunk MakeChunk(size_t capacity);
	void ReleaseChunks();

	VulkanDevice* Device = nullptr;
	std::vector<Chunk> Chunks;
	std::vector<Copy> Copies;
	std::vector<std::unique_ptr<VulkanBuffer>> RetiredBuffers;
	std::vector<std::unique_ptr<VulkanAccelerationStructure>> RetiredStructures;
};
