#include "Precomp.h"
#include "AccelStructure.h"
#include "UPathTracerRenderDevice.h"

// The driver strides through the instance array by its own idea of this
// struct's size. This package compiles the engine's 4 byte packed headers
// alongside the Vulkan ones, and getting that wrong here would look exactly
// like the first instance working and none of the others existing.
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64, "instance struct is the wrong size");
static_assert(offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference) == 56, "instance struct is laid out wrong");

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
	InstanceDataBuffer.reset();
	LightBuffer.reset();
	AttributeBuffer.reset();
	AttributeCapacity = 0;
	haveDynamic = false;
	Bottom.clear();
	AllAttributes.clear();
	TopCapacity = 0;
	Lights = 0;
	LightCapacity = 0;
	attributesChanged = false;
	LoggedInstances = false;
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

	// Aligned explicitly. A buffer handed to an acceleration structure build has
	// an alignment requirement on its device address, and without asking, a
	// small allocation gets suballocated wherever it fits inside a larger block.
	// A large buffer tends to land on a well aligned boundary by luck, which is
	// exactly how this hid: the static world is about a megabyte and built
	// correctly, while every prop and character is a few kilobytes and did not.
	auto buffer = BufferBuilder()
		.Size(size)
		.Usage(usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.MinAlignment(256)
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

	out.TriangleCount = (int)(geometry.Positions.size() / 3);

	out.Vertices = UploadBuffer(
		geometry.Positions.data(), geometry.Positions.size() * sizeof(vec3),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		"PathTracerVertices");

	const uint32_t triangleCount = (uint32_t)(geometry.Positions.size() / 3);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	// Opaque wherever nothing is masked, which is nearly everything: traversal
	// then accepts a hit outright instead of asking the shader about every
	// candidate triangle it crosses.
	geom.flags = geometry.HasMasked ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
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

			if (geometry.Dynamic)
			{
				CreateDynamicBottomLevel(geometry, level);
				haveDynamic = true;
			}
			else
			{
				BuildBottomLevel(geometry, level);
			}
			Bottom.push_back(std::move(level));
		}

		// One buffer holding every geometry's attributes, which an instance
		// indexes into through its custom index. Host visible, because an
		// animated actor's normals change with its pose and its slice has to be
		// rewritten every frame.
		EnsureAttributeCapacity(AllAttributes.size());
		if (AttributeBuffer && !AllAttributes.empty())
		{
			void* mapped = AttributeBuffer->Map(0, AllAttributes.size() * sizeof(TriangleAttributes));
			memcpy(mapped, AllAttributes.data(), AllAttributes.size() * sizeof(TriangleAttributes));
			AttributeBuffer->Unmap();
		}
	}

	WriteDynamicGeometry(scene);

	// Rewritten every frame rather than built once. A light that moves, a flare
	// that is thrown, and the player's own light augmentation all change the
	// list, and a list uploaded at level load could express none of them.
	// The fog lights follow the ordinary ones in the same buffer, and the grid
	// carries how many there are: the push constants have no room left.
	Lights = (int)scene.Lights.size();
	const size_t wanted = std::max<size_t>(scene.Lights.size() + scene.FogLights.size(), 1);
	if (!LightBuffer || wanted > LightCapacity)
	{
		LightCapacity = std::max<size_t>(wanted * 2, 256);
		LightBuffer = BufferBuilder()
			.Size(LightCapacity * sizeof(SceneLight))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.MinAlignment(256)
			.DebugName("PathTracerLights")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}

	{
		auto* mapped = (SceneLight*)LightBuffer->Map(0, wanted * sizeof(SceneLight));
		if (scene.Lights.empty())
		{
			// A storage buffer may not be zero sized; the shader checks the
			// count before it reads anything.
			SceneLight placeholder = {};
			mapped[0] = placeholder;
		}
		else
		{
			memcpy(mapped, scene.Lights.data(), scene.Lights.size() * sizeof(SceneLight));
		}
		if (!scene.FogLights.empty())
			memcpy(mapped + scene.Lights.size(), scene.FogLights.data(), scene.FogLights.size() * sizeof(SceneLight));
		LightBuffer->Unmap();
	}

	WriteLightGrid(scene);

	unguard;
}

