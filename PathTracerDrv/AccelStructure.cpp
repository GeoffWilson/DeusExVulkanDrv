#include "TracePrecomp.h"
#include "AccelStructure.h"
#include "GpuContext.h"
#include "FrameUploads.h"

// The driver strides through the instance array by its own idea of this
// struct's size. This package compiles the engine's 4 byte packed headers
// alongside the Vulkan ones, and getting that wrong here would look exactly
// like the first instance working and none of the others existing.
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64, "instance struct is the wrong size");
static_assert(offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference) == 56, "instance struct is laid out wrong");

AccelStructure::AccelStructure(GpuContext* renderer) : renderer(renderer)
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
	LightmapBuffer.reset();
	LightmapCapacity = 0;
	AttributeBuffer.reset();
	AttributeCapacity = 0;
	haveDynamic = false;
	Bottom.clear();
	AllAttributes.clear();
	TopCapacity = 0;
	InstanceCount = 0;
	Lights = 0;
	LightCapacity = 0;
	attributesChanged = false;
	LoggedInstances = false;
}

// A shape that never changes: its vertices uploaded with the frame, and its
// structure built in the frame's own command buffer rather than waited for
// on its own, so a mesh coming into view mid-level costs no stall.
void AccelStructure::CreateStaticBottomLevel(const SceneGeometry& geometry, BottomLevel& out, FrameUploads& uploads)
{
	guard(AccelStructure::CreateStaticBottomLevel);

	VulkanDevice* device = renderer->GetDevice();

	out.TriangleCount = (int)(geometry.Positions.size() / 3);
	out.Opaque = !geometry.HasMasked;
	if (out.TriangleCount <= 0)
		return;

	// Aligned explicitly. A buffer handed to an acceleration structure build has
	// an alignment requirement on its device address, and without asking, a
	// small allocation gets suballocated wherever it fits inside a larger block.
	// A large buffer tends to land on a well aligned boundary by luck, which is
	// exactly how this hid: the static world is about a megabyte and built
	// correctly, while every prop and character is a few kilobytes and did not.
	const size_t bytes = geometry.Positions.size() * sizeof(vec3);
	out.Vertices = BufferBuilder()
		.Size(bytes)
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerVertices")
		.Create(device);
	uploads.Upload(out.Vertices.get(), 0, geometry.Positions.data(), bytes);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	// Opaque wherever nothing is masked, which is nearly everything: traversal
	// then accepts a hit outright instead of asking the shader about every
	// candidate triangle it crosses.
	geom.flags = out.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
	geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom.geometry.triangles.vertexStride = sizeof(vec3);
	geom.geometry.triangles.maxVertex = (uint32_t)geometry.Positions.size() - 1;
	geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geom;

	const uint32_t triangleCount = (uint32_t)out.TriangleCount;
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

	// Only needed for the build, and retired once it is recorded.
	out.Scratch = BufferBuilder()
		.Size(sizes.buildScratchSize)
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerBlasScratch")
		.Create(device);

	out.NeedsBuild = true;

	unguard;
}

void AccelStructure::Update(SceneData& scene, FrameUploads& uploads)
{
	SyncGeometry(scene, uploads);
	WriteLightmaps(scene, uploads);
	WriteInstances(scene, uploads);
}

void AccelStructure::WriteLightmaps(SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteLightmaps);

	if (LightmapBuffer && !scene.LightmapsChanged)
		return;
	scene.LightmapsChanged = false;
	const size_t wanted = std::max<size_t>(scene.Lightmaps.size(), 4);
	if (!LightmapBuffer || wanted > LightmapCapacity)
	{
		LightmapCapacity = wanted;
		uploads.Retire(std::move(LightmapBuffer));
		LightmapBuffer = BufferBuilder()
			.Size(LightmapCapacity * sizeof(uint32_t))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerLightmaps")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}
	if (scene.Lightmaps.empty())
	{
		const uint32_t none[4] = {};
		uploads.Upload(LightmapBuffer.get(), 0, none, sizeof(none));
	}
	else
	{
		uploads.Upload(LightmapBuffer.get(), 0, scene.Lightmaps.data(), scene.Lightmaps.size() * sizeof(uint32_t));
	}

	unguard;
}

