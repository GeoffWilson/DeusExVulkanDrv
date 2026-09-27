#pragma once

#include "vec.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The level's glowing surfaces as lights (SceneData's Emitters): each glowing
// triangle, and a grid over the level listing the ones most worth sampling in
// each cell, which the trace shader draws one from at every surface it shades
// (glowLight in Shaders.cpp). Built once a level, by the render device from
// the level's own surfaces and by the harness from its made up ones.
//
// Laid out as words:
//   0        how many emitters
//   1        the word their records start at
//   2..4     the grid's origin, floats
//   5        its cell size, a float
//   6..8     its dimensions
//   9..11    unused
//   12..     a start and a count for each cell, into the lists after them
//   then the lists of emitter numbers, then the records, 20 words each,
//   floats but for two:
//     its middle, and its power, negative where it glows from both faces;
//     the face it glows from, and its area;
//     its first corner, and how far its furthest corner is from its middle;
//     its first edge, and the geometry it is in, which the helper turns
//     into the triangle's attribute index (TriangleAttributes);
//     its second edge, and the triangle's number in its geometry.
// The edges run so that their cross product is the face it glows from. The
// first two are all the shader reads to choose between emitters.
struct EmitterSource
{
	vec3 V0, E1, E2;
	// How brightly it glows over its whole area: its texture's average
	// brightness times its area. Only for choosing between emitters.
	float Power = 0.0f;
	bool TwoSided = false;
	uint32_t Geometry = 0;
	uint32_t Primitive = 0;
};

namespace EmitterGrid
{
	static const uint32_t Header = 12;
	static const uint32_t RecordWords = 20;
	// The most emitters a cell lists, the strongest there. The rest are
	// found only by the bounces that happen to reach them, as every glowing
	// surface was before, which the shader's weighting between the two
	// allows for.
	static const size_t PerCell = 12;
	// How far an emitter's list reaches: out to where it would light a
	// surface facing it at this much, allowing for GlowLighting at ten times
	// the default, and never further than MaxReach.
	constexpr float Faint = 0.0005f;
	constexpr float MaxReach = 1024.0f;

	inline uint32_t FloatBits(float f)
	{
		uint32_t u;
		memcpy(&u, &f, sizeof(u));
		return u;
	}