// A uniform grid over everywhere a light can reach, each cell listing the
// lights whose reach touches it.
//
// Every shaded point used to weigh every light in the level. Liberty Island
// has 126, most of them nowhere near any given point, and that loop - not the
// shadow rays, not the geometry - was nearly the whole frame: with lighting
// switched off the frame rate went from 56 to the cap.
//
// Laid out as one array of words: the grid's origin and cell size as floats,
// its dimensions, then a start and count per cell, then the light indices the
// starts point into.
void AccelStructure::WriteLightGrid(const LevelScene& scene)
{
	guard(AccelStructure::WriteLightGrid);

	const size_t lightCount = scene.Lights.size();
	auto reachOf = [&](size_t i) { return std::abs(scene.Lights[i].PositionRadius.w); };

	// The box around every light's reach.
	vec3 lo(0.0f), hi(0.0f);
	for (size_t i = 0; i < lightCount; i++)
	{
		const vec4& p = scene.Lights[i].PositionRadius;
		const float r = reachOf(i);
		const vec3 a(p.x - r, p.y - r, p.z - r), b(p.x + r, p.y + r, p.z + r);
		if (i == 0) { lo = a; hi = b; }
		else
		{
			lo = vec3(std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z));
			hi = vec3(std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z));
		}
	}

	// Cells sized so the grid holds a few tens of thousands at most, and no
	// smaller than a room.
	const vec3 extent = hi - lo;
	const float volume = std::max(extent.x, 1.0f) * std::max(extent.y, 1.0f) * std::max(extent.z, 1.0f);
	const float cellSize = std::max(256.0f, std::cbrt(volume / 32768.0f));
	uint32_t dims[3];
	for (int a = 0; a < 3; a++)
		dims[a] = lightCount ? (uint32_t)std::min(64.0f, std::max(1.0f, std::ceil(extent[a] / cellSize))) : 0u;
	const uint32_t cells = dims[0] * dims[1] * dims[2];

	auto cellRange = [&](float centre, float r, int axis, int& first, int& last)
	{
		first = std::max(0, (int)std::floor((centre - r - lo[axis]) / cellSize));
		last = std::min((int)dims[axis] - 1, (int)std::floor((centre + r - lo[axis]) / cellSize));
	};

	// Does this light's reach touch that cell? A sphere against a box, or for
	// a cylinder light - whose reach is measured across the floor only - a
	// circle against the box's footprint.
	auto touches = [&](size_t i, int x, int y, int z)
	{
		const vec4& p = scene.Lights[i].PositionRadius;
		const float r = reachOf(i);
		const bool cylinder = scene.Lights[i].Flags.y > 0.5f;
		float d2 = 0.0f;
		const float centre[3] = { p.x, p.y, p.z };
		const int cell[3] = { x, y, z };
		for (int a = 0; a < (cylinder ? 2 : 3); a++)
		{
			const float c0 = lo[a] + cell[a] * cellSize, c1 = c0 + cellSize;
			const float d = centre[a] < c0 ? c0 - centre[a] : (centre[a] > c1 ? centre[a] - c1 : 0.0f);
			d2 += d * d;
		}
		return d2 < r * r;
	};

	// Two passes: count, then fill behind a running total.
	const uint32_t header = 8;
	std::vector<uint32_t> counts(cells, 0u);
	auto forEachCell = [&](size_t i, auto&& fn)
	{
		const vec4& p = scene.Lights[i].PositionRadius;
		const float r = reachOf(i);
		const bool cylinder = scene.Lights[i].Flags.y > 0.5f;
		int x0, x1, y0, y1, z0, z1;
		cellRange(p.x, r, 0, x0, x1);
		cellRange(p.y, r, 1, y0, y1);
		if (cylinder) { z0 = 0; z1 = (int)dims[2] - 1; }
		else cellRange(p.z, r, 2, z0, z1);
		for (int z = z0; z <= z1; z++)
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
					if (touches(i, x, y, z))
						fn((uint32_t)((z * (int)dims[1] + y) * (int)dims[0] + x));
	};
	for (size_t i = 0; i < lightCount; i++)
		forEachCell(i, [&](uint32_t c) { counts[c]++; });

	uint32_t total = 0, busiest = 0;
	for (uint32_t c : counts)
	{
		total += c;
		busiest = std::max(busiest, c);
	}
	if (cells != LoggedGridCells)
	{
		LoggedGridCells = cells;
		debugf(TEXT("PathTracer light grid: %d lights, %dx%dx%d cells of %.0f, %d entries, busiest cell %d, average %.1f"),
			(int)lightCount, (int)dims[0], (int)dims[1], (int)dims[2], cellSize, (int)total, (int)busiest,
			cells ? total / (float)cells : 0.0f);
	}

	LightGrid.assign(header + (size_t)cells * 2 + total, 0u);
	auto floatBits = [](float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; };
	LightGrid[0] = floatBits(lo.x);
	LightGrid[1] = floatBits(lo.y);
	LightGrid[2] = floatBits(lo.z);
	LightGrid[3] = floatBits(cellSize);
	LightGrid[4] = dims[0];
	LightGrid[5] = dims[1];
	LightGrid[6] = dims[2];
	LightGrid[7] = (uint32_t)scene.FogLights.size();

	uint32_t next = header + cells * 2;
	for (uint32_t c = 0; c < cells; c++)
	{
		LightGrid[header + c * 2] = next;
		LightGrid[header + c * 2 + 1] = 0;
		next += counts[c];
	}
	for (size_t i = 0; i < lightCount; i++)
	{
		forEachCell(i, [&](uint32_t c)
		{
			uint32_t& filled = LightGrid[header + c * 2 + 1];
			LightGrid[LightGrid[header + c * 2] + filled] = (uint32_t)i;
			filled++;
		});
	}

	if (!LightGridBuffer || LightGrid.size() > LightGridCapacity)
	{
		LightGridCapacity = std::max<size_t>(LightGrid.size() * 2, 4096);
		LightGridBuffer = BufferBuilder()
			.Size(LightGridCapacity * sizeof(uint32_t))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.MinAlignment(256)
			.DebugName("PathTracerLightGrid")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}

	void* mapped = LightGridBuffer->Map(0, LightGrid.size() * sizeof(uint32_t));
	memcpy(mapped, LightGrid.data(), LightGrid.size() * sizeof(uint32_t));
	LightGridBuffer->Unmap();

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

	// Written by the host every frame like the instance records themselves.
	InstanceDataBuffer = BufferBuilder()
		.Size(TopCapacity * sizeof(vec4))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.MinAlignment(256)
		.DebugName("PathTracerInstanceData")
		.Create(device);

	InstanceBuffer = BufferBuilder()
		.Size(TopCapacity * sizeof(VkAccelerationStructureInstanceKHR))
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.MinAlignment(256)   // instance data has its own 16 byte minimum
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

	// Everything that animates is rebuilt first, in this same command buffer,
	// so the top level structure is built against this frame's shapes.
	RecordDynamicBuilds(scene, commands);

	EnsureTopLevelCapacity(scene.Instances.size());

	const size_t count = std::min(scene.Instances.size(), TopCapacity);

	auto* mapped = (VkAccelerationStructureInstanceKHR*)InstanceBuffer->Map(0, count * sizeof(VkAccelerationStructureInstanceKHR));
	auto* instanceData = (vec4*)InstanceDataBuffer->Map(0, count * sizeof(vec4));
	for (size_t i = 0; i < count; i++)
	{
		instanceData[i] = scene.Instances[i].Ambient;
		const SceneInstance& src = scene.Instances[i];
		VkAccelerationStructureInstanceKHR& dst = mapped[i];
		dst = {};
		memcpy(&dst.transform, src.Transform, sizeof(float) * 12);
		// The custom index is how the trace shader finds this instance's
		// shading data: it is the offset of its geometry's attributes.
		dst.instanceCustomIndex = Bottom[src.GeometryIndex].AttributeBase;
		dst.mask = (HideStatic && src.GeometryIndex < scene.StaticGeometries) ? 0x00 : 0xFF;
		dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		dst.accelerationStructureReference = Bottom[src.GeometryIndex].Structure->GetDeviceAddress();
	}
	InstanceDataBuffer->Unmap();
	InstanceBuffer->Unmap();

	// Once per level: what the top level structure was actually built from.
	if (!LoggedInstances)
	{
		LoggedInstances = true;
		debugf(TEXT("PathTracer tlas: %d instances, %d bottom level structures, %d attributes"),
			(int)count, (int)Bottom.size(), (int)AllAttributes.size());
	}

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


