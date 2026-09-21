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
	TopScratch.reset();
	TopBuffer.reset();
	InstanceBuffer.reset();
	LightBuffer.reset();
	AttributeBuffer.reset();
	Bottom.clear();
	AllAttributes.clear();
	TopCapacity = 0;
	Lights = 0;
	attributesChanged = false;
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

void AccelStructure::BuildBottomLevel(const SceneGeometry& geometry, BottomLevel& out)
{
	guard(AccelStructure::BuildBottomLevel);

	VulkanDevice* device = renderer->GetDevice();

	out.Vertices = UploadBuffer(
		geometry.Positions.data(), geometry.Positions.size() * sizeof(vec3),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		"PathTracerVertices");

	const uint32_t triangleCount = (uint32_t)(geometry.Positions.size() / 3);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom.geometry.triangles.vertexData.deviceAddress = out.Vertices->GetDeviceAddress();
	geom.geometry.triangles.vertexStride = sizeof(vec3);
	geom.geometry.triangles.maxVertex = (uint32_t)geometry.Positions.size() - 1;
	geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geom;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triangleCount, &sizes);

	out.Buffer = BufferBuilder()
		.Size(sizes.accelerationStructureSize)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.DebugName("PathTracerBlasBuffer")
		.Create(device);

	out.Structure = AccelerationStructureBuilder()
		.Type(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)
		.Buffer(out.Buffer.get(), sizes.accelerationStructureSize)
		.DebugName("PathTracerBlas")
		.Create(device);

	auto scratch = BufferBuilder()
		.Size(sizes.buildScratchSize)
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerBlasScratch")
		.Create(device);

	buildInfo.dstAccelerationStructure = out.Structure->accelstruct;
	buildInfo.scratchData.deviceAddress = scratch->GetDeviceAddress();

	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = triangleCount;
	const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

	renderer->ExecuteImmediate([&buildInfo, &ranges](VulkanCommandBuffer* cmd)
	{
		cmd->buildAccelerationStructures(1, &buildInfo, ranges);
	});

	unguard;
}

void AccelStructure::SyncGeometry(const LevelScene& scene)
{
	guard(AccelStructure::SyncGeometry);

	// Geometry only ever gets appended within a level, so anything past what is
	// already built is new.
	if (Bottom.size() < scene.Geometries.size())
	{
		for (size_t i = Bottom.size(); i < scene.Geometries.size(); i++)
		{
			BottomLevel level;
			level.AttributeBase = (uint32_t)AllAttributes.size();

			const SceneGeometry& geometry = scene.Geometries[i];
			AllAttributes.insert(AllAttributes.end(), geometry.Attributes.begin(), geometry.Attributes.end());

			BuildBottomLevel(geometry, level);
			Bottom.push_back(std::move(level));
		}

		// One buffer holding every geometry's attributes, which an instance
		// indexes into through its custom index. Rebuilt whole rather than
		// patched: it is a few megabytes at most and this happens when a new
		// shape appears, not every frame.
		AttributeBuffer = UploadBuffer(
			AllAttributes.data(), AllAttributes.size() * sizeof(TriangleAttributes),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "PathTracerAttributes");

		attributesChanged = true;
	}

	if (!LightBuffer)
	{
		Lights = (int)scene.Lights.size();
		if (scene.Lights.empty())
		{
			// A storage buffer may not be zero sized; the shader checks the
			// count before it reads anything.
			SceneLight placeholder = {};
			LightBuffer = UploadBuffer(&placeholder, sizeof(SceneLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "PathTracerLights");
		}
		else
		{
			LightBuffer = UploadBuffer(scene.Lights.data(), scene.Lights.size() * sizeof(SceneLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "PathTracerLights");
		}
		attributesChanged = true;
	}

	unguard;
}

void AccelStructure::EnsureTopLevelCapacity(size_t instanceCount)
{
	if (TopLevel && instanceCount <= TopCapacity)
		return;

	VulkanDevice* device = renderer->GetDevice();

	// Grown with headroom so that a few more actors coming into view does not
	// reallocate the structure every frame.
	TopCapacity = std::max<size_t>(instanceCount * 2, 256);

	InstanceBuffer = BufferBuilder()
		.Size(TopCapacity * sizeof(VkAccelerationStructureInstanceKHR))
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.DebugName("PathTracerInstances")
		.Create(device);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.data.deviceAddress = InstanceBuffer->GetDeviceAddress();

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geom;

	const uint32_t maxInstances = (uint32_t)TopCapacity;
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxInstances, &sizes);

	TopBuffer = BufferBuilder()
		.Size(sizes.accelerationStructureSize)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.DebugName("PathTracerTlasBuffer")
		.Create(device);

	TopLevel = AccelerationStructureBuilder()
		.Type(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
		.Buffer(TopBuffer.get(), sizes.accelerationStructureSize)
		.DebugName("PathTracerTlas")
		.Create(device);

	// Kept rather than allocated per frame; the size only depends on capacity.
	TopScratch = BufferBuilder()
		.Size(sizes.buildScratchSize)
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerTlasScratch")
		.Create(device);

	attributesChanged = true;   // the descriptor points at the structure
}

void AccelStructure::BuildTopLevel(const LevelScene& scene, VulkanCommandBuffer* commands)
{
	guard(AccelStructure::BuildTopLevel);

	if (scene.Instances.empty() || Bottom.empty())
		return;

	EnsureTopLevelCapacity(scene.Instances.size());

	const size_t count = std::min(scene.Instances.size(), TopCapacity);

	auto* mapped = (VkAccelerationStructureInstanceKHR*)InstanceBuffer->Map(0, count * sizeof(VkAccelerationStructureInstanceKHR));
	for (size_t i = 0; i < count; i++)
	{
		const SceneInstance& src = scene.Instances[i];
		VkAccelerationStructureInstanceKHR& dst = mapped[i];
		dst = {};
		memcpy(&dst.transform, src.Transform, sizeof(float) * 12);
		// The custom index is how the trace shader finds this instance's
		// shading data: it is the offset of its geometry's attributes.
		dst.instanceCustomIndex = Bottom[src.GeometryIndex].AttributeBase;
		dst.mask = 0xFF;
		dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		dst.accelerationStructureReference = Bottom[src.GeometryIndex].Structure->GetDeviceAddress();
	}
	InstanceBuffer->Unmap();

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.data.deviceAddress = InstanceBuffer->GetDeviceAddress();

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geom;
	buildInfo.dstAccelerationStructure = TopLevel->accelstruct;
	buildInfo.scratchData.deviceAddress = TopScratch->GetDeviceAddress();

	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = (uint32_t)count;
	const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

	commands->buildAccelerationStructures(1, &buildInfo, ranges);

	// The trace reads what this just wrote.
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
	commands->pipelineBarrier(
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &barrier, 0, nullptr, 0, nullptr);

	unguard;
}
