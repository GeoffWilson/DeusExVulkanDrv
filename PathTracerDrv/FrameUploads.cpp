#include "TracePrecomp.h"
#include "FrameUploads.h"

// Big enough for an ordinary frame - a few hundred instances, the lights and
// their grid, a few dozen animated poses - to fit one chunk.
static const size_t MinimumChunk = 4 << 20;

FrameUploads::FrameUploads(VulkanDevice* device) : Device(device)
{
}

FrameUploads::~FrameUploads()
{
	ReleaseChunks();
}

FrameUploads::Chunk FrameUploads::MakeChunk(size_t capacity)
{
	Chunk chunk;
	chunk.Buffer = BufferBuilder()
		.Size(capacity)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerFrameUploads")
		.Create(Device);
	chunk.Mapped = (uint8_t*)chunk.Buffer->Map(0, capacity);
	chunk.Capacity = capacity;
	return chunk;
}

void FrameUploads::ReleaseChunks()
{
	for (Chunk& chunk : Chunks)
		chunk.Buffer->Unmap();
	Chunks.clear();
}

void FrameUploads::Begin()
{
	RetiredBuffers.clear();
	RetiredStructures.clear();
	Copies.clear();

	// A frame that outgrew its staging - a level's first, with the whole world
	// in it - left it in pieces. One of their combined size from now on.
	if (Chunks.size() > 1)
	{
		size_t total = 0;
		for (const Chunk& chunk : Chunks)
			total += chunk.Capacity;
		ReleaseChunks();
		Chunks.push_back(MakeChunk(total));
	}
	for (Chunk& chunk : Chunks)
		chunk.Used = 0;
}

void* FrameUploads::Write(VulkanBuffer* dst, size_t dstOffset, size_t bytes)
{
	const size_t aligned = (bytes + 15) & ~(size_t)15;
	if (Chunks.empty() || Chunks.back().Used + aligned > Chunks.back().Capacity)
	{
		const size_t last = Chunks.empty() ? 0 : Chunks.back().Capacity;
		Chunks.push_back(MakeChunk(std::max({ aligned, MinimumChunk, last * 2 })));
	}

	Chunk& chunk = Chunks.back();
	Copies.push_back({ Chunks.size() - 1, dst, chunk.Used, dstOffset, bytes });
	void* at = chunk.Mapped + chunk.Used;
	chunk.Used += aligned;
	return at;
}

void FrameUploads::Upload(VulkanBuffer* dst, size_t dstOffset, const void* data, size_t bytes)
{
	if (bytes)
		memcpy(Write(dst, dstOffset, bytes), data, bytes);
}

void FrameUploads::Retire(std::unique_ptr<VulkanBuffer> buffer)
{
	if (buffer)
		RetiredBuffers.push_back(std::move(buffer));
}

void FrameUploads::Retire(std::unique_ptr<VulkanAccelerationStructure> structure)
{
	if (structure)
		RetiredStructures.push_back(std::move(structure));
}

void FrameUploads::Record(VulkanCommandBuffer* commands)
{
	if (Copies.empty())
		return;

	for (const Copy& copy : Copies)
	{
		VkBufferCopy region = { copy.SourceOffset, copy.DestinationOffset, copy.Size };
		vkCmdCopyBuffer(commands->buffer, Chunks[copy.Chunk].Buffer->buffer, copy.Destination->buffer, 1, &region);
	}
	Copies.clear();

	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &barrier, 0, nullptr, 0, nullptr);
}
