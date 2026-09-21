#include "Precomp.h"
#include "AccelStructure.h"
#include "UPathTracerRenderDevice.h"

AccelStructure::AccelStructure(UPathTracerRenderDevice* renderer) : renderer(renderer)
{
}

AccelStructure::~AccelStructure()
{
	Reset();
}

void AccelStructure::Reset()
{
	TopLevel.reset();
	BottomLevel.reset();
	TopBuffer.reset();
	BottomBuffer.reset();
	InstanceBuffer.reset();
	LightBuffer.reset();
	AttributeBuffer.reset();
	VertexBuffer.reset();
	Lights = 0;
}

std::unique_ptr<VulkanBuffer> AccelStructure::UploadBuffer(const void* data, size_t size, VkBufferUsageFlags usage, const char* debugName)
{
	auto staging = BufferBuilder()
		.Size(size)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerStaging")
		.Create(renderer->GetDevice());

	void* mapped = staging->Map(0, size);
	memcpy(mapped, data, size);
	staging->Unmap();

	auto buffer = BufferBuilder()
		.Size(size)
		.Usage(usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.DebugName(debugName)
		.Create(renderer->GetDevice());

	VulkanBuffer* src = staging.get();
	VulkanBuffer* dst = buffer.get();
	renderer->ExecuteImmediate([src, dst, size](VulkanCommandBuffer* cmd)
	{
		cmd->copyBuffer(src, dst, 0, 0, size);
	});

	return buffer;
}

void AccelStructure::Build(const LevelScene& scene)
{
	guard(AccelStructure::Build);

	Reset();

	if (scene.Positions.empty())
		return;

	VulkanDevice* device = renderer->GetDevice();

	// --- The geometry the structure is built over -------------------------

	VertexBuffer = UploadBuffer(
		scene.Positions.data(), scene.Positions.size() * sizeof(vec3),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		"PathTracerVertices");

	AttributeBuffer = UploadBuffer(
		scene.Attributes.data(), scene.Attributes.size() * sizeof(TriangleAttributes),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		"PathTracerAttributes");

	// A storage buffer may not be zero sized, and a level with no lights at all
	// is unusual but not impossible - the shader checks the count before it
	// reads anything.
	Lights = (int)scene.Lights.size();
	if (scene.Lights.empty())
	{
		SceneLight placeholder = {};
		LightBuffer = UploadBuffer(&placeholder, sizeof(SceneLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "PathTracerLights");
	}
	else
	{
		LightBuffer = UploadBuffer(scene.Lights.data(), scene.Lights.size() * sizeof(SceneLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "PathTracerLights");
	}

	const uint32_t triangleCount = (uint32_t)(scene.Positions.size() / 3);

	// --- Bottom level -----------------------------------------------------

	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexData.deviceAddress = VertexBuffer->GetDeviceAddress();
	geometry.geometry.triangles.vertexStride = sizeof(vec3);
	geometry.geometry.triangles.maxVertex = (uint32_t)scene.Positions.size() - 1;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triangleCount, &sizes);

	BottomBuffer = BufferBuilder()
		.Size(sizes.accelerationStructureSize)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.DebugName("PathTracerBlasBuffer")
		.Create(device);

	BottomLevel = AccelerationStructureBuilder()
		.Type(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)
		.Buffer(BottomBuffer.get(), sizes.accelerationStructureSize)
		.DebugName("PathTracerBlas")
		.Create(device);

	{
		// The scratch allocation is transient, but its alignment is not
		// negotiable and the driver will not tell you politely if it is wrong.
		auto scratch = BufferBuilder()
			.Size(sizes.buildScratchSize)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerBlasScratch")
			.Create(device);

		buildInfo.dstAccelerationStructure = BottomLevel->accelstruct;
		buildInfo.scratchData.deviceAddress = scratch->GetDeviceAddress();

		VkAccelerationStructureBuildRangeInfoKHR range = {};
		range.primitiveCount = triangleCount;
		const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

		renderer->ExecuteImmediate([&buildInfo, &ranges](VulkanCommandBuffer* cmd)
		{
			cmd->buildAccelerationStructures(1, &buildInfo, ranges);
		});
	}

	// --- Top level --------------------------------------------------------

	VkAccelerationStructureInstanceKHR instance = {};
	// Identity: the level is already in world space, so there is nothing to
	// transform. Movers would each want their own instance and matrix here.
	instance.transform.matrix[0][0] = 1.0f;
	instance.transform.matrix[1][1] = 1.0f;
	instance.transform.matrix[2][2] = 1.0f;
	instance.mask = 0xFF;
	instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance.accelerationStructureReference = BottomLevel->GetDeviceAddress();

	InstanceBuffer = UploadBuffer(&instance, sizeof(instance),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		"PathTracerInstances");

	VkAccelerationStructureGeometryKHR topGeometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	topGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	topGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	topGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	topGeometry.geometry.instances.data.deviceAddress = InstanceBuffer->GetDeviceAddress();

	VkAccelerationStructureBuildGeometryInfoKHR topBuild = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	topBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	topBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	topBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	topBuild.geometryCount = 1;
	topBuild.pGeometries = &topGeometry;

	const uint32_t instanceCount = 1;
	VkAccelerationStructureBuildSizesInfoKHR topSizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topBuild, &instanceCount, &topSizes);

	TopBuffer = BufferBuilder()
		.Size(topSizes.accelerationStructureSize)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.DebugName("PathTracerTlasBuffer")
		.Create(device);

	TopLevel = AccelerationStructureBuilder()
		.Type(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
		.Buffer(TopBuffer.get(), topSizes.accelerationStructureSize)
		.DebugName("PathTracerTlas")
		.Create(device);

	{
		auto scratch = BufferBuilder()
			.Size(topSizes.buildScratchSize)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerTlasScratch")
			.Create(device);

		topBuild.dstAccelerationStructure = TopLevel->accelstruct;
		topBuild.scratchData.deviceAddress = scratch->GetDeviceAddress();

		VkAccelerationStructureBuildRangeInfoKHR range = {};
		range.primitiveCount = instanceCount;
		const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

		renderer->ExecuteImmediate([&topBuild, &ranges](VulkanCommandBuffer* cmd)
		{
			// The bottom level build has to have finished before the top level
			// reads its device address.
			VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
			cmd->pipelineBarrier(
				VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				0, 1, &barrier, 0, nullptr, 0, nullptr);

			cmd->buildAccelerationStructures(1, &topBuild, ranges);
		});
	}

	unguard;
}