	inline void Build(const std::vector<EmitterSource>& emitters, std::vector<uint32_t>& words)
	{
		words.assign(Header, 0u);
		const size_t count = emitters.size();
		if (count == 0)
			return;

		std::vector<vec3> centres(count);
		std::vector<float> reaches(count);
		vec3 lo(0.0f), hi(0.0f);
		for (size_t i = 0; i < count; i++)
		{
			const EmitterSource& e = emitters[i];
			centres[i] = e.V0 + (e.E1 + e.E2) * (1.0f / 3.0f);
			reaches[i] = std::min(MaxReach, std::max(32.0f, std::sqrt(10.0f * e.Power / (3.14159265f * Faint))));
			const float r = reaches[i];
			const vec3 a = centres[i] - vec3(r, r, r), b = centres[i] + vec3(r, r, r);
			if (i == 0) { lo = a; hi = b; }
			else
			{
				lo = vec3(std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z));
				hi = vec3(std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z));
			}
		}

		// As the light grid sizes its cells: a few tens of thousands at most,
		// none smaller than a room.
		const vec3 extent = hi - lo;
		const float volume = std::max(extent.x, 1.0f) * std::max(extent.y, 1.0f) * std::max(extent.z, 1.0f);
		const float cellSize = std::max(256.0f, std::cbrt(volume / 32768.0f));
		uint32_t dims[3];
		for (int a = 0; a < 3; a++)
			dims[a] = (uint32_t)std::min(64.0f, std::max(1.0f, std::ceil(extent[a] / cellSize)));
		const uint32_t cells = dims[0] * dims[1] * dims[2];

		// Every cell each emitter's reach touches, scored by its power over
		// the square of its distance from the cell's middle - held back
		// within half a cell - then each cell's strongest kept.
		struct Entry { uint32_t Cell; float Score; uint32_t Emitter; };
		std::vector<Entry> entries;
		for (size_t i = 0; i < count; i++)
		{
			const vec3& c = centres[i];
			const float r = reaches[i];
			int first[3], last[3];
			for (int a = 0; a < 3; a++)
			{
				first[a] = std::max(0, (int)std::floor((c[a] - r - lo[a]) / cellSize));
				last[a] = std::min((int)dims[a] - 1, (int)std::floor((c[a] + r - lo[a]) / cellSize));
			}
			for (int z = first[2]; z <= last[2]; z++)
				for (int y = first[1]; y <= last[1]; y++)
					for (int x = first[0]; x <= last[0]; x++)
					{
						const int cell[3] = { x, y, z };
						float outside = 0.0f, fromMiddle = 0.0f;
						for (int a = 0; a < 3; a++)
						{
							const float c0 = lo[a] + cell[a] * cellSize, c1 = c0 + cellSize;
							const float d = c[a] < c0 ? c0 - c[a] : (c[a] > c1 ? c[a] - c1 : 0.0f);
							outside += d * d;
							const float m = c[a] - (c0 + c1) * 0.5f;
							fromMiddle += m * m;
						}
						if (outside >= r * r)
							continue;
						const float half = 0.5f * cellSize;
						entries.push_back({ (uint32_t)((z * (int)dims[1] + y) * (int)dims[0] + x),
							emitters[i].Power / std::max(fromMiddle, half * half), (uint32_t)i });
					}
		}
		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
		{
			return a.Cell != b.Cell ? a.Cell < b.Cell : a.Score > b.Score;
		});

		std::vector<uint32_t> starts(cells, 0u), counts(cells, 0u), lists;
		for (size_t k = 0; k < entries.size();)
		{
			const uint32_t cell = entries[k].Cell;
			starts[cell] = (uint32_t)lists.size();
			size_t kept = 0;
			for (; k < entries.size() && entries[k].Cell == cell; k++)
				if (kept < PerCell)
				{
					lists.push_back(entries[k].Emitter);
					kept++;
				}
			counts[cell] = (uint32_t)kept;
		}

		const uint32_t listBase = Header + cells * 2;
		const uint32_t recordBase = listBase + (uint32_t)lists.size();
		words.assign(recordBase + count * RecordWords, 0u);
		words[0] = (uint32_t)count;
		words[1] = recordBase;
		words[2] = FloatBits(lo.x);
		words[3] = FloatBits(lo.y);
		words[4] = FloatBits(lo.z);
		words[5] = FloatBits(cellSize);
		words[6] = dims[0];
		words[7] = dims[1];
		words[8] = dims[2];
		for (uint32_t c = 0; c < cells; c++)
		{
			words[Header + c * 2] = listBase + starts[c];
			words[Header + c * 2 + 1] = counts[c];
		}
		if (!lists.empty())
			memcpy(&words[listBase], lists.data(), lists.size() * sizeof(uint32_t));
		for (size_t i = 0; i < count; i++)
		{
			const EmitterSource& e = emitters[i];
			const vec3& c = centres[i];
			const vec3 cr = cross(e.E1, e.E2);
			const float twiceArea = std::sqrt(dot(cr, cr));
			const vec3 n = twiceArea > 0.0f ? cr * (1.0f / twiceArea) : vec3(0.0f, 0.0f, 1.0f);
			const vec3 corners[3] = { e.V0, e.V0 + e.E1, e.V0 + e.E2 };
			float radius = 0.0f;
			for (const vec3& p : corners)
				radius = std::max(radius, std::sqrt(dot(p - c, p - c)));
			uint32_t* r = &words[recordBase + i * RecordWords];
			const float fields[20] = {
				c.x, c.y, c.z, e.TwoSided ? -e.Power : e.Power,
				n.x, n.y, n.z, 0.5f * twiceArea,
				e.V0.x, e.V0.y, e.V0.z, radius,
				e.E1.x, e.E1.y, e.E1.z, 0.0f,
				e.E2.x, e.E2.y, e.E2.z, 0.0f };
			for (int k = 0; k < 20; k++)
				r[k] = FloatBits(fields[k]);
			r[15] = e.Geometry;
			r[19] = e.Primitive;
		}
	}
}