void AccelStructure::EnsureAttributeCapacity(size_t count)
{
	if (AttributeBuffer && count <= AttributeCapacity)
		return;

	AttributeCapacity = std::max<size_t>(count * 2, 4096);
	AttributeBuffer = BufferBuilder()
		.Size(AttributeCapacity * sizeof(TriangleAttributes))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.MinAlignment(256)
		.DebugName("PathTracerAttributes")
		.Create(renderer->GetDevice());
	attributesChanged = true;
}

// A structure whose vertices are written by the host every frame.
//
// Built rather than refitted: these are a few hundred triangles each and a
// refit constrains what the geometry may do between frames, where a character
// changing weapon or skin can change its triangle count outright.
void AccelStructure::CreateDynamicBottomLevel(const SceneGeometry& geometry, BottomLevel& out)
{
	guard(AccelStructure::CreateDynamicBottomLevel);

	VulkanDevice* device = renderer->GetDevice();

	out.Dynamic = true;
	out.TriangleCount = (int)(geometry.Positions.size() / 3);

	// Headroom, so that a pose with a few more triangles does not reallocate
	// every frame.
	out.VertexCapacity = std::max<size_t>(geometry.Positions.size() * 2, 1024);

	out.Vertices = BufferBuilder()
		.Size(out.VertexCapacity * sizeof(vec3))
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU)
		.MinAlignment(256)
		.DebugName("PathTracerDynamicVertices")
		.Create(device);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geom.flags = geometry.HasMasked ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom.geometry.triangles.vertexData.deviceAddress = out.Vertices->GetDeviceAddress();
	geom.geometry.triangles.vertexStride = sizeof(vec3);
	geom.geometry.triangles.maxVertex = (uint32_t)out.VertexCapacity - 1;
	geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geom;

	const uint32_t maxTriangles = (uint32_t)(out.VertexCapacity / 3);
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxTriangles, &sizes);

	out.Buffer = BufferBuilder()
		.Size(sizes.accelerationStructureSize)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.DebugName("PathTracerDynamicBlasBuffer")
		.Create(device);

	out.Structure = AccelerationStructureBuilder()
		.Type(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)
		.Buffer(out.Buffer.get(), sizes.accelerationStructureSize)
		.DebugName("PathTracerDynamicBlas")
		.Create(device);

	// Kept rather than allocated per frame.
	out.Scratch = BufferBuilder()
		.Size(sizes.buildScratchSize)
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerDynamicScratch")
		.Create(device);

	unguard;
}