void AccelStructure::SyncGeometry(const SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::SyncGeometry);

	// Geometry only ever gets appended within a level, so anything past what is
	// already built is new.
	if (Bottom.size() < scene.Geometries.size())
	{
		const size_t attributesBefore = AllAttributes.size();
		for (size_t i = Bottom.size(); i < scene.Geometries.size(); i++)
		{
			BottomLevel level;
			level.AttributeBase = (uint32_t)AllAttributes.size();

			const SceneGeometry& geometry = scene.Geometries[i];
			level.AttributeSlots = geometry.Attributes.size();
			AllAttributes.insert(AllAttributes.end(), geometry.Attributes.begin(), geometry.Attributes.end());

			if (geometry.Dynamic)
			{
				CreateDynamicBottomLevel(geometry, level);
				haveDynamic = true;
			}
			else
			{
				CreateStaticBottomLevel(geometry, level, uploads);
			}
			Bottom.push_back(std::move(level));
		}

		// One buffer holding every geometry's attributes, which an instance
		// indexes into through its custom index. Only the new ones are sent,
		// unless the buffer had to grow and starts empty.
		const bool grew = EnsureAttributeCapacity(AllAttributes.size(), uploads);
		const size_t first = grew ? 0 : attributesBefore;
		if (AttributeBuffer && AllAttributes.size() > first)
			uploads.Upload(AttributeBuffer.get(), first * sizeof(TriangleAttributes),
				AllAttributes.data() + first, (AllAttributes.size() - first) * sizeof(TriangleAttributes));
	}

	WriteDynamicGeometry(scene, uploads);
	WriteLights(scene, uploads);

	unguard;
}

// Rewritten every frame rather than built once. A light that moves, a flare
// that is thrown, and the player's own light augmentation all change the list,
// and a list uploaded at level load could express none of them. The fog lights
// follow the ordinary ones in the same buffer, and the grid carries how many
// there are: the push constants have no room left.
void AccelStructure::WriteLights(const SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteLights);

	Lights = (int)scene.Lights.size();
	const size_t wanted = std::max<size_t>(scene.Lights.size() + scene.FogLights.size(), 1);
	if (!LightBuffer || wanted > LightCapacity)
	{
		LightCapacity = std::max<size_t>(wanted * 2, 256);
		uploads.Retire(std::move(LightBuffer));
		LightBuffer = BufferBuilder()
			.Size(LightCapacity * sizeof(SceneLight))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerLights")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}

	auto* staged = (SceneLight*)uploads.Write(LightBuffer.get(), 0, wanted * sizeof(SceneLight));
	if (scene.Lights.empty())
	{
		// A storage buffer may not be zero sized; the shader checks the count
		// before it reads anything.
		SceneLight placeholder = {};
		staged[0] = placeholder;
	}
	else
	{
		memcpy(staged, scene.Lights.data(), scene.Lights.size() * sizeof(SceneLight));
	}
	if (!scene.FogLights.empty())
		memcpy(staged + scene.Lights.size(), scene.FogLights.data(), scene.FogLights.size() * sizeof(SceneLight));

	WriteLightGrid(scene, uploads);

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
void AccelStructure::WriteLightGrid(const SceneData& scene, FrameUploads& uploads)
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
		const bool cylinder = std::fmod(scene.Lights[i].Flags.y, 2.0f) > 0.5f;
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
		const bool cylinder = std::fmod(scene.Lights[i].Flags.y, 2.0f) > 0.5f;
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
		uploads.Retire(std::move(LightGridBuffer));
		LightGridBuffer = BufferBuilder()
			.Size(LightGridCapacity * sizeof(uint32_t))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerLightGrid")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}

	uploads.Upload(LightGridBuffer.get(), 0, LightGrid.data(), LightGrid.size() * sizeof(uint32_t));

	unguard;
}

