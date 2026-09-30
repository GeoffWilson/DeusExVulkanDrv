#include "TracePrecomp.h"
#include "AccelStructure.h"
#include "EmitterGrid.h"
#include "GpuContext.h"
#include "FrameUploads.h"
#include <algorithm>
#include <chrono>

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
	EmitterBuffer.reset();
	EmitterCapacity = 0;
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
	LastGridInputs.clear();
	LastCylinders.clear();
	LastPowers.clear();
	LightGrid.clear();
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
	// candidate triangle it crosses. Where it does ask, it asks once for each
	// triangle, as the trace's sum of glows along a ray needs (glowAlong).
	geom.flags = out.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
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
	WriteEmitters(scene, uploads);
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

void AccelStructure::WriteEmitters(SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteEmitters);

	if (EmitterBuffer && !scene.EmittersChanged)
		return;
	scene.EmittersChanged = false;
	std::vector<uint32_t> words = scene.Emitters;
	if (words.size() < EmitterGrid::Header)
		words.assign(EmitterGrid::Header, 0u);
	// The records name a geometry and a triangle in it; the shader wants the
	// triangle's attributes, wherever that geometry's were put.
	const uint32_t count = words[0], recordBase = words[1];
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t* r = &words[recordBase + i * EmitterGrid::RecordWords];
		const uint32_t geometry = r[15], primitive = r[19];
		r[15] = geometry < Bottom.size() ? Bottom[geometry].AttributeBase + primitive : 0u;
	}
	const size_t wanted = words.size();
	if (!EmitterBuffer || wanted > EmitterCapacity)
	{
		EmitterCapacity = wanted;
		uploads.Retire(std::move(EmitterBuffer));
		EmitterBuffer = BufferBuilder()
			.Size(EmitterCapacity * sizeof(uint32_t))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerEmitters")
			.Create(renderer->GetDevice());
		attributesChanged = true;
	}
	uploads.Upload(EmitterBuffer.get(), 0, words.data(), words.size() * sizeof(uint32_t));
	if (count)
		debugf("PathTracer: %u glowing triangles sampled as lights", count);

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
	const auto lightsStart = std::chrono::steady_clock::now();

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
	LightsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lightsStart).count();

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
// And then weighing every light in the cell was. The Wan Chai canal's neon
// puts up to 78 lights in a cell, and a point weighed each of them at every
// bounce: with them all off the trace took a quarter of the time. So each
// cell's lights are ranked here by what they could give it - how bright the
// light gets, times a falloff like the engine's, taken between the cell's
// nearest point and its middle - heaviest first, and the shader weighs only
// the first few exactly and draws a few more from the rest in proportion to
// their rank (directLight). A cell with no more lights than that is weighed
// in full, as before.
//
// Laid out as one array of words: the grid's origin and cell size as floats,
// its dimensions and the fog lights' count, then a start and count per cell,
// then the entries the starts point into, two words each: a light's index,
// and the rank of that light and every one after it in its cell added up (a
// float), which a draw from any tail of the list searches. The heaviest
// ExactLights come first, in order; the rest in no order, since they are
// only ever drawn from.
void AccelStructure::WriteLightGrid(const SceneData& scene, FrameUploads& uploads)
{
	guard(AccelStructure::WriteLightGrid);

	const size_t lightCount = scene.Lights.size();
	auto reachOf = [&](size_t i) { return std::abs(scene.Lights[i].PositionRadius.w); };

	// What the grid is made from: where each light is, how far it reaches,
	// whether it is a cylinder and how bright it gets, in the list's order.
	// Most of a level's lights never move, so most frames it is the same as
	// the last, and the buffer already holds it.
	GridInputs.resize(lightCount);
	Cylinders.resize(lightCount);
	Powers.resize(lightCount);
	for (size_t i = 0; i < lightCount; i++)
	{
		const SceneLight& light = scene.Lights[i];
		GridInputs[i] = light.PositionRadius;
		// Worked out once a light: the test below runs for every cell a
		// light's box covers, twice, and an fmod there - a slow library
		// call - cost 6.5 ms a frame in the Hong Kong market.
		Cylinders[i] = ((int)light.Flags.y & 1) != 0;
		// As the shader weighs it with the engine's lighting: its colour as
		// displayed, at its brightest, twice over when it is baked into the
		// lightmaps. A light that says nothing of its peak - the harness's -
		// is taken at what it is now.
		auto shown = [](float c) { return std::pow(std::max(c, 0.0f), 1.0f / 2.2f); };
		const float colour = 0.2126f * shown(light.ColorBrightness.x) + 0.7152f * shown(light.ColorBrightness.y) + 0.0722f * shown(light.ColorBrightness.z);
		Powers[i] = std::max(light.Peak.x, light.ColorBrightness.w) * colour * (light.Flags.y > 1.5f ? 2.0f : 1.0f);
	}
	// How many fog lights follow is in the header, but no cell depends on
	// it: a fog light flickering in and out changes that word alone.
	const uint32_t fogCount = (uint32_t)scene.FogLights.size();
	bool same = LightGridBuffer && !LightGrid.empty() && GridInputs.size() == LastGridInputs.size() && Cylinders == LastCylinders &&
		memcmp(GridInputs.data(), LastGridInputs.data(), GridInputs.size() * sizeof(vec4)) == 0;
	// A rank out of date only guides the draws less well - what they add up
	// to comes out the same - so a light fading, as an explosion's does, is
	// left as it was ranked. One grown much brighter than that would be
	// drawn too seldom for what it gives, and is ranked again.
	for (size_t i = 0; same && i < lightCount; i++)
		same = Powers[i] <= LastPowers[i] * 2.0f;
	if (same)
	{
		if (LightGrid[7] != fogCount)
		{
			LightGrid[7] = fogCount;
			uploads.Upload(LightGridBuffer.get(), 7 * sizeof(uint32_t), &LightGrid[7], sizeof(uint32_t));
		}
		return;
	}
	LastGridInputs.swap(GridInputs);
	LastCylinders = Cylinders;
	LastPowers = Powers;

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

	// Every cell each light's reach touches, and its rank there, in one pass
	// over the cells of the box around its reach. Touching is a sphere
	// against the cell's box - for a cylinder light, whose reach is measured
	// across the floor only, a circle against its footprint. The rank is how
	// bright it gets by a falloff out to its reach, taken half at the cell's
	// nearest point and half at its middle, which may be out of reach: (1 -
	// x^2)^2 of the way x out, within a few hundredths of the engine's
	// 1 - 3x^2 + 2x^3 and needing no square root. Never quite nothing, for a
	// light listed can reach some of the cell, and a draw has to be able to
	// find it.
	GridEntries.clear();
	for (size_t i = 0; i < lightCount; i++)
	{
		const vec4& p = scene.Lights[i].PositionRadius;
		const float r = reachOf(i), r2 = r * r, inverse2 = 1.0f / std::max(r2, 1.0f);
		const bool cylinder = Cylinders[i] != 0;
		int first[3], last[3];
		cellRange(p.x, r, 0, first[0], last[0]);
		cellRange(p.y, r, 1, first[1], last[1]);
		if (cylinder) { first[2] = 0; last[2] = (int)dims[2] - 1; }
		else cellRange(p.z, r, 2, first[2], last[2]);
		auto along = [&](int axis, int cell, float centre, float& nearest2, float& middle2)
		{
			const float c0 = lo[axis] + cell * cellSize, c1 = c0 + cellSize;
			const float d = centre < c0 ? c0 - centre : (centre > c1 ? centre - c1 : 0.0f);
			const float m = centre - (c0 + c1) * 0.5f;
			nearest2 = d * d;
			middle2 = m * m;
		};
		auto falloff = [](float x2) { x2 = std::min(x2, 1.0f); return (1.0f - x2) * (1.0f - x2); };
		for (int z = first[2]; z <= last[2]; z++)
		{
			float zn = 0.0f, zm = 0.0f;
			if (!cylinder)
				along(2, z, p.z, zn, zm);
			for (int y = first[1]; y <= last[1]; y++)
			{
				float yn, ym;
				along(1, y, p.y, yn, ym);
				if (zn + yn >= r2)
					continue;
				for (int x = first[0]; x <= last[0]; x++)
				{
					float xn, xm;
					along(0, x, p.x, xn, xm);
					const float nearest2 = xn + yn + zn;
					if (nearest2 >= r2)
						continue;
					const float middle2 = xm + ym + zm;
					const float rank = Powers[i] * 0.5f * (falloff(nearest2 * inverse2) + falloff(middle2 * inverse2));
					GridEntries.push_back({ (uint32_t)((z * (int)dims[1] + y) * (int)dims[0] + x), std::max(rank, 1.0e-30f), (uint32_t)i });
				}
			}
		}
	}
	const uint32_t total = (uint32_t)GridEntries.size();

	// Counted by cell, then placed by cell behind a running total.
	GridCounts.assign(cells + 1, 0u);
	for (const GridEntry& e : GridEntries)
		GridCounts[e.Cell + 1]++;
	uint32_t busiest = 0;
	for (uint32_t c = 0; c < cells; c++)
	{
		busiest = std::max(busiest, GridCounts[c + 1]);
		GridCounts[c + 1] += GridCounts[c];
	}
	if (cells != LoggedGridCells)
	{
		LoggedGridCells = cells;
		debugf(TEXT("PathTracer light grid: %d lights, %dx%dx%d cells of %.0f, %d entries, busiest cell %d, average %.1f"),
			(int)lightCount, (int)dims[0], (int)dims[1], (int)dims[2], cellSize, (int)total, (int)busiest,
			cells ? total / (float)cells : 0.0f);
	}
	GridRanked.resize(total);
	GridFill.assign(GridCounts.begin(), GridCounts.end() - 1);
	for (const GridEntry& e : GridEntries)
		GridRanked[GridFill[e.Cell]++] = { e.Rank, e.Light };

	const uint32_t header = 8;
	LightGrid.assign(header + (size_t)cells * 2 + (size_t)total * 2, 0u);
	auto floatBits = [](float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; };
	LightGrid[0] = floatBits(lo.x);
	LightGrid[1] = floatBits(lo.y);
	LightGrid[2] = floatBits(lo.z);
	LightGrid[3] = floatBits(cellSize);
	LightGrid[4] = dims[0];
	LightGrid[5] = dims[1];
	LightGrid[6] = dims[2];
	LightGrid[7] = fogCount;

	// Each cell's heaviest first, with the sums behind them.
	const uint32_t entries = header + cells * 2;
	for (uint32_t c = 0; c < cells; c++)
	{
		const uint32_t first = GridCounts[c], count = GridCounts[c + 1] - first;
		LightGrid[header + c * 2] = entries + first * 2;
		LightGrid[header + c * 2 + 1] = count;
		// Only the head has to be in order - the lights the shader weighs
		// exactly, heaviest first - and the rest only summed: sorting whole
		// cells was most of the build.
		auto begin = GridRanked.begin() + first, end = begin + count;
		std::partial_sort(begin, begin + std::min(count, ExactLights), end, [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b)
		{
			return a.first > b.first || (a.first == b.first && a.second < b.second);
		});
		float after = 0.0f;
		for (uint32_t k = count; k-- > 0;)
		{
			after += GridRanked[first + k].first;
			LightGrid[entries + (first + k) * 2] = GridRanked[first + k].second;
			LightGrid[entries + (first + k) * 2 + 1] = floatBits(after);
		}
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
	geom.flags = geometry.HasMasked ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
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
		geom.flags = level.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
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