// Vertices and shading data for everything that animates, written from the host.
void AccelStructure::WriteDynamicGeometry(const LevelScene& scene)
{
	guard(AccelStructure::WriteDynamicGeometry);

	if (!haveDynamic)
		return;

	// Mapped once for the whole pass rather than once per geometry.
	TriangleAttributes* mappedAttrs = nullptr;

	const size_t count = std::min(Bottom.size(), scene.Geometries.size());
	for (size_t i = 0; i < count; i++)
	{
		const SceneGeometry& geometry = scene.Geometries[i];
		if (!geometry.Dynamic)
			continue;

		BottomLevel& level = Bottom[i];
		if (!level.Vertices || geometry.Positions.empty())
			continue;
		if (level.WrittenVersion == geometry.Version)
			continue;
		level.WrittenVersion = geometry.Version;
		level.NeedsBuild = true;

		// A pose that outgrows the buffer it was given: rare, and a rebuild of
		// the structure is the honest answer rather than truncating it.
		if (geometry.Positions.size() > level.VertexCapacity)
		{
			CreateDynamicBottomLevel(geometry, level);
			attributesChanged = true;
		}

		level.TriangleCount = (int)(geometry.Positions.size() / 3);

		void* mapped = level.Vertices->Map(0, geometry.Positions.size() * sizeof(vec3));
		memcpy(mapped, geometry.Positions.data(), geometry.Positions.size() * sizeof(vec3));
		level.Vertices->Unmap();

		// The normals moved with the pose, so this actor's slice of the shading
		// data is stale too.
		const size_t attributeCount = std::min(geometry.Attributes.size(),
			AllAttributes.size() - level.AttributeBase);
		if (attributeCount > 0 && AttributeBuffer)
		{
			memcpy(&AllAttributes[level.AttributeBase], geometry.Attributes.data(),
				attributeCount * sizeof(TriangleAttributes));

			if (!mappedAttrs)
				mappedAttrs = (TriangleAttributes*)AttributeBuffer->Map(
					0, AttributeCapacity * sizeof(TriangleAttributes));
			memcpy(mappedAttrs + level.AttributeBase, geometry.Attributes.data(),
				attributeCount * sizeof(TriangleAttributes));
		}
	}

	if (mappedAttrs)
		AttributeBuffer->Unmap();

	unguard;
}