void AccelStructure::EnsureTopLevelCapacity(size_t instanceCount, FrameUploads& uploads)
{
	if (TopLevel && instanceCount <= TopCapacity)
		return;

	VulkanDevice* device = renderer->GetDevice();

	// Grown with headroom so that a few more actors coming into view does not
	// reallocate the structure every frame. What it replaces may still be in
	// use by the frame before, so it is retired rather than destroyed.
	TopCapacity = std::max<size_t>(instanceCount * 2, 256);
	uploads.Retire(std::move(InstanceDataBuffer));
	uploads.Retire(std::move(InstanceBuffer));
	uploads.Retire(std::move(TopLevel));
	uploads.Retire(std::move(TopBuffer));
	uploads.Retire(std::move(TopScratch));

	InstanceDataBuffer = BufferBuilder()
		.Size(TopCapacity * sizeof(vec4))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerInstanceData")
		.Create(device);

	InstanceBuffer = BufferBuilder()
		.Size(TopCapacity * sizeof(VkAccelerationStructureInstanceKHR))
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
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

// Every placement, rewritten every frame, because the movers and the actors
// have all moved since the last one.
void AccelStructure::WriteInstances(const SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteInstances);

	InstanceCount = 0;
	if (scene.Instances.empty() || Bottom.empty())
		return;

	EnsureTopLevelCapacity(scene.Instances.size(), uploads);
	const size_t count = std::min(scene.Instances.size(), TopCapacity);

	auto* staged = (VkAccelerationStructureInstanceKHR*)uploads.Write(InstanceBuffer.get(), 0, count * sizeof(VkAccelerationStructureInstanceKHR));
	auto* instanceData = (vec4*)uploads.Write(InstanceDataBuffer.get(), 0, count * sizeof(vec4));
	for (size_t i = 0; i < count; i++)
	{
		instanceData[i] = scene.Instances[i].Ambient;
		const SceneInstance& src = scene.Instances[i];
		VkAccelerationStructureInstanceKHR dst = {};
		memcpy(&dst.transform, src.Transform, sizeof(float) * 12);
		// The custom index is how the trace shader finds this instance's
		// shading data: it is the offset of its geometry's attributes.
		dst.instanceCustomIndex = Bottom[src.GeometryIndex].AttributeBase;
		dst.mask = (HideStatic && src.GeometryIndex < scene.StaticGeometries) ? 0x00 : (src.Mask & 0xFF);
		dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		// A shape with no triangles has no structure, and is placed with a
		// null one, which traces as nothing.
		dst.accelerationStructureReference = Bottom[src.GeometryIndex].Structure ? Bottom[src.GeometryIndex].Structure->GetDeviceAddress() : 0;
		staged[i] = dst;
	}
	InstanceCount = count;

	// Once per level: what the top level structure was actually built from.
	if (!LoggedInstances)
	{
		LoggedInstances = true;
		debugf(TEXT("PathTracer tlas: %d instances, %d bottom level structures, %d attributes"),
			(int)count, (int)Bottom.size(), (int)AllAttributes.size());
	}

	unguard;
}