// The per frame rebuilds, recorded into the frame's command buffer so they cost
// one submission rather than one each.
void AccelStructure::RecordDynamicBuilds(const LevelScene& scene, VulkanCommandBuffer* commands)
{
	guard(AccelStructure::RecordDynamicBuilds);

	if (!haveDynamic)
		return;

	const size_t count = std::min(Bottom.size(), scene.Geometries.size());
	bool any = false;
	for (size_t i = 0; i < count; i++)
	{
		if (!scene.Geometries[i].Dynamic)
			continue;

		BottomLevel& level = Bottom[i];
		if (!level.Structure || !level.Vertices || level.TriangleCount <= 0)
			continue;
		if (!level.NeedsBuild)
			continue;
		level.NeedsBuild = false;

		VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
		geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geom.flags = scene.Geometries[i].HasMasked ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
		geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geom.geometry.triangles.vertexData.deviceAddress = level.Vertices->GetDeviceAddress();
		geom.geometry.triangles.vertexStride = sizeof(vec3);
		geom.geometry.triangles.maxVertex = (uint32_t)(level.TriangleCount * 3) - 1;
		geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

		VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
		buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		buildInfo.geometryCount = 1;
		buildInfo.pGeometries = &geom;
		buildInfo.dstAccelerationStructure = level.Structure->accelstruct;
		buildInfo.scratchData.deviceAddress = level.Scratch->GetDeviceAddress();

		VkAccelerationStructureBuildRangeInfoKHR range = {};
		range.primitiveCount = (uint32_t)level.TriangleCount;
		const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

		commands->buildAccelerationStructures(1, &buildInfo, ranges);
		any = true;
	}

	if (any)
	{
		// The top level build reads what these just wrote.
		VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		commands->pipelineBarrier(
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			0, 1, &barrier, 0, nullptr, 0, nullptr);
	}

	unguard;
}