void AccelStructure::Record(VulkanCommandBuffer* commands, FrameUploads& uploads)
{
	guard(AccelStructure::Record);

	// New shapes, and those that animate and changed, first, in this same
	// command buffer, so the top level structure is built against this
	// frame's shapes.
	RecordBottomLevelBuilds(commands, uploads);

	if (!IsReady())
		return;

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
	range.primitiveCount = (uint32_t)InstanceCount;
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

// True when the buffer had to be made again, and so holds nothing yet.
bool AccelStructure::EnsureAttributeCapacity(size_t count, FrameUploads& uploads)
{
	if (AttributeBuffer && count <= AttributeCapacity)
		return false;

	AttributeCapacity = std::max<size_t>(count * 2, 4096);
	uploads.Retire(std::move(AttributeBuffer));
	AttributeBuffer = BufferBuilder()
		.Size(AttributeCapacity * sizeof(TriangleAttributes))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.MinAlignment(256)
		.DebugName("PathTracerAttributes")
		.Create(renderer->GetDevice());
	attributesChanged = true;
	return true;
}


// A structure whose vertices are rewritten whenever its pose changes.
//
// Built rather than refitted: these are a few hundred triangles each and a
// refit constrains what the geometry may do between frames, where a character
// changing weapon or skin can change its triangle count outright.
void AccelStructure::CreateDynamicBottomLevel(const SceneGeometry& geometry, BottomLevel& out)
{
	guard(AccelStructure::CreateDynamicBottomLevel);

	VulkanDevice* device = renderer->GetDevice();

	out.Dynamic = true;
	out.Opaque = !geometry.HasMasked;
	out.TriangleCount = (int)(geometry.Positions.size() / 3);

	// Headroom, so that a pose with a few more triangles does not reallocate
	// every frame.
	out.VertexCapacity = std::max<size_t>(geometry.Positions.size() * 2, 1024);

	out.Vertices = BufferBuilder()
		.Size(out.VertexCapacity * sizeof(vec3))
		.Usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
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

// What a dynamic shape had, handed to the frame to free once the frames that
// might still read it are done.
void AccelStructure::RetireBottomLevel(BottomLevel& level, FrameUploads& uploads)
{
	uploads.Retire(std::move(level.Structure));
	uploads.Retire(std::move(level.Buffer));
	uploads.Retire(std::move(level.Vertices));
	uploads.Retire(std::move(level.Scratch));
}

// Vertices and shading data for everything that animates and changed.
void AccelStructure::WriteDynamicGeometry(const SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteDynamicGeometry);

	if (!haveDynamic)
		return;

	bool attributesGrew = false;
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
			RetireBottomLevel(level, uploads);
			CreateDynamicBottomLevel(geometry, level);
		}

		level.TriangleCount = (int)(geometry.Positions.size() / 3);
		uploads.Upload(level.Vertices.get(), 0, geometry.Positions.data(), geometry.Positions.size() * sizeof(vec3));

		// More triangles than its slot holds: a new slot at the end, with room
		// to grow again. The old one is left unused until the level changes;
		// writing past it would overwrite the shape after it.
		if (geometry.Attributes.size() > level.AttributeSlots)
		{
			level.AttributeSlots = std::max<size_t>(geometry.Attributes.size() * 2, 64);
			level.AttributeBase = (uint32_t)AllAttributes.size();
			AllAttributes.resize(AllAttributes.size() + level.AttributeSlots);
			attributesGrew |= EnsureAttributeCapacity(AllAttributes.size(), uploads);
		}

		// The normals moved with the pose, so this actor's slice of the shading
		// data is stale too.
		const size_t attributeCount = geometry.Attributes.size();
		if (attributeCount > 0 && AttributeBuffer)
		{
			memcpy(&AllAttributes[level.AttributeBase], geometry.Attributes.data(),
				attributeCount * sizeof(TriangleAttributes));
			uploads.Upload(AttributeBuffer.get(), level.AttributeBase * sizeof(TriangleAttributes),
				geometry.Attributes.data(), attributeCount * sizeof(TriangleAttributes));
		}
	}

	// A buffer made bigger starts empty, and holds everything once filled.
	if (attributesGrew && AttributeBuffer)
		uploads.Upload(AttributeBuffer.get(), 0, AllAttributes.data(), AllAttributes.size() * sizeof(TriangleAttributes));

	unguard;
}

// The bottom level builds this frame needs - new shapes, and poses that
// changed - recorded into the frame's command buffer so they cost one
// submission rather than one each.
void AccelStructure::RecordBottomLevelBuilds(VulkanCommandBuffer* commands, FrameUploads& uploads)
{
	guard(AccelStructure::RecordBottomLevelBuilds);

	bool any = false;
	for (BottomLevel& level : Bottom)
	{
		if (!level.NeedsBuild)
			continue;
		level.NeedsBuild = false;
		if (!level.Structure || !level.Vertices || !level.Scratch || level.TriangleCount <= 0)
			continue;

		VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
		geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geom.flags = level.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
		geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geom.geometry.triangles.vertexData.deviceAddress = level.Vertices->GetDeviceAddress();
		geom.geometry.triangles.vertexStride = sizeof(vec3);
		geom.geometry.triangles.maxVertex = (uint32_t)(level.TriangleCount * 3) - 1;
		geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

		// The same preference the structure was sized with: built fast for a
		// shape that is rebuilt whenever it moves, traced fast for one that is
		// built once.
		VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		buildInfo.flags = level.Dynamic ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
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

		// A shape built once needs its scratch only for this build.
		if (!level.Dynamic)
			uploads.Retire(std::move(level.Scratch));
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
