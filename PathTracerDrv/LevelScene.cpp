#include "Precomp.h"
#include "LevelScene.h"
#include "Materials.h"

namespace
{
	inline vec3 SrgbToLinear(FLOAT r, FLOAT g, FLOAT b)
	{
		return vec3(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f));
	}

	inline vec3 ToVec3(const FVector& v)
	{
		return vec3(v.X, v.Y, v.Z);
	}

	// A zone's ambient light, which is what stops an unlit corner of a Deus Ex
	// room being pure black, as Render.dll works it out: FGetHSV of the zone's
	// hue, saturation and brightness together. That puts the brightness
	// through the engine's curve, which is close to a square root - 20 out of
	// 255 comes out at 0.23, not 0.08. In displayed terms, as everything in
	// the engine's lighting is. Few zones set one; most of the game has none.
	vec3 ZoneAmbient(AZoneInfo* zone)
	{
		if (!zone || zone->AmbientBrightness == 0)
			return vec3(0.0f, 0.0f, 0.0f);

		const FPlane c = FGetHSV(zone->AmbientHue, zone->AmbientSaturation, zone->AmbientBrightness);
		return vec3(c.X, c.Y, c.Z);
	}

	// The ambient on a surface the engine lights with a lightmap - the level's
	// own and its movers'. Every texel of a lightmap starts at 64 times it, on
	// a scale where 127 is full brightness (the lights' sum saturates there,
	// and the lightmap is drawn doubled), so on screen it is half the zone's
	// ambient. Made linear, as the texture is: the engine's multiply happens
	// on the displayed colours. Summing it as a linear light, at the zone's
	// brightness over 255, drew ambient-lit walls over twice as bright as the
	// engine does.
	vec3 LightmapAmbient(AZoneInfo* zone)
	{
		const vec3 a = ZoneAmbient(zone) * (2.0f * 64.0f / 255.0f);
		return SrgbToLinear(a.x, a.y, a.z);
	}

	// A texture's average colour, used as a stand-in for sampling it.
	//
	// MipZero is the engine's own average and is right for level textures,
	// which have it computed when the map is built. Mesh skins often do not -
	// it is left black, and a character shaded with it is invisible against a
	// dark room, which is exactly how this looked. Fall back to averaging the
	// palette, and to a plain grey if there is not one.
	// Each texture's average, worked out once per level. It was being redone
	// for every triangle of every animated character on every frame, and for a
	// skin that means a pass over the whole palette each time - milliseconds a
	// frame in a crowded level, for a colour that is only a fallback now that
	// textures are sampled. Cleared with the scene, since a texture object can
	// be freed and its address reused by the next level.
	struct CachedAverage
	{
		bool Known = false;
		vec3 Colour;
	};
	std::unordered_map<UTexture*, CachedAverage> AverageCache;

	vec3 AverageColourUncached(UTexture* texture, vec3 fallback, bool& known);

	vec3 AverageColour(UTexture* texture, vec3 fallback)
	{
		if (!texture)
			return fallback;
		auto it = AverageCache.find(texture);
		if (it == AverageCache.end())
		{
			CachedAverage entry;
			entry.Colour = AverageColourUncached(texture, fallback, entry.Known);
			it = AverageCache.emplace(texture, entry).first;
		}
		return it->second.Known ? it->second.Colour : fallback;
	}

	vec3 AverageColourUncached(UTexture* texture, vec3 fallback, bool& known)
	{
		known = true;

		// MipZero is the engine's cached average of the whole image, including
		// the texels that masked art uses as its transparency key - which in
		// these packages is magenta. Trusting it made the fallback colour of
		// every grate, fence and banner in the game bright pink. For masked art
		// the palette average below is the honest answer.
		const bool masked = (texture->PolyFlags & PF_Masked) != 0;
		const FColor& c = texture->MipZero;
		if (!masked && (c.R > 4 || c.G > 4 || c.B > 4))
			return SrgbToLinear(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f);

		if (texture->Palette && texture->Palette->Colors.Num() > 0)
		{
			// Index zero is the transparent one in masked art, so it says
			// nothing about what the texture looks like.
			FLOAT r = 0.0f, g = 0.0f, b = 0.0f;
			INT count = 0;
			for (INT i = 1; i < texture->Palette->Colors.Num(); i++)
			{
				const FColor& p = texture->Palette->Colors(i);
				r += p.R; g += p.G; b += p.B;
				count++;
			}
			if (count > 0)
				return SrgbToLinear(r / (255.0f * count), g / (255.0f * count), b / (255.0f * count));
		}

		known = false;
		return fallback;
	}

	// The engine's rotators are 16 bit units; FCoords does the work, this just
	// flattens the result into the 3x4 row major form Vulkan wants.
	// prePivot is the actor's own draw offset, applied in the object's space
	// before the rotation - the order ABrush::ToWorld spells out:
	//   UnitCoords / -PrePivot / MainScale / Rotation / PostScale / Location
	// Folding it into the translation keeps the cached geometry untouched, since
	// the offset belongs to the actor rather than to the shape.
	void MakeTransform(const FVector& location, const FRotator& rotation, const FVector& scale, const FVector& prePivot, float out[12])
	{
		FCoords coords = GMath.UnitCoords / rotation;

		// GMath.UnitCoords / Rotation gives the object's own axes as world space
		// directions - XAxis is which way the actor is facing. A matrix mapping
		// object to world sends (1,0,0) to XAxis, so those axes are its COLUMNS,
		// not its rows. Writing them as rows transposes the rotation, which for
		// anything not axis aligned is a different orientation entirely.
		const FVector axes[3] = { coords.XAxis, coords.YAxis, coords.ZAxis };
		const float s[3] = { scale.X, scale.Y, scale.Z };

		for (int col = 0; col < 3; col++)
		{
			out[0 * 4 + col] = axes[col].X * s[col];
			out[1 * 4 + col] = axes[col].Y * s[col];
			out[2 * 4 + col] = axes[col].Z * s[col];
		}
		// location + R * (scale * prePivot). The engine's chain reads
		// "/ -PrePivot", and its / operator already subtracts, so the two
		// negatives cancel and the offset is added.
		const FVector offset =
			axes[0] * (prePivot.X * s[0]) +
			axes[1] * (prePivot.Y * s[1]) +
			axes[2] * (prePivot.Z * s[2]);

		out[0 * 4 + 3] = location.X + offset.X;
		out[1 * 4 + 3] = location.Y + offset.Y;
		out[2 * 4 + 3] = location.Z + offset.Z;
	}

	void MakeIdentity(float out[12])
	{
		for (int i = 0; i < 12; i++)
			out[i] = 0.0f;
		out[0] = out[5] = out[10] = 1.0f;
	}
}

void LevelScene::Clear()
{
	Geometries.clear();
	StaticGeometries = 0;
	Lights.clear();
	FogLights.clear();
	Instances.clear();
	BrushGeometry.clear();
	MeshGeometry.clear();
	MeshTriangleCache.clear();
	AverageCache.clear();
	ActorPoseKeys.clear();
	SpriteGeometry.clear();
	FixedFrames.clear();
	DecalGeometry = -1;
	DecalSignature = 0;
	ActorGeometry.clear();
	PreviousPoses.clear();
	HaveViewModelTransform = false;
	ViewModelIndex = -1;
	ViewModelItem = nullptr;
	CurrentPoses.clear();
	Textures.clear();
	TextureMasked.clear();
	TextureMaterials.clear();
	TextureIndex.clear();
	GeometryAdded = false;
	MeshesLogged = 0;
	SummaryLogged = false;
	SourceLevel = nullptr;
	SourceNodeCount = 0;
	MirroredSurfaces = 0;
	BakedLightIds.clear();
	LightmapRecords.clear();
	LightmapRecordWords.clear();
	LightmapLightWords.clear();
	LightmapMaskBytes.clear();
	LightmappedModel = nullptr;
	Lightmaps.clear();
	Emitters.clear();
	EmitterSources.clear();
	EmitterTriangles.clear();
}

bool LevelScene::BuildStatic(ULevel* level)
{
	guard(LevelScene::BuildStatic);

	Clear();

	if (!level || !level->Model)
		return false;

	// Split in two. A geometry that is not opaque sends every triangle a ray
	// crosses back to the shader to be judged, rather than letting the
	// hardware accept it, and one masked grate or translucent sheet anywhere
	// in the level made the whole level so. Adding the sea to Liberty Island
	// is what did it there: looking back along the dock, every long ray paid
	// for every triangle it passed. Now only the surfaces that need judging
	// are in the slow half.
	// The lights baked into the lightmaps, numbered, before the surfaces
	// that list them.
	for (INT i = 0; i < level->Model->Lights.Num(); i++)
	{
		AActor* light = level->Model->Lights(i);
		if (light && !BakedLightIds.count(light))
			BakedLightIds[light] = (uint32_t)BakedLightIds.size() + 1;
	}

	SceneGeometry all;
	LightmappedModel = level->Model;
	AddBspSurfaces(level->Model, all, true);
	FinishLightmaps();

	Geometries.emplace_back();
	Geometries.emplace_back();
	SceneGeometry& opaque = Geometries[0];
	SceneGeometry& judged = Geometries[1];
	opaque.HasMasked = false;
	judged.HasMasked = true;
	// Where each triangle ends up, for the glowing ones' records.
	std::vector<uint32_t> placedIn(all.Attributes.size()), placedAt(all.Attributes.size());
	for (size_t t = 0; t < all.Attributes.size(); t++)
	{
		// Masked, translucent and modulated need the shader; plain, mirrored
		// and sky windows do not.
		const float kind = all.Attributes[t].UV2Tex.w;
		const bool needsShader = kind == 1.0f || kind == 2.0f || kind == 4.0f;
		SceneGeometry& to = needsShader ? judged : opaque;
		placedIn[t] = needsShader ? 1u : 0u;
		placedAt[t] = (uint32_t)to.Attributes.size();
		to.Positions.insert(to.Positions.end(), all.Positions.begin() + t * 3, all.Positions.begin() + t * 3 + 3);
		to.Attributes.push_back(all.Attributes[t]);
	}
	for (size_t k = 0; k < EmitterSources.size(); k++)
	{
		EmitterSources[k].Geometry = placedIn[EmitterTriangles[k]];
		EmitterSources[k].Primitive = placedAt[EmitterTriangles[k]];
	}
	EmitterGrid::Build(EmitterSources, Emitters);
	EmittersChanged = true;
	if (!EmitterSources.empty())
		debugf(TEXT("PathTracer: %d glowing triangles sampled as lights"), (int)EmitterSources.size());
	// A geometry with nothing in it cannot have an acceleration structure, so
	// a level with no such surfaces carries one placeholder triangle of no
	// size, which nothing can hit.
	if (judged.Positions.empty())
	{
		judged.Positions.assign(3, vec3(0.0f, 0.0f, 0.0f));
		TriangleAttributes none = {};
		none.UV2Tex = vec4(0.0f, 0.0f, -1.0f, 0.0f);
		judged.Attributes.push_back(none);
	}
	StaticGeometries = 2;
	// Lights are gathered per frame in CollectDynamic rather than here. The
	// light augmentation turns the player into a light, a thrown flare is a
	// light that moves, and a lamp that is shot out stops being one - none of
	// which a list built once at level load can express.

	// Where to look for the engine's rarer effects, which the trace has.
	int wavySurfaces = 0, waveLights = 0;
	for (INT i = 0; i < level->Model->Surfs.Num(); i++)
		if (level->Model->Surfs(i).PolyFlags & PF_SmallWavy)
			wavySurfaces++;
	for (INT i = 0; i < level->Actors.Num(); i++)
		if (level->Actors(i) && (level->Actors(i)->LightEffect == LE_SlowWave || level->Actors(i)->LightEffect == LE_FastWave))
			waveLights++;
	if (wavySurfaces || waveLights)
		debugf(TEXT("PathTracer: %d small wavy surfaces, %d lights with slow or fast waves"), wavySurfaces, waveLights);

	SourceLevel = level;
	SourceNodeCount = level->Model->Nodes.Num();
	GeometryAdded = true;

	return !Geometries[0].Positions.empty();

	unguard;
}

// A baked light's shadow mask on a lightmap as the engine filters it before
// lighting the surface (Render.dll's FUN_10b02650): each texel the sum of its
// 3x3 neighbourhood's bits weighted 24 40 24 / 40 64 40 / 24 40 24 out of
// 320, so 255 where the light is clear all round - twice the 127 the engine
// fills a light with no mask with, which is why a baked light counts double.
// The bits are stored a row at a time, whole bytes a row, the first texel in
// each byte's lowest bit; past the first and last rows and the row's first
// texel, the nearest stands in, and past its last byte the last bit of it.
static void FilteredMask(UModel* model, const FLightMapIndex& index, INT k, uint8_t* out)
{
	const INT width = index.UClamp, height = index.VClamp;
	const INT rowBytes = (width + 7) >> 3;
	const INT start = index.DataOffset + k * rowBytes * height;
	auto bit = [&](INT x, INT y) -> int
	{
		x = Clamp(x, 0, rowBytes * 8 - 1);
		y = Clamp(y, 0, height - 1);
		const INT b = start + y * rowBytes + (x >> 3);
		return (b >= 0 && b < model->LightBits.Num()) ? (model->LightBits(b) >> (x & 7)) & 1 : 0;
	};
	static const int weights[3][3] = { { 24, 40, 24 }, { 40, 64, 40 }, { 24, 40, 24 } };
	for (INT y = 0; y < height; y++)
		for (INT x = 0; x < width; x++)
		{
			int sum = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					sum += weights[dy + 1][dx + 1] * bit(x + dx, y + dy);
			out[x + y * width] = (uint8_t)(sum * 255 / 320);
		}
}

// Where a point falls on a surface's lightmap, in texels, the engine's
// samples at whole numbers: as the devices place it, from the surface's
// texture axes and the lightmap's own pan and scale.
static void LightmapAxes(UModel* model, const FBspSurf& surf, const FLightMapIndex& index, float u[4], float v[4])
{
	const FVector base = model->Points(surf.pBase);
	const FVector tu = model->Vectors(surf.vTextureU), tv = model->Vectors(surf.vTextureV);
	const float us = index.UScale != 0.0f ? 1.0f / index.UScale : 0.0f;
	const float vs = index.VScale != 0.0f ? 1.0f / index.VScale : 0.0f;
	u[0] = tu.X * us; u[1] = tu.Y * us; u[2] = tu.Z * us; u[3] = (-(base | tu) - index.Pan.X) * us;
	v[0] = tv.X * vs; v[1] = tv.Y * vs; v[2] = tv.Z * vs; v[3] = (-(base | tv) - index.Pan.Y) * vs;
}

static bool LightmapUsable(UModel* model, INT iSurf, FLightMapIndex*& index)
{
	index = (iSurf >= 0 && iSurf < model->Surfs.Num()) ? model->GetLightMapIndex(iSurf) : nullptr;
	if (!index || index->UClamp <= 0 || index->VClamp <= 0 || index->UClamp > 65535 || index->VClamp > 65535 || index->iLightActors < 0)
		return false;
	const FBspSurf& surf = model->Surfs(iSurf);
	return surf.pBase >= 0 && surf.pBase < model->Points.Num() &&
		surf.vTextureU >= 0 && surf.vTextureU < model->Vectors.Num() && surf.vTextureV >= 0 && surf.vTextureV < model->Vectors.Num();
}

// The record for a level surface's lightmap, made the first time one of its
// nodes asks: its mapping, the numbers of the lights baked into it and each
// one's filtered mask. Its number plus one, or 0 for a surface without one.
uint32_t LevelScene::AddLightmap(UModel* model, INT iSurf)
{
	auto found = LightmapRecords.find(iSurf);
	if (found != LightmapRecords.end())
		return found->second;
	uint32_t record = 0;
	FLightMapIndex* index = nullptr;
	if (LightmapUsable(model, iSurf, index))
	{
		float u[4], v[4];
		LightmapAxes(model, model->Surfs(iSurf), *index, u, v);
		const uint32_t lightStart = (uint32_t)LightmapLightWords.size();
		const size_t maskStart = LightmapMaskBytes.size();
		const size_t texels = (size_t)index->UClamp * index->VClamp;
		for (INT k = 0; index->iLightActors + k < model->Lights.Num(); k++)
		{
			AActor* light = model->Lights(index->iLightActors + k);
			if (!light)
				break;
			auto id = BakedLightIds.find(light);
			LightmapLightWords.push_back(id != BakedLightIds.end() ? id->second : 0u);
			LightmapMaskBytes.resize(LightmapMaskBytes.size() + texels);
			FilteredMask(model, *index, k, LightmapMaskBytes.data() + LightmapMaskBytes.size() - texels);
		}
		// Each surface's masks start on a whole word.
		LightmapMaskBytes.resize((LightmapMaskBytes.size() + 3) & ~(size_t)3);
		auto floatBits = [](float f) { uint32_t w; memcpy(&w, &f, sizeof(w)); return w; };
		for (int i = 0; i < 4; i++)
			LightmapRecordWords.push_back(floatBits(u[i]));
		for (int i = 0; i < 4; i++)
			LightmapRecordWords.push_back(floatBits(v[i]));
		LightmapRecordWords.push_back((uint32_t)maskStart);
		LightmapRecordWords.push_back((uint32_t)index->UClamp | ((uint32_t)index->VClamp << 16));
		LightmapRecordWords.push_back(lightStart);
		LightmapRecordWords.push_back((uint32_t)LightmapLightWords.size() - lightStart);
		record = (uint32_t)(LightmapRecordWords.size() / 12);
	}
	LightmapRecords[iSurf] = record;
	return record;
}

// The records, lists and masks put together as Lightmaps lays them out, the
// records' starts moved to where their lists and masks ended up.
void LevelScene::FinishLightmaps()
{
	const uint32_t count = (uint32_t)(LightmapRecordWords.size() / 12);
	const uint32_t listBase = 1 + count * 12;
	const uint32_t maskBase = (listBase + (uint32_t)LightmapLightWords.size()) * 4;
	Lightmaps.assign(1 + LightmapRecordWords.size() + LightmapLightWords.size() + LightmapMaskBytes.size() / 4, 0u);
	Lightmaps[0] = count;
	for (uint32_t r = 0; r < count; r++)
	{
		uint32_t* out = &Lightmaps[1 + r * 12];
		memcpy(out, &LightmapRecordWords[r * 12], 12 * sizeof(uint32_t));
		out[8] += maskBase;
		out[10] += listBase;
	}
	if (!LightmapLightWords.empty())
		memcpy(&Lightmaps[listBase], LightmapLightWords.data(), LightmapLightWords.size() * sizeof(uint32_t));
	if (!LightmapMaskBytes.empty())
		memcpy((uint8_t*)Lightmaps.data() + maskBase, LightmapMaskBytes.data(), LightmapMaskBytes.size());
	debugf(TEXT("PathTracer lightmaps: %d surfaces, %d masks, %.1f MB of the engine's shadows"),
		(int)count, (int)LightmapLightWords.size(), Lightmaps.size() * 4 / (1024.0f * 1024.0f));
	LightmapRecordWords.clear();
	LightmapLightWords.clear();
	LightmapMaskBytes.clear();
}

float LevelScene::BakedMaskAt(UModel* model, INT iSurf, AActor* light, const FVector& point)
{
	FLightMapIndex* index = nullptr;
	if (!model || !LightmapUsable(model, iSurf, index))
		return -1.0f;
	INT k = 0;
	for (; index->iLightActors + k < model->Lights.Num(); k++)
	{
		AActor* baked = model->Lights(index->iLightActors + k);
		if (!baked || baked == light)
			break;
	}
	if (index->iLightActors + k >= model->Lights.Num() || model->Lights(index->iLightActors + k) != light)
		return 0.0f;
	std::vector<uint8_t> mask((size_t)index->UClamp * index->VClamp);
	FilteredMask(model, *index, k, mask.data());
	float u[4], v[4];
	LightmapAxes(model, model->Surfs(iSurf), *index, u, v);
	const float x = u[0] * point.X + u[1] * point.Y + u[2] * point.Z + u[3];
	const float y = v[0] * point.X + v[1] * point.Y + v[2] * point.Z + v[3];
	const INT x0 = appFloor(x), y0 = appFloor(y);
	const float fx = x - x0, fy = y - y0;
	auto at = [&](INT tx, INT ty) { return (float)mask[Clamp(tx, 0, index->UClamp - 1) + Clamp(ty, 0, index->VClamp - 1) * index->UClamp]; };
	return ((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy) / 255.0f;
}

// Walk a BSP and turn every solid surface into triangles.
//
// A node carries a fan of vertices in its own plane, so the normal is the
// plane's and the fan triangulates without any smoothing to reconstruct.
// Does this texture change by itself? Either it regenerates in place, or it is
// a link in an animation chain. A surface wearing one cannot reuse what was
// accumulated for it on earlier frames.
// An instance's Ambient.w: its magnitude one more than the actor's
// ScaleGlow, which the engine scales a lit mesh's lighting by, plus 32 when
// the actor stands in a fog zone, which is where the engine lays volumetric
// fog over what it draws; and negative when it moved or changed since the
// last frame, which throws away the history of the pixels it covers.
static float InstanceFlags(bool moved, float scaleGlow, AZoneInfo* zone = nullptr)
{
	const float magnitude = 1.0f + Clamp(scaleGlow, 0.0f, 16.0f) + ((zone && zone->bFogZone) ? 32.0f : 0.0f);
	return moved ? -magnitude : magnitude;
}

static bool TextureAnimates(UTexture* texture)
{
	// Judged when the geometry is built. A texture that only gains its
	// animation later - a face, once its owner starts speaking - will not be
	// caught here, but an animated actor's geometry is rebuilt every frame
	// anyway, so its flag is re-evaluated along with it.
	return texture && (texture->bRealtime || texture->bParametric || texture->AnimNext != nullptr);
}

void LevelScene::AddBspSurfaces(UModel* model, SceneGeometry& out, bool skipPortals)
{
	guard(LevelScene::AddBspSurfaces);

	const INT nodeCount = model->Nodes.Num();
	out.Positions.reserve(out.Positions.size() + nodeCount * 6);
	out.Attributes.reserve(out.Attributes.size() + nodeCount * 2);

	// The zones open to the sky: those a window onto the sky zone faces
	// into. Only their ground is wet when the streets are (the trace
	// shader's wetnessAt), which spares every floor indoors the ray that
	// looks for rain.
	bool openToSky[FBspNode::MAX_ZONES] = {};
	for (INT i = 0; i < nodeCount; i++)
	{
		const FBspNode& node = model->Nodes(i);
		if (node.iSurf >= 0 && node.iSurf < model->Surfs.Num() && (model->Surfs(node.iSurf).PolyFlags & PF_FakeBackdrop) &&
			node.iZone[1] < FBspNode::MAX_ZONES)
			openToSky[node.iZone[1]] = true;
	}

	for (INT i = 0; i < nodeCount; i++)
	{
		const FBspNode& node = model->Nodes(i);
		if (node.NumVertices < 3)
			continue;
		if (node.iSurf < 0 || node.iSurf >= model->Surfs.Num())
			continue;

		const FBspSurf& surf = model->Surfs(node.iSurf);

		// Invisible surfaces carry no light. Backdrop and portal surfaces are
		// the sky and zone boundaries rather than geometry; leaving them in
		// would seal the level inside a box. A mover's own brush has neither.
		// Backdrop surfaces are kept: they are windows onto the sky zone, and
		// a ray reaching one carries on from inside the skybox. Leaving them
		// out let rays run on past the edge of the level into nothing, which
		// is the black band along the horizon.
		//
		// So are zone portals the engine draws. A portal it hides is flagged
		// invisible as well, and is dropped by that; one that is not is a
		// visible sheet - the surface of the sea around Liberty Island is the
		// water zone's portal with a water texture on it, and dropping every
		// portal is what left the sea floor showing.
		DWORD skip = PF_Invisible;
		if (surf.PolyFlags & skip)
			continue;

		// A mover's polygons are in the level's own BSP as well as in the brush
		// the mover carries, so drawing both put every door in the scene twice:
		// once wherever it currently is, and once frozen at the position the
		// level was built with. FBspSurf records which brush actor owns the
		// surface, and a moving brush is placed as an instance instead.
		if (skipPortals && surf.Actor && surf.Actor->IsMovingBrush())
			continue;

		vec3 normal = vec3(node.Plane.X, node.Plane.Y, node.Plane.Z);

		const vec3 albedo = AverageColour(surf.Texture, vec3(0.5f, 0.5f, 0.5f));

		// Nothing in this engine marks a surface as emissive. PF_Unlit is the
		// closest it has: the author saying "do not light this, it is already
		// bright". A heuristic, and the main thing a material table replaces.
		bool unlit = false;
		if (surf.PolyFlags & PF_Unlit)
			unlit = true;

		// The zone in front of this node is the one the surface faces into.
		AZoneInfo* zone = nullptr;
		if (node.iZone[1] < FBspNode::MAX_ZONES)
			zone = model->Zones[node.iZone[1]].ZoneActor;
		const vec3 ambient = LightmapAmbient(zone);

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, TextureAnimates(surf.Texture) ? 1.0f : 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);
		// w: the surface is special lit, and only special lights reach it.
		// w: special lit, plus 2 in a fog zone, as for an instance's, plus
		// 4 in a zone open to the sky.
		attr.Ambient = vec4(ambient.x, ambient.y, ambient.z,
			((surf.PolyFlags & PF_SpecialLit) ? 1.0f : 0.0f) + ((zone && zone->bFogZone) ? 2.0f : 0.0f) +
			((node.iZone[1] < FBspNode::MAX_ZONES && openToSky[node.iZone[1]]) ? 4.0f : 0.0f));
		// z: which of the level's lightmaps the surface has, for the engine's
		// own shadow masks on it (see Lightmaps).
		if (model == LightmappedModel)
			attr.Emission.z = (float)AddLightmap(model, node.iSurf);
		const float lightmapRecord = attr.Emission.z;

		// A glowing surface of the level's own is also sampled as a light, so
		// what is around it takes its glow from a shadow ray at it rather
		// than only from the bounces that happen to find it (EmitterGrid.h).
		// Not glass or a decal laid over what is behind it, nor a mirror or a
		// window onto the sky, nor anything in the skybox, which is another
		// place entirely. Its brightness for choosing between emitters is its
		// texture's average; what it gives is read off the texture.
		const bool glows = unlit && model == LightmappedModel && surf.Texture &&
			!(surf.PolyFlags & (PF_Translucent | PF_Modulated | PF_Mirrored | PF_FakeBackdrop)) &&
			!(zone && zone->IsA(ASkyZoneInfo::StaticClass()));
		float glowBrightness = 0.0f;
		if (glows)
		{
			const vec3 c = AverageColour(surf.Texture, vec3(0.0f, 0.0f, 0.0f));
			glowBrightness = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
		}

		// A BSP surface has no stored texture coordinates: the engine derives
		// them from two axis vectors and an origin point, which is what lets one
		// texture run unbroken across many nodes. Projecting a vertex onto those
		// axes gives its position in texture space, in texels, which the texture
		// size turns into the 0..1 the sampler wants.
		// Masked by the surface as well as by the texture, as the engine does:
		// a level can mask one surface of a texture that is solid everywhere
		// else, and a grille drawn unmasked shows palette entry zero - Deus
		// Ex's magenta - through every hole.
		const bool surfaceMasked = surf.Texture && ((surf.PolyFlags | surf.Texture->PolyFlags) & PF_Masked) != 0;
		const int textureIndex = TextureFor(surf.Texture, surfaceMasked);
		FVector textureU(0, 0, 0), textureV(0, 0, 0), textureBase(0, 0, 0);
		float uScale = 0.0f, vScale = 0.0f;
		if (textureIndex >= 0 &&
			surf.vTextureU < model->Vectors.Num() && surf.vTextureV < model->Vectors.Num() &&
			surf.pBase < model->Points.Num() &&
			surf.Texture->USize > 0 && surf.Texture->VSize > 0)
		{
			textureU = model->Vectors(surf.vTextureU);
			textureV = model->Vectors(surf.vTextureV);
			textureBase = model->Points(surf.pBase);
			uScale = 1.0f / (float)surf.Texture->USize;
			vScale = 1.0f / (float)surf.Texture->VSize;
		}
		else
		{
			attr.UV01 = vec4(0.0f, 0.0f, 0.0f, 0.0f);
			attr.UV2Tex = vec4(0.0f, 0.0f, -1.0f, 0.0f);
		}

		// Auto panning - how the clouds in a skybox drift. The engine moves
		// the texture 35 texels a second times its zone's pan speed; carried
		// as texture widths per second in the emission's unused xy, and added
		// in the shader from the level's clock. A surface that moves keeps no
		// history, or the drift would be averaged away.
		if (zone && uScale > 0.0f)
		{
			const float panU = (surf.PolyFlags & PF_AutoUPan) ? 35.0f * zone->TexUPanSpeed * uScale : 0.0f;
			const float panV = (surf.PolyFlags & PF_AutoVPan) ? 35.0f * zone->TexVPanSpeed * vScale : 0.0f;
			attr.Emission.x = panU;
			attr.Emission.y = panV;
			if (panU != 0.0f || panV != 0.0f)
				attr.Albedo.w = 1.0f;
		}
		// A small wavy surface - water, mostly - sways its texture on the
		// level's clock (surfaceUV in the shader), and keeps no history either.
		if ((surf.PolyFlags & PF_SmallWavy) && uScale > 0.0f)
			attr.Albedo.w = 2.0f;

		auto surfaceUV = [&](const FVector& point) -> vec2
		{
			const FVector d = point - textureBase;
			return vec2(
				((d | textureU) + surf.PanU) * uScale,
				((d | textureV) + surf.PanV) * vScale);
		};

		const INT vertPool = node.iVertPool;
		if (vertPool < 0 || vertPool + node.NumVertices > model->Verts.Num())
			continue;

		const FVector& p0 = model->Points(model->Verts(vertPool).pVertex);
		const vec3 v0 = ToVec3(p0);
		for (INT t = 1; t + 1 < node.NumVertices; t++)
		{
			const FVector& p1 = model->Points(model->Verts(vertPool + t).pVertex);
			const FVector& p2 = model->Points(model->Verts(vertPool + t + 1).pVertex);
			const vec3 v1 = ToVec3(p1);
			const vec3 v2 = ToVec3(p2);

			const vec3 e1 = v1 - v0;
			const vec3 e2 = v2 - v0;
			const vec3 cr = cross(e1, e2);
			if (dot(cr, cr) <= 1e-6f)
				continue;

			// The fan shares vertex zero, but each triangle needs its own
			// coordinates, so they are computed per triangle rather than once.
			if (textureIndex >= 0)
			{
				const vec2 uv0 = surfaceUV(p0);
				const vec2 uv1 = surfaceUV(p1);
				const vec2 uv2 = surfaceUV(p2);
				const bool masked = surfaceMasked;
				const bool translucent = (surf.PolyFlags & PF_Translucent) != 0;
				const bool modulated = (surf.PolyFlags & PF_Modulated) != 0;
				const bool mirrored = (surf.PolyFlags & PF_Mirrored) != 0;
				if (mirrored)
					MirroredSurfaces++;
				const float kind = mirrored ? 3.0f : (modulated ? 4.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f)));
				if (kind != 0.0f && kind != 3.0f)
					out.HasMasked = true;
				attr.UV01 = vec4(uv0.x, uv0.y, uv1.x, uv1.y);
				attr.UV2Tex = vec4(uv2.x, uv2.y, (float)textureIndex, kind);
				const vec3 corners[3] = { v0, v1, v2 };
				SetUvDensity(attr, corners);
			}
			if (surf.PolyFlags & PF_FakeBackdrop)
				attr.UV2Tex.w = 5.0f;
			if (textureIndex >= 0)
				SetDetail(attr, surf.Texture);

			// Its emitter's number plus one, where the triangle is one: only
			// an unlit surface's, which has no lightmap for it to displace.
			// Too faint - a sliver, or a dark texture - it is left to the
			// bounces. Its edges run so they cross along the surface's face.
			attr.Emission.z = lightmapRecord;
			const float power = glowBrightness * 0.5f * length(cr);
			if (glows && textureIndex >= 0 && power >= 4.0f)
			{
				EmitterSource e;
				e.V0 = v0;
				const bool flipped = dot(cr, normal) < 0.0f;
				e.E1 = flipped ? e2 : e1;
				e.E2 = flipped ? e1 : e2;
				e.Power = power;
				e.TwoSided = (surf.PolyFlags & PF_TwoSided) != 0;
				EmitterSources.push_back(e);
				EmitterTriangles.push_back((uint32_t)out.Attributes.size());
				attr.Emission.z = (float)EmitterSources.size();
			}

			out.Positions.push_back(v0);
			out.Positions.push_back(v1);
			out.Positions.push_back(v2);
			out.Attributes.push_back(attr);
		}
	}

	unguard;
}

// The detail texture a surface's texture names: fine grain the engine lays
// over the surface close up (detailFactor in Shaders.cpp says how). Carried in
// CornerOffsets, which only a mesh's triangles use: x the detail texture's
// index plus one, 0 for none; y and z how many of its widths and heights one
// of the surface texture's spans, which turns the surface's coordinates into
// its. Both are laid out along the same texture axes, in texels.
void LevelScene::SetDetail(TriangleAttributes& attr, UTexture* texture)
{
	UTexture* detail = texture ? texture->DetailTexture : nullptr;
	if (!detail || detail->USize <= 0 || detail->VSize <= 0 || texture->USize <= 0 || texture->VSize <= 0)
		return;
	const int index = TextureFor(detail);
	if (index < 0)
		return;
	const float scale = detail->Scale > 0.0f ? detail->Scale : 1.0f;
	const float u = (float)texture->USize / ((float)detail->USize * scale);
	const float v = (float)texture->VSize / ((float)detail->VSize * scale);
	attr.CornerOffsets[0] = (uint32_t)index + 1u;
	memcpy(&attr.CornerOffsets[1], &u, sizeof(u));
	memcpy(&attr.CornerOffsets[2], &v, sizeof(v));
}

// Rotor lights whose pattern should turn the other way from the engine's, to
// follow the model it stands for. Named by map and light, because nothing in
// the light itself says which way its fan turns.
static bool IsReversedRotor(AActor* actor)
{
	static const struct { const TCHAR* Map; const TCHAR* Light; } reversed[] = {
		{ TEXT("09_NYC_ShipFan"), TEXT("Light10") },
	};
	UObject* map = actor->XLevel ? actor->XLevel->GetOuter() : nullptr;
	if (!map)
		return false;
	for (const auto& r : reversed)
		if (!appStricmp(map->GetName(), r.Map) && !appStricmp(actor->GetName(), r.Light))
			return true;
	return false;
}

// A light's glow in the air of a fog zone: the engine's volumetric lighting,
// which it bakes into fog maps over each surface and into a fog colour at
// each vertex of a mesh. Everything here is as Render.dll sets it up
// (FLightManager's per light setup and its Fog routine): the glow reaches
// (VolumeRadius + 1) * 25, its strength is VolumeBrightness times the light's
// current brightness over 64, and VolumeFog out of 255 is how much of the
// scene behind it the glow replaces rather than adds to. The colour is the
// light's own scaled by its brightness and the level's, left in display
// terms because the shader blends it over the finished picture exactly as the
// other devices blend a fog map.
void LevelScene::AddFogLight(AActor* actor, const FPlane& colour, float brightness)
{
	if (!actor->VolumeRadius || !actor->VolumeBrightness)
		return;
	if (!actor->Region.Zone || !actor->Region.Zone->bFogZone)
		return;

	const float levelBrightness = actor->Level ? actor->Level->Brightness : 1.0f;
	const float scale = Min(brightness, 1.0f) * levelBrightness;

	SceneLight fog = {};
	fog.PositionRadius = vec4(actor->Location.X, actor->Location.Y, actor->Location.Z, (actor->VolumeRadius + 1) * 25.0f);
	fog.ColorBrightness = vec4(colour.X * scale, colour.Y * scale, colour.Z * scale, actor->VolumeBrightness * Min(brightness, 1.0f) / 64.0f);
	fog.DirectionCone = vec4(actor->VolumeFog / 255.0f, 0.0f, 0.0f, 0.0f);
	FogLights.push_back(fog);
}

void LevelScene::AddLight(AActor* actor)
{
	guardSlow(LevelScene::AddLight);

	if (actor->LightType == LT_None || actor->LightBrightness == 0)
		return;


	// FGetHSV is the engine's own conversion, so a light comes out the
	// colour its author saw. Note the engine's saturation runs the other way
	// round to the usual convention: 255 is white, 0 fully saturated.
	FPlane c = FGetHSV(actor->LightHue, actor->LightSaturation, 255);

	SceneLight light;
	// LightRadius is stored in units of 25, which is why a radius of 8
	// lights a whole room.
	// A special light only reaches special lit surfaces, and says so by
	// carrying its radius negated.
	const float radius = actor->LightRadius * 25.0f;
	light.PositionRadius = vec4(actor->Location.X, actor->Location.Y, actor->Location.Z, actor->bSpecialLit ? -radius : radius);

	// How bright it is right now. The engine's own light types, on its clock:
	// a period of LightPeriod/35 seconds, offset by LightPhase in 256ths of
	// one. Every light used to be taken as steady, so nothing in the game
	// ever pulsed, blinked or flickered.
	float brightness = actor->LightBrightness / 255.0f;
	// Which every type below only ever dims.
	float peak = brightness;
	const bool waver = actor->LightEffect == LE_TorchWaver || actor->LightEffect == LE_FireWaver || actor->LightEffect == LE_WateryShimmer;
	const bool waves = actor->LightEffect == LE_SlowWave || actor->LightEffect == LE_FastWave;
	bool changing = actor->LightEffect == LE_Disco || actor->LightEffect == LE_Searchlight || actor->LightEffect == LE_Rotor || waver || waves;
	const double seconds = actor->Level ? (double)actor->Level->TimeSeconds : 0.0;
	const double cycle = seconds * 35.0 / Max((int)actor->LightPeriod, 1) + actor->LightPhase / 256.0;
	const float wave = (float)std::sin(cycle * 2.0 * PI);
	switch (actor->LightType)
	{
	case LT_Pulse:       brightness *= 0.6f + 0.39f * wave; changing = true; break;
	case LT_SubtlePulse: brightness *= 0.9f + 0.09f * wave; changing = true; break;
	case LT_Blink:
	{
		// The engine's own test, read out of Render.dll: off whenever the low
		// bit of this counter is set. It runs at 35*65536 counts a second, so
		// in practice it is a rapid flicker rather than a slow on and off.
		const double counter = seconds * (35.0 * 65536.0) / (actor->LightPeriod + 1) + actor->LightPhase * 256.0;
		if (((long long)counter) & 1)
			brightness = 0.0f;
		changing = true;
		break;
	}
	case LT_Flicker:
	{
		const float r = appFrand();
		brightness = (r < 0.5f) ? 0.0f : brightness * r;
		changing = true;
		break;
	}
	// One toggle shared by every strobe light, flipped once a frame: on and
	// off on alternate frames, as the engine does it.
	case LT_Strobe:      if (StrobeOff) brightness = 0.0f; changing = true; break;
	default:             break;
	}
	// Dark for the moment, a light keeps its place in the list, at no
	// brightness. Left out, it moved every light after it along one, and the
	// helper, finding the list changed, built its light grid again - which in
	// the Wan Chai canal, where the neon flickers, was 2 ms a frame. Its glow
	// in the fog is left out as before: that list is not what the grid is
	// made from.
	if (brightness <= 0.0f)
		brightness = 0.0f;
	else
		AddFogLight(actor, c, brightness);

	vec3 colour = SrgbToLinear(c.X, c.Y, c.Z);
	if (HighlightSpecialLights)
	{
		const bool spot = actor->LightEffect == LE_Spotlight || actor->LightEffect == LE_StaticSpot;
		const bool shaped = actor->LightEffect == LE_NonIncidence || actor->LightEffect == LE_Cylinder;
		if (changing || spot || shaped)
		{
			colour = changing ? vec3(0.0f, 1.0f, 0.0f) : (spot ? vec3(1.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 1.0f));
			brightness *= 8.0f;
			peak *= 8.0f;
		}
	}
	light.ColorBrightness = vec4(colour.x, colour.y, colour.z, brightness * LightScale);
	light.Peak = vec4(peak * LightScale, 0.0f, 0.0f, 0.0f);

	// Spotlights shine along the actor's rotation, within a cone set by
	// LightCone out of 256: the engine takes one minus that as the cosine
	// of the cone's edge, and scales by the square of how far inside it a
	// point is - all as Render.dll does it. A pawn's spotlight follows where
	// it is looking rather than which way its body faces.
	light.DirectionCone = vec4(0.0f, 0.0f, 0.0f, -1.0f);
	if (actor->LightEffect == LE_Spotlight || actor->LightEffect == LE_StaticSpot)
	{
		APawn* pawn = Cast<APawn>(actor);
		const FVector dir = (pawn ? pawn->ViewRotation : actor->Rotation).Vector();
		light.DirectionCone = vec4(dir.X, dir.Y, dir.Z, 1.0f - actor->LightCone / 256.0f);
	}
	// Patterns worked out in the shader with the engine's own formulas, read
	// out of Render.dll: 0 disco, 1 searchlight, 2 rotor, 3 torch waver,
	// 4 fire waver, 5 watery shimmer, 6 slow wave, 7 fast wave, -1 none.
	float pattern = -1.0f;
	if (actor->LightEffect == LE_Disco)
		pattern = 0.0f;
	else if (waver)
		pattern = 3.0f + (float)(actor->LightEffect - LE_TorchWaver);
	else if (waves)
		pattern = actor->LightEffect == LE_SlowWave ? 6.0f : 7.0f;
	else if (actor->LightEffect == LE_Rotor)
	{
		pattern = 2.0f;
		// Which way it turns, in DirectionCone.x. The engine turns every
		// rotor the same way, which leaves the one over the ship's big fan
		// spinning against the blades casting it; that one light is turned
		// round to match its fan.
		light.DirectionCone.x = IsReversedRotor(actor) ? -1.0f : 1.0f;
	}
	else if (actor->LightEffect == LE_Searchlight)
	{
		pattern = 1.0f;
		// The sweep's offset: the clock over the period, plus the phase in
		// 64ths of a turn, plus a whole turn. Worked out here in double
		// precision and brought down to within one sweep above one, 8 pi
		// (see the shader), so the shader's angle stays positive the way
		// the engine's does once any time has passed.
		const double sweep = actor->LightPeriod ? seconds * 35.0 / actor->LightPeriod : 0.0;
		double offset = actor->LightPhase * (PI / 32.0) + sweep + 2.0 * PI;
		offset = std::fmod(offset, 8.0 * PI) + 8.0 * PI;
		light.DirectionCone.x = (float)offset;
	}

	light.Flags = vec4(
		actor->LightEffect == LE_NonIncidence ? 1.0f : 0.0f,
		(actor->LightEffect == LE_Cylinder ? 1.0f : 0.0f) + (BakedLightIds.count(actor) ? 2.0f * (float)BakedLightIds[actor] : 0.0f),
		changing ? 1.0f : 0.0f,
		pattern);

	Lights.push_back(light);

	unguardSlow;
}

// One of the light augmentation's lights: a Beam, which nothing but
// AugLight spawns, while it shines, carried by the pawn it lights for.
bool LevelScene::IsFlashlightBeam(AActor* actor)
{
	return actor->LightType != LT_None && actor->LightBrightness && actor->Owner && actor->Owner->IsA(APawn::StaticClass()) &&
		!appStricmp(actor->GetClass()->GetName(), TEXT("Beam"));
}

// The light augmentation as a flashlight. The game gives it two lights, one
// moved every tick to just short of wherever the view meets something and
// one at the head (AugLight's SetBeamLocation and SetGlowLocation, in
// DeusEx.u): a round patch of light wherever you look, its shadows falling
// away from the patch rather than from you. In their place, a torch at the
// eyes - a little to the left and up, as a lamp worn at the temple would be,
// so that what it lights shows the edges of its shadows - aimed to cross the
// line of sight 320 units out, so that its hotspot sits on the crosshair
// across a room. Seen from behind, or through someone else's camera, it
// shines from the pawn's eyes the way it is looking, a little in front of
// its face so its own head is not in the way. Its colour is the game's own
// for it, taken half way to white; its beam shows in the air ten times as
// much in a fog zone as out of one. None when beam is null.
void LevelScene::PlaceFlashlight(AActor* beam)
{
	for (vec4& v : Flashlight)
		v = vec4(0.0f, 0.0f, 0.0f, 0.0f);
	if (!beam)
		return;
	APawn* pawn = (APawn*)beam->Owner;

	const FVector pawnEye = pawn->Location + FVector(0.0f, 0.0f, pawn->BaseEyeHeight);
	FVector eye, forward, right, up, lamp;
	if (pawn == ViewActor && !ViewFromBehind && (ViewOrigin - pawnEye).SizeSquared() < 64.0f * 64.0f)
	{
		eye = ViewOrigin;
		forward = ViewForward;
		right = ViewRight;
		up = -ViewDown;
		lamp = eye - right * 6.0f + up * 2.0f;
	}
	else
	{
		const FCoords coords = GMath.UnitCoords / pawn->ViewRotation;
		eye = pawnEye;
		forward = coords.XAxis;
		right = coords.YAxis;
		up = coords.ZAxis;
		lamp = eye + forward * 12.0f - right * 6.0f + up * 2.0f;
	}
	const FVector aim = (eye + forward * 320.0f - lamp).SafeNormal();

	const FPlane c = FGetHSV(beam->LightHue, beam->LightSaturation, 255);
	vec3 colour = SrgbToLinear(c.X, c.Y, c.Z);
	const float lum = 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
	if (lum <= 0.0f)
		return;
	colour = vec3(0.5f + 0.5f * colour.x / lum, 0.5f + 0.5f * colour.y / lum, 0.5f + 0.5f * colour.z / lum);

	// Lit to a matte surface's own colour 256 units off, on the axis.
	const float intensity = 81920.0f * FlashlightBrightness;
	const bool fogZone = pawn->Region.Zone && pawn->Region.Zone->bFogZone;
	const float haze = 5.0e-6f * FlashlightHaze * (fogZone ? 10.0f : 1.0f);

	Flashlight[0] = vec4(lamp.X, lamp.Y, lamp.Z, 1.0f);
	Flashlight[1] = vec4(aim.X, aim.Y, aim.Z, intensity);
	Flashlight[2] = vec4(colour.x, colour.y, colour.z, haze);
}

// A brush's own polygons, which is what the engine treats as its geometry.
//
// The node tree a mover carries is a coarser description of the same shape: a
// door with a window came out as a solid slab, because the detail lives in the
// polygon list. The level's BSP had it right, which is why the door looked
// correct while it was being drawn twice.
void LevelScene::AddBrushPolys(UModel* brush, SceneGeometry& out)
{
	guard(LevelScene::AddBrushPolys);

	UPolys* polys = brush->Polys;
	if (!polys)
		return;

	for (INT i = 0; i < polys->Element.Num(); i++)
	{
		const FPoly& poly = polys->Element(i);
		if (poly.NumVertices < 3)
			continue;
		if (poly.PolyFlags & PF_Invisible)
			continue;

		const vec3 normal = vec3(poly.Normal.X, poly.Normal.Y, poly.Normal.Z);
		const vec3 albedo = AverageColour(poly.Texture, vec3(0.5f, 0.5f, 0.5f));

		bool unlit = false;
		if (poly.PolyFlags & PF_Unlit)
			unlit = true;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, TextureAnimates(poly.Texture) ? 1.0f : 0.0f);
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);
		// A mover is instanced into whatever room it stands in, so its ambient
		// comes from the instance. w: special lit, as for the level's surfaces.
		attr.Ambient = vec4(0.0f, 0.0f, 0.0f, (poly.PolyFlags & PF_SpecialLit) ? 1.0f : 0.0f);

		// Masked by the polygon as well as by the texture, as for the level.
		const bool masked = poly.Texture && ((poly.PolyFlags | poly.Texture->PolyFlags) & PF_Masked) != 0;
		const int textureIndex = TextureFor(poly.Texture, masked);
		const bool translucent = (poly.PolyFlags & PF_Translucent) != 0;
		const bool modulated = (poly.PolyFlags & PF_Modulated) != 0;
		const bool mirrored = (poly.PolyFlags & PF_Mirrored) != 0;
		const float kind = mirrored ? 3.0f : (modulated ? 4.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f)));
		if (kind != 0.0f && kind != 3.0f)
			out.HasMasked = true;

		const bool textured = textureIndex >= 0 && poly.Texture->USize > 0 && poly.Texture->VSize > 0;
		const float uScale = textured ? 1.0f / (float)poly.Texture->USize : 0.0f;
		const float vScale = textured ? 1.0f / (float)poly.Texture->VSize : 0.0f;
		auto polyUV = [&](const FVector& point) -> vec2
		{
			const FVector d = point - poly.Base;
			return vec2(
				((d | poly.TextureU) + poly.PanU) * uScale,
				((d | poly.TextureV) + poly.PanV) * vScale);
		};

		// Triangulated as a fan, which is valid because brush polygons are
		// convex by construction.
		const FVector& p0 = poly.Vertex[0];
		const vec3 v0 = ToVec3(p0);
		for (INT t = 1; t + 1 < poly.NumVertices; t++)
		{
			const FVector& p1 = poly.Vertex[t];
			const FVector& p2 = poly.Vertex[t + 1];
			const vec3 v1 = ToVec3(p1);
			const vec3 v2 = ToVec3(p2);

			const vec3 cr = cross(v1 - v0, v2 - v0);
			if (dot(cr, cr) <= 1e-6f)
				continue;

			if (textured)
			{
				const vec2 uv0 = polyUV(p0);
				const vec2 uv1 = polyUV(p1);
				const vec2 uv2 = polyUV(p2);
				attr.UV01 = vec4(uv0.x, uv0.y, uv1.x, uv1.y);
				attr.UV2Tex = vec4(uv2.x, uv2.y, (float)textureIndex, kind);
				const vec3 corners[3] = { v0, v1, v2 };
				SetUvDensity(attr, corners);
				SetDetail(attr, poly.Texture);
			}
			else
			{
				attr.UV01 = vec4(0.0f, 0.0f, 0.0f, 0.0f);
				attr.UV2Tex = vec4(0.0f, 0.0f, -1.0f, kind);
			}

			out.Positions.push_back(v0);
			out.Positions.push_back(v1);
			out.Positions.push_back(v2);
			out.Attributes.push_back(attr);
		}
	}

	unguard;
}

// Which two keyframes an actor is between, and how far.
//
// AnimFrame is a fraction of the actor's current sequence, so it lands between
// two of that sequence's frames rather than on one. Taking only the nearer of
// the two is what made every character move in steps.
void LevelScene::AnimationPose(UMesh* mesh, FName sequence, FLOAT animFrame, int& frameA, int& frameB, float& alpha)
{
	const FMeshAnimSeq* seq = (sequence != NAME_None) ? mesh->GetAnimSeq(sequence) : nullptr;
	const int start = seq ? seq->StartFrame : 0;
	const int count = (seq && seq->NumFrames > 0) ? seq->NumFrames : Max(mesh->AnimFrames, 1);

	const float position = Clamp((float)animFrame, 0.0f, 0.99999f) * count;
	int a = (int)position;
	alpha = position - (float)a;
	a = Clamp(a, 0, count - 1);
	// Held at the last frame rather than wrapping: the engine resets AnimFrame
	// when a sequence loops, and blending the end back to the start would pass
	// through a pose the animation never takes.
	const int b = Min(a + 1, count - 1);

	frameA = start + a;
	frameB = start + b;
}

// An animated actor gets a geometry of its own, rebuilt every frame.
//
// Sharing one per quantised pose meant a character could only ever hold the
// poses that had been built, and every new one leaked a structure that was
// never freed.
int LevelScene::AnimatedGeometryFor(AActor* actor, UMesh* mesh, int frameA, int frameB, float alpha, UTexture* const skins[8], float styleKind, const FCoords* toLocal)
{
	// Everything the shape depends on. Its placement is not among them: the
	// shape is built in the actor's own space. When none of it has changed
	// since the last frame - a character standing in a paused pose, or a
	// decoration that has animation frames and is not playing any - the
	// geometry is left as it is, which also spares uploading it and
	// rebuilding its acceleration structure.
	uint64_t key = 1469598103934665603ull;
	auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
	auto bits = [](float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return (uint64_t)u; };
	mix((uint64_t)(uintptr_t)mesh);
	mix((uint64_t)frameA);
	mix((uint64_t)frameB);
	mix(bits(alpha));
	mix(bits(styleKind));
	mix((uint64_t)actor->AnimSequence.GetIndex());
	mix(bits(actor->AnimFrame));
#if defined(DEUSEX)
	for (int i = 0; i < 4; i++)
	{
		mix((uint64_t)actor->BlendAnimSequence[i].GetIndex());
		mix(bits(actor->BlendAnimFrame[i]));
	}
#endif
	for (int i = 0; i < 8; i++)
		mix((uint64_t)(uintptr_t)skins[i]);
	mix((uint64_t)(uintptr_t)actor->Texture);
	mix(actor->bUnlit ? 1u : 0u);
	mix(actor->bMeshEnviroMap ? 1u : 0u);
	mix(toLocal ? 1u : 0u);

	auto it = ActorGeometry.find(actor);
	int index;
	if (it != ActorGeometry.end())
	{
		index = it->second;
		auto previous = ActorPoseKeys.find(actor);
		if (previous != ActorPoseKeys.end() && previous->second == key && !Geometries[index].Positions.empty())
			return index;
	}
	else
	{
		if ((int)ActorGeometry.size() >= MaxMeshGeometries)
			return -1;
		Geometries.emplace_back();
		index = (int)Geometries.size() - 1;
		Geometries[index].Dynamic = true;
		ActorGeometry[actor] = index;
		GeometryAdded = true;
	}

	MeshBuilds++;
	const int built = GeometryForMesh(mesh, frameA, frameB, alpha, skins, index, styleKind, actor, toLocal, actor);
	if (built >= 0)
		ActorPoseKeys[actor] = key;
	else
		ActorPoseKeys.erase(actor);
	return built;
}

// A sprite's shape: a unit square facing +Z, centred on the origin, with the
// texture's top left at its top left. Shared by every sprite with the same
// texture and style; each one's size and facing are its instance transform.
static float KindFromStyle(BYTE style);

// How bright the engine draws an unlit mesh: half its ScaleGlow plus its
// AmbientGlow out of 256, no more than full brightness - read out of
// Render.dll's DrawLodMesh. Deus Ex's trees are unlit at a ScaleGlow of 0.35,
// so they are drawn at under a fifth of their texture's brightness; at full
// brightness they glowed across the park at night.
static float UnlitMeshGlow(AActor* actor)
{
	return Clamp(0.5f * (float)actor->ScaleGlow + actor->AmbientGlow / 256.0f, 0.0f, 1.0f);
}

// A lit mesh's ambient, as FLightManager sets it up for the mesh in
// Render.dll: its zone's plus its own AmbientGlow out of 255, or at 255 a
// pulse of 0.25 + 0.2 sin(8t) on the viewport's clock - a dropped pickup
// glowing. In displayed terms: the shader adds it to the mesh's light before
// the clamp, as the engine does, and a mesh gets all of it where a lightmap
// gets half.
vec3 LevelScene::MeshAmbient(AActor* actor, AZoneInfo* zone) const
{
	const float glow = actor->AmbientGlow == 255
		? 0.25f + 0.2f * (float)std::sin(8.0 * ViewportTime)
		: actor->AmbientGlow / 255.0f;
	return ZoneAmbient(zone) + vec3(glow, glow, glow);
}

int LevelScene::GeometryForSprite(UTexture* texture, float kind)
{
	const uint64_t key = ((uint64_t)(uintptr_t)texture << 4) ^ (uint64_t)(int)kind;
	auto it = SpriteGeometry.find(key);
	if (it != SpriteGeometry.end())
		return it->second;

	if ((int)SpriteGeometry.size() >= MaxMeshGeometries)
		return -1;

	Geometries.emplace_back();
	SceneGeometry& geometry = Geometries.back();
	// Never opaque, even for a sprite drawn solid: every hit on one has to be
	// seen by the shader so that it can be kept out of shadow rays. The engine
	// draws sprites flat onto the screen, and they cast no shadows there.
	geometry.HasMasked = true;

	const int textureIndex = TextureFor(texture);
	const vec3 albedo = AverageColour(texture, vec3(1.0f, 1.0f, 1.0f));

	const vec3 corners[4] = {
		vec3(-0.5f,  0.5f, 0.0f),   // top left
		vec3( 0.5f,  0.5f, 0.0f),   // top right
		vec3( 0.5f, -0.5f, 0.0f),   // bottom right
		vec3(-0.5f, -0.5f, 0.0f),   // bottom left
	};
	const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	const int tris[2][3] = { { 0, 3, 2 }, { 0, 2, 1 } };

	for (const auto& tri : tris)
	{
		TriangleAttributes attr;
		attr.Normal = vec4(0.0f, 0.0f, 1.0f, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, TextureAnimates(texture) ? 1.0f : 0.0f);
		// Two marks a sprite: lit by nothing, casting nothing, and as bright
		// as the instance's glow says.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, 2.0f);
		attr.Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);
		attr.UV01 = vec4(uv[tri[0]][0], uv[tri[0]][1], uv[tri[1]][0], uv[tri[1]][1]);
		attr.UV2Tex = vec4(uv[tri[2]][0], uv[tri[2]][1], (float)textureIndex, kind);
		const vec3 laid[3] = { corners[tri[0]], corners[tri[1]], corners[tri[2]] };
		SetUvDensity(attr, laid);

		for (int v = 0; v < 3; v++)
			geometry.Positions.push_back(corners[tri[v]]);
		geometry.Attributes.push_back(attr);
	}

	const int index = (int)Geometries.size() - 1;
	SpriteGeometry[key] = index;
	GeometryAdded = true;
	return index;
}

// Where and how a sprite is drawn, the way the engine's DrawActorSprite does
// it: the actor's texture, DrawScale texels to the world unit, always square on
// to the view and centred on the actor. A sprite that plays once shows the
// frame of its animation matching how much of its life has gone.
bool LevelScene::PlaceSprite(AActor* actor, int& geometryIndex, float transform[12])
{
	UTexture* texture = actor->Texture;
	if (!texture || actor->Style == STY_None)
		return false;

	if (actor->DrawType == DT_SpriteAnimOnce)
	{
		INT count = 1;
		for (UTexture* t = texture->AnimNext; t && t != texture && count < 256; t = t->AnimNext)
			count++;
		INT frame = Clamp(appFloor(actor->LifeFraction() * count), 0, count - 1);
		while (frame-- > 0 && texture->AnimNext)
			texture = texture->AnimNext;
		if (count > 1)
			FixedFrames.insert(texture);
	}

	// The texture's own flags say whether it has holes in it; the style can
	// make the whole thing additive or multiplying instead.
	float kind = KindFromStyle(actor->Style);
	if (kind == 0.0f && (texture->PolyFlags & PF_Translucent))
		kind = 2.0f;
	else if (kind == 0.0f && (texture->PolyFlags & PF_Modulated))
		kind = 4.0f;
	else if (kind == 0.0f && (texture->PolyFlags & PF_Masked))
		kind = 1.0f;

	geometryIndex = GeometryForSprite(texture, kind);
	if (geometryIndex < 0)
		return false;

	const float scale = actor->DrawScale != 0.0f ? actor->DrawScale : 1.0f;
	const float width = scale * texture->USize;
	const float height = scale * texture->VSize;

	// Quad X along the view's right, Y up the screen, Z back at the viewer.
	const FVector up = -ViewDown;
	const FVector columns[3] = { ViewRight * width, up * height, -ViewForward };
	for (int col = 0; col < 3; col++)
	{
		transform[0 * 4 + col] = columns[col].X;
		transform[1 * 4 + col] = columns[col].Y;
		transform[2 * 4 + col] = columns[col].Z;
	}
	const FVector centre = actor->Location + actor->PrePivot;
	transform[0 * 4 + 3] = centre.X;
	transform[1 * 4 + 3] = centre.Y;
	transform[2 * 4 + 3] = centre.Z;
	return true;
}

// The game's blob shadows: a decal it re-attaches under each character every
// tick. The trace casts real shadows from the characters themselves, so these
// only darken the ground a second time. Matched by class name, the class or
// any it derives from, since the game declares it outside the engine.
static bool IsBlobShadow(AActor* actor)
{
	for (UClass* c = actor ? actor->GetClass() : nullptr; c; c = c->GetSuperClass())
	{
		if (appStrstr(c->GetName(), TEXT("Shadow")))
			return true;
	}
	return false;
}

void LevelScene::CollectDecals(ULevel* level)
{
	UModel* model = level ? level->Model : nullptr;
	if (!model)
		return;

	// What is attached where. Cheap enough to walk every frame: most surfaces
	// carry nothing, and only a change costs a rebuild.
	uint64_t signature = 1469598103934665603ull;
	int count = 0;
	for (INT s = 0; s < model->Surfs.Num(); s++)
	{
		const FBspSurf& surf = model->Surfs(s);
		for (INT d = 0; d < surf.Decals.Num(); d++)
		{
			const FDecal& decal = surf.Decals(d);
			if (IsBlobShadow(decal.Actor))
				continue;
			signature = (signature ^ (uint64_t)(uintptr_t)decal.Actor) * 1099511628211ull;
			signature = (signature ^ (uint64_t)s) * 1099511628211ull;
			// Where it is, as well as which it is. A character's blob shadow
			// is one decal re-attached under them every tick: same actor,
			// usually the same floor, only its corners move. Leaving those out
			// held the shadow in place until it crossed onto another surface,
			// then jumped it to catch up.
			uint32_t corners[12];
			memcpy(corners, decal.Vertices, sizeof(corners));
			for (uint32_t c : corners)
				signature = (signature ^ c) * 1099511628211ull;
			count++;
		}
	}

	const bool changed = signature != DecalSignature;
	if (changed)
	{
		DecalSignature = signature;

		const bool created = DecalGeometry < 0;
		if (created)
		{
			if (count == 0)
				return;
			Geometries.emplace_back();
			DecalGeometry = (int)Geometries.size() - 1;
			Geometries[DecalGeometry].Dynamic = true;
		}

		SceneGeometry& geometry = Geometries[DecalGeometry];
		geometry.Version++;
		geometry.Positions.clear();
		geometry.Attributes.clear();
		// Never opaque: anything drawn modulated or translucent is handled
		// when the ray is confirmed against it.
		geometry.HasMasked = true;

		int added = 0;
		for (INT s = 0; s < model->Surfs.Num() && added < MaxDecals; s++)
		{
			const FBspSurf& surf = model->Surfs(s);
			if (surf.Decals.Num() == 0)
				continue;
			// A mover's decals ride along with it, and this is built in the
			// level's space.
			if (surf.Actor && surf.Actor->IsMovingBrush())
				continue;
			if (surf.pBase >= model->Points.Num() || surf.vNormal >= model->Vectors.Num())
				continue;

			const FVector base = model->Points(surf.pBase);
			const FVector surfaceNormal = model->Vectors(surf.vNormal);
			// Lifted off the wall, so that a ray passing through a decal does
			// not start again already beyond the wall it is painted on.
			const FVector lift = surfaceNormal * 0.25f;

			for (INT d = 0; d < surf.Decals.Num() && added < MaxDecals; d++)
			{
				const FDecal& decal = surf.Decals(d);
				ADecal* actor = decal.Actor;
				if (!actor || actor->bHidden || !actor->Texture || actor->Style == STY_None)
					continue;
				if (IsBlobShadow(actor))
					continue;

				UTexture* texture = actor->Texture;
				// The engine draws decals modulated whatever the actor's style
				// says, unless they are translucent. Taking the style as given
				// drew a bullet hole as a lit grey square with a hole in it.
				const float kind = (actor->Style == STY_Translucent) ? 2.0f : 4.0f;

				const int textureIndex = TextureFor(texture, kind == 1.0f);
				const vec3 albedo = AverageColour(texture, vec3(0.5f, 0.5f, 0.5f));

				vec3 corners[4];
				for (int v = 0; v < 4; v++)
					corners[v] = ToVec3(base + decal.Vertices[v] + lift);
				const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
				const int tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

				for (const auto& tri : tris)
				{
					TriangleAttributes attr;
					attr.Normal = vec4(surfaceNormal.X, surfaceNormal.Y, surfaceNormal.Z, 0.0f);
					attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, TextureAnimates(texture) ? 1.0f : 0.0f);
					attr.Emission = vec4(0.0f, 0.0f, 0.0f, 0.0f);
					attr.Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);
					attr.UV01 = vec4(uv[tri[0]][0], uv[tri[0]][1], uv[tri[1]][0], uv[tri[1]][1]);
					attr.UV2Tex = vec4(uv[tri[2]][0], uv[tri[2]][1], (float)textureIndex, kind);
					const vec3 laid[3] = { corners[tri[0]], corners[tri[1]], corners[tri[2]] };
					SetUvDensity(attr, laid);
					for (int v = 0; v < 3; v++)
						geometry.Positions.push_back(corners[tri[v]]);
					geometry.Attributes.push_back(attr);
				}
				added++;
			}
		}

		// Padded to the full reservation, so that the slot of shading data the
		// geometry is given when first built is big enough for every decal it
		// may ever hold.
		TriangleAttributes unused = {};
		unused.UV2Tex = vec4(0.0f, 0.0f, -1.0f, 0.0f);
		geometry.Attributes.resize((size_t)MaxDecals * 2, unused);

		if (created)
			GeometryAdded = true;
	}

	if (DecalGeometry < 0 || Geometries[DecalGeometry].Positions.empty())
		return;

	SceneInstance instance;
	instance.GeometryIndex = DecalGeometry;
	MakeIdentity(instance.Transform);
	// Painted on, a quarter of a unit off the surface: nothing the surface's
	// own light should have to pass. Seen, but no shadow.
	instance.Mask = InstanceCastsNoShadow;
	// Flagged as changed on the frame a decal arrives or goes, so the pixels
	// under it drop what they had accumulated of the bare wall.
	instance.Ambient = vec4(0.0f, 0.0f, 0.0f, InstanceFlags(changed, 1.0f));
	Instances.push_back(instance);
}

int LevelScene::GeometryForBrush(UModel* brush)
{
	auto it = BrushGeometry.find(brush);
	if (it != BrushGeometry.end())
		return it->second;

	Geometries.emplace_back();
	SceneGeometry& geometry = Geometries.back();
	// A mover's brush is its own little model in its own local space. Built from
	// its polygon list rather than its nodes, which is both what the engine
	// treats as the brush's geometry and the only one carrying its detail.
	AddBrushPolys(brush, geometry);
	if (geometry.Positions.empty())
		AddBspSurfaces(brush, geometry, false);

	const int index = (int)Geometries.size() - 1;
	BrushGeometry[brush] = index;
	GeometryAdded = true;
	return index;
}

// The index this texture will have in the shader's array, uploading nothing:
// the upload happens once per frame for whatever the registry ended up holding.
int LevelScene::TextureFor(UTexture* texture, bool masked, AActor* owner)
{
	if (!texture)
		return -1;

	masked = masked || (texture->PolyFlags & PF_Masked) != 0;
	const uint64_t key = ((uint64_t)(uintptr_t)texture << 1) | (masked ? 1u : 0u);
	auto it = TextureIndex.find(key);
	if (it != TextureIndex.end())
		return it->second;

	if (Textures.size() >= (size_t)MaxTextures)
		return -1;

	const int index = (int)Textures.size();
	Textures.push_back(texture);
	TextureMasked.push_back(masked);
	TextureMaterials.push_back(Materials::For(texture, owner));
	// w: the texture's own size, its width plus 4096 times its height, which
	// the trace sways a small wavy surface's texture by (surfaceUV).
	if (texture)
		TextureMaterials.back().w = (float)Min((int)texture->USize, 4095) + 4096.0f * (float)Min((int)texture->VSize, 4095);
	TextureIndex[key] = index;
	return index;
}

// Build a mesh's triangles for a pose.
//
// frameA and frameB are two keyframes and alpha blends between them, which is
// how the engine animates: UE1 meshes store whole vertex positions per frame and
// the pose between two of them is a straight interpolation. Passing the same
// frame twice gives a single keyframe unchanged.
//
// reuseIndex rebuilds into an existing geometry rather than adding one. An
// animated actor needs fresh vertices every frame, and adding a geometry per
// frame per actor would grow without bound.
// The texture the engine reflects in an actor's environment mapped polygons:
// the actor's own Texture, else its zone's, else the level's. Not its skin -
// a character's glasses slot shows whatever this is, which for most people is
// a texture that masks the slot away entirely.
static UTexture* EnvironmentMapFor(AActor* actor)
{
	if (!actor)
		return nullptr;
	if (actor->Texture)
		return actor->Texture;
	if (actor->Region.Zone && actor->Region.Zone->EnvironmentMap)
		return actor->Region.Zone->EnvironmentMap;
	if (actor->Level && actor->Level->EnvironmentMap)
		return actor->Level->EnvironmentMap;
	return nullptr;
}

// World space back into an actor's own, from the same placement its instance
// gets, so the engine can hand back its pose in the space the instance places.
// The columns are the rotation's axes times the draw scale, and dividing each
// by its squared length gives the rows of the inverse.
static FCoords ActorToLocal(AActor* actor)
{
	const float drawScale = actor->DrawScale != 0.0f ? actor->DrawScale : 1.0f;
	float placement[12];
	MakeTransform(actor->Location, actor->Rotation, FVector(drawScale, drawScale, drawScale), actor->PrePivot, placement);
	FCoords toLocal;
	toLocal.Origin = FVector(placement[3], placement[7], placement[11]);
	FVector* rows[3] = { &toLocal.XAxis, &toLocal.YAxis, &toLocal.ZAxis };
	for (int c = 0; c < 3; c++)
	{
		const FVector column(placement[0 * 4 + c], placement[1 * 4 + c], placement[2 * 4 + c]);
		*rows[c] = column / column.SizeSquared();
	}
	return toLocal;
}

// Translate an actor's rendering style into the surface kinds this tracer uses.
// UE1 sets the mode per actor as well as per polygon, and reading only the
// polygon flags left anything relying on Style - the red dot sight's reticle
// among them - drawn as an opaque slab of whatever its texture averaged to.
static float KindFromStyle(BYTE style)
{
	switch (style)
	{
	case STY_Masked:      return 1.0f;
	case STY_Translucent: return 2.0f;
	case STY_Modulated:   return 4.0f;
	default:              return 0.0f;
	}
}

// A mesh's triangles, gathered from its faces and wedges the first time it is
// posed: the same for every pose, where gathering them again cost every
// character every frame. Deus Ex's characters are ULodMesh, which keeps its
// geometry in Faces and Wedges rather than in the Tris it inherits.
const LevelScene::MeshTriangles& LevelScene::TrianglesOf(UMesh* mesh)
{
	auto found = MeshTriangleCache.find(mesh);
	if (found != MeshTriangleCache.end())
		return found->second;

	MeshTriangles& gathered = MeshTriangleCache[mesh];
	ULodMesh* lod = Cast<ULodMesh>(mesh);
	const bool useLod = lod && lod->Faces.Num() > 0;
	const bool remap = useLod && lod->RemapAnimVerts.Num() > 0;
	if (useLod)
	{
		gathered.Triangles.reserve(lod->Faces.Num());
		for (INT i = 0; i < lod->Faces.Num(); i++)
		{
			const FMeshFace& face = lod->Faces(i);

			MeshTriangle tri = {};
			tri.PolyFlags = 0;
			tri.TextureIndex = -1;
			if (face.MaterialIndex < lod->Materials.Num())
			{
				const FMeshMaterial& material = lod->Materials(face.MaterialIndex);
				tri.PolyFlags = material.PolyFlags;
				tri.TextureIndex = material.TextureIndex;
			}

			bool ok = true;
			for (int v = 0; v < 3; v++)
			{
				const _WORD iWedge = face.iWedge[v];
				if (iWedge >= lod->Wedges.Num()) { ok = false; break; }
				tri.Tex[v] = lod->Wedges(iWedge).TexUV;
				INT iVertex = lod->Wedges(iWedge).iVertex;
				INT keyVertex = iVertex;
				if (remap)
				{
					if (iVertex >= lod->RemapAnimVerts.Num()) { ok = false; break; }
					keyVertex = lod->RemapAnimVerts(iVertex);
				}
				tri.Vertex[v] = iVertex;
				tri.KeyVertex[v] = keyVertex;
				gathered.VertexCount = Max(gathered.VertexCount, iVertex + 1);
			}
			if (ok)
				gathered.Triangles.push_back(tri);
		}
	}
	else
	{
		gathered.Triangles.reserve(mesh->Tris.Num());
		for (INT i = 0; i < mesh->Tris.Num(); i++)
		{
			const FMeshTri& src = mesh->Tris(i);
			MeshTriangle tri = {};
			for (int v = 0; v < 3; v++)
			{
				tri.Vertex[v] = src.iVertex[v];
				tri.KeyVertex[v] = src.iVertex[v];
				tri.Tex[v] = src.Tex[v];
				gathered.VertexCount = Max(gathered.VertexCount, (INT)src.iVertex[v] + 1);
			}
			tri.PolyFlags = src.PolyFlags;
			tri.TextureIndex = src.TextureIndex;
			gathered.Triangles.push_back(tri);
		}
	}
	return gathered;
}

int LevelScene::GeometryForMesh(UMesh* mesh, int frameA, int frameB, float alpha, UTexture* const skins[8], int reuseIndex, float styleKind, AActor* owner, const FCoords* toLocal, AActor* envSource)
{
	// The skin set is part of the identity. Hashed rather than compared, so two
	// actors in the same outfit share one structure instead of building another.
	uint64_t skinHash = 1469598103934665603ull;
	for (int i = 0; i < 8; i++)
	{
		skinHash ^= (uint64_t)(uintptr_t)skins[i];
		skinHash *= 1099511628211ull;
	}

	// What environment mapped polygons show is part of the identity too.
	UTexture* const envMap = EnvironmentMapFor(envSource);
	const bool enviroAll = envSource && envSource->bMeshEnviroMap;
	skinHash ^= (uint64_t)(uintptr_t)envMap;
	skinHash *= 1099511628211ull;
	skinHash ^= enviroAll ? 1u : 0u;
	skinHash *= 1099511628211ull;
	// An actor drawn unlit - a muzzle flash, a glowing effect - is unlit in
	// every polygon, whatever the mesh says.
	const bool actorUnlit = envSource && envSource->bUnlit;
	skinHash ^= actorUnlit ? 2u : 0u;
	skinHash *= 1099511628211ull;

#if defined(OLDUNREAL469SDK)
	// What 469 draws a triangle with when its slot has no texture (Render's
	// DrawLodMesh): what it environment maps with - the actor's Texture, its
	// zone's EnvironmentMap, the level's - and failing all three the last
	// texture the mesh has, going through its slots as GetTexture does. UT's
	// pylon has its triangles in slot 1 and its texture in slot 0; the
	// toolbox, the bins, the tyre rim and the wreck are the same a slot
	// along. Made of what the key already holds.
	UTexture* emptySlotTexture = envMap;
	if (!emptySlotTexture)
	{
		for (INT i = 0; i < mesh->Textures.Num() && i < 16; i++)
		{
			UTexture* t = (i < 8 && skins[i]) ? skins[i] : mesh->Textures(i);
			if (t)
				emptySlotTexture = t;
		}
	}
#endif

	// Style is part of the identity: the same mesh drawn normally and drawn
	// translucent are two different shapes as far as the tracer is concerned.
	const uint64_t key = ((uint64_t)(uintptr_t)mesh << 20) ^ ((uint64_t)(frameA & 0xfff) << 8)
		^ (skinHash >> 16) ^ ((uint64_t)(int)styleKind << 60);

	if (reuseIndex < 0)
	{
		auto it = MeshGeometry.find(key);
		if (it != MeshGeometry.end())
			return it->second;

		if ((int)MeshGeometry.size() >= MaxMeshGeometries)
			return -1;
	}

	if (mesh->AnimFrames <= 0)
		return -1;

	frameA = Clamp(frameA, 0, mesh->AnimFrames - 1);
	frameB = Clamp(frameB, 0, mesh->AnimFrames - 1);

	// Deus Ex's characters are ULodMesh, which keeps its geometry in Faces and
	// Wedges rather than in the Tris it inherits - for those meshes Tris is
	// empty, which is why reading only Tris found no characters at all. The two
	// layouts are gathered into one list of triangles here.
	ULodMesh* lod = Cast<ULodMesh>(mesh);
	const bool useLod = lod && lod->Faces.Num() > 0;

	// A mesh converted from the older format animates through a remap table and
	// a different per frame stride.
	const bool remap = useLod && lod->RemapAnimVerts.Num() > 0;
	const INT frameVerts = remap ? lod->OldFrameVerts : mesh->FrameVerts;
	if (frameVerts <= 0)
		return -1;

	// Each frame's vertex block begins with the special attachment vertices -
	// the points a weapon or a head is fixed to - and the wedges index the
	// visible vertices that follow them. Without this offset the first few
	// vertices of every character are read as attachment points, which sit well
	// away from the body: the triangles using them stretch across the level, and
	// one of those covering the camera is a black screen under lighting. Animals
	// and props have no special vertices, which is why only people were affected.
	const INT specialVerts = (useLod && !remap) ? lod->SpecialVerts : 0;
	const INT base = frameA * frameVerts + specialVerts;
	const INT baseB = frameB * frameVerts + specialVerts;

	// With an owner, the pose comes from the engine rather than from reading
	// keyframes here. Deus Ex animates on more than one channel at once: the
	// body plays AnimSequence while up to four blend channels play on top of
	// it, and lip sync is one of those - a talking character's mouth shapes
	// are a blend sequence, not a texture, which is why faces stayed still
	// while every skin and texture measured unchanged. ULodMesh::GetFrame is
	// what the stock renderer calls and it folds all of the channels in, along
	// with the tween between sequences that negative AnimFrame asks for.
	//
	// It returns the attachment vertices first and then the visible ones in
	// the order the wedges index, already remapped. The coordinates handed to
	// it take world space back into the instance's own, so the points land in
	// the same space the keyframe path produces and the instance transform is
	// untouched.
	std::vector<FVector>& enginePoints = PosePoints;
	const bool enginePose = owner && toLocal && useLod && lod->ModelVerts > 0;
	if (enginePose)
	{
		INT request = lod->ModelVerts;
		enginePoints.resize(lod->SpecialVerts + Max(lod->ModelVerts, lod->FrameVerts) + 1);
		const DWORD poseStart = appCycles();
		lod->GetFrame(&enginePoints[0], sizeof(FVector), *toLocal, owner, request);
		CollectStageMs[8] += (DWORD)(appCycles() - poseStart) * GSecondsPerCycle * 1000.0;
	}
	const bool keyframesValid =
		base + (frameVerts - specialVerts) <= mesh->Verts.Num() &&
		baseB + (frameVerts - specialVerts) <= mesh->Verts.Num();
	if (!enginePose && !keyframesValid)
		return -1;

	// While a blend channel is playing, the parts it moves - a talking mouth -
	// change shape without the actor moving, so the actor's placement says
	// nothing changed and the trace kept averaging the face against earlier
	// mouth shapes into a smear. Those parts are found by comparing the
	// engine's pose against the body's own keyframes: whatever differs is
	// being moved by something other than the body's sequence. Only those
	// triangles lose their history; the rest of the character keeps its own.
	// Deus Ex's alone: other engines have no blend channels.
	bool blendActive = false;
#if defined(DEUSEX)
	if (enginePose && keyframesValid)
	{
		for (int i = 0; i < 4; i++)
			if (owner->BlendAnimSequence[i] != NAME_None)
				blendActive = true;
	}
#endif

	if (reuseIndex < 0)
		Geometries.emplace_back();
	SceneGeometry& geometry = reuseIndex >= 0 ? Geometries[reuseIndex] : Geometries.back();
	geometry.Version++;
	geometry.Positions.clear();
	geometry.Attributes.clear();
	geometry.HasMasked = false;

	const MeshTriangles& gathered = TrianglesOf(mesh);
	const std::vector<MeshTriangle>& triangles = gathered.Triangles;
	const INT smoothVertexCount = gathered.VertexCount;

	// The mesh's own import rotation. Deus Ex's characters are modelled facing a
	// different axis and carry a yaw here to correct it, which is why every NPC
	// stood at ninety degrees to where they were looking. Baked into the
	// geometry rather than into the instance: it is a property of the mesh, and
	// the geometry is already cached per mesh.
	//
	// Same convention as MakeTransform - the axes of UnitCoords / Rotation are
	// the columns of the object-to-world matrix, so a vertex is the sum of the
	// axes scaled by its components, not the dot products against them.
	const bool rotateMesh =
		mesh->RotOrigin.Pitch != 0 || mesh->RotOrigin.Yaw != 0 || mesh->RotOrigin.Roll != 0;
	const FCoords meshCoords = GMath.UnitCoords / mesh->RotOrigin;

	geometry.Positions.reserve(triangles.size() * 3);
	geometry.Attributes.reserve(triangles.size());

	// Every triangle is posed first, and the engine's smoothing worked out from
	// all of them before any is written. Render.dll sums the unit normal of
	// each face - hidden ones too - into its three vertices and lights the
	// mesh at those, normalised, blending the light across each face: that is
	// what makes a security camera's eight flat sides look round, where
	// shading each face by its own normal cut it from a block.
	std::vector<PosedTriangle>& posedTriangles = PosedTriangles;
	std::vector<vec3>& vertexNormals = VertexNormals;
	posedTriangles.assign(triangles.size(), PosedTriangle());
	vertexNormals.assign(smoothVertexCount, vec3(0.0f));

	for (size_t t = 0; t < triangles.size(); t++)
	{
		const MeshTriangle& tri = triangles[t];

		FVector p[3];
		bool ok = true;
		bool blended = false;
		for (int v = 0; v < 3; v++)
		{
			FVector posed;
			if (enginePose)
			{
				const size_t index = (size_t)lod->SpecialVerts + (size_t)tri.Vertex[v];
				if (index >= enginePoints.size()) { ok = false; break; }
				posed = enginePoints[index];
				if (!blendActive)
				{
					p[v] = posed;
					continue;
				}
			}

			const INT index = base + tri.KeyVertex[v];
			const INT indexB = baseB + tri.KeyVertex[v];
			if (index < 0 || index >= mesh->Verts.Num()) { ok = false; break; }
			if (indexB < 0 || indexB >= mesh->Verts.Num()) { ok = false; break; }
			// The mesh's own scale and origin are part of its definition rather
			// than of the actor placing it. Origin is documented as being "in
			// original coordinate system" - it is in raw vertex units, so it
			// comes off before the scale rather than after. Adding it afterwards
			// instead put every Deus Ex human 12200 units into the sky, because
			// GM_Trench and its relatives carry Origin.Z = 12200 while animals
			// and props carry zero - which is why the animals looked fine.
			// Interpolated between the two keyframes before anything else is
			// applied. Both transforms below are linear, so blending the raw
			// vertices and blending the finished positions come to the same
			// thing.
			FVector raw = mesh->Verts(index).Vector();
			if (alpha > 0.0f)
			{
				const FVector rawB = mesh->Verts(indexB).Vector();
				raw = raw + (rawB - raw) * alpha;
			}
			p[v] = (raw - mesh->Origin) * mesh->Scale;
			if (rotateMesh)
				p[v] = meshCoords.XAxis * p[v].X + meshCoords.YAxis * p[v].Y + meshCoords.ZAxis * p[v].Z;

			if (enginePose)
			{
				// A small tolerance: the two poses are built by different code
				// and need not agree to the last bit where nothing moved them.
				if ((posed - p[v]).SizeSquared() > 0.05f * 0.05f)
					blended = true;
				p[v] = posed;
			}
		}
		if (!ok)
			continue;

		PosedTriangle& posedTri = posedTriangles[t];
		for (int v = 0; v < 3; v++)
			posedTri.Corners[v] = ToVec3(p[v]);
		const vec3 cr = cross(posedTri.Corners[1] - posedTri.Corners[0], posedTri.Corners[2] - posedTri.Corners[0]);
		const float len2 = dot(cr, cr);
		if (len2 <= 1e-6f)
			continue;

		posedTri.Normal = cr * (1.0f / std::sqrt(len2));
		posedTri.Ok = true;
		posedTri.Blended = blended;
		for (int v = 0; v < 3; v++)
			vertexNormals[tri.Vertex[v]] += posedTri.Normal;
	}

	// What each material comes to - its skin, its slot, its colour and kind -
	// is the same for every triangle using it, and a mesh has a handful: each
	// is worked out the first time a triangle uses it rather than for every
	// triangle, where two table lookups a triangle were a good part of what
	// posing a character cost.
	struct MaterialResult
	{
		INT TextureIndex;
		DWORD PolyFlags;
		bool Environment, Animates, Unlit;
		vec3 Albedo;
		int Texture;
		float Kind;
	};
	static const int MaxMaterials = 32;
	MaterialResult materials[MaxMaterials];
	int materialCount = 0;
	auto resolve = [&](const MeshTriangle& tri, MaterialResult& m)
	{
		m.TextureIndex = tri.TextureIndex;
		m.PolyFlags = tri.PolyFlags;

		// The actor's own skin for this material first: Deus Ex's characters
		// carry no textures on the mesh at all, so without this every person in
		// the game is the same flat grey. The mesh's list is the fallback, which
		// is what props use.
		UTexture* skin = nullptr;
		if (tri.TextureIndex >= 0 && tri.TextureIndex < 8)
			skin = skins[tri.TextureIndex];
		if (!skin && tri.TextureIndex >= 0 && tri.TextureIndex < mesh->Textures.Num())
			skin = mesh->Textures(tri.TextureIndex);
#if defined(OLDUNREAL469SDK)
		if (!skin)
			skin = emptySlotTexture;
#endif
		// Environment mapped: the reflected picture replaces the skin, looked
		// up by reflection direction in the shader rather than by the wedge's
		// coordinates.
		m.Environment = envMap && ((tri.PolyFlags & PF_Environment) || enviroAll);
		if (m.Environment)
			skin = envMap;

		m.Albedo = AverageColour(skin, vec3(0.6f, 0.6f, 0.6f));
		m.Animates = TextureAnimates(skin);
		m.Unlit = actorUnlit || (tri.PolyFlags & PF_Unlit) != 0;

		// Masked by the polygon as well as by the texture, as the engine does:
		// a character with no glasses has a masked glasses slot showing a
		// texture that is nothing but palette entry zero.
		const bool masked = skin && ((skin->PolyFlags & PF_Masked) != 0 || (tri.PolyFlags & PF_Masked) != 0);
		m.Texture = TextureFor(skin, masked, envSource ? envSource : owner);
		const bool translucent = (tri.PolyFlags & PF_Translucent) != 0;
		const bool modulated = (tri.PolyFlags & PF_Modulated) != 0;
		const bool mirrored = (tri.PolyFlags & PF_Mirrored) != 0;
		float kind = mirrored ? 3.0f : (modulated ? 4.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f)));

		// An actor drawn translucent or modulated is drawn that way whatever its
		// polygons say - the style governs the whole mesh rather than filling in
		// for polygons that carry no flags. The laser sight's dot is the case
		// that shows it: its material is flagged masked, its texture has no hole
		// to mask against, so treating the style as a fallback left an opaque
		// quad shaded like a wall panel with the dot in the middle of it.
		if (!mirrored && (styleKind == 2.0f || styleKind == 4.0f))
			kind = styleKind;
		else if (kind == 0.0f)
			kind = styleKind;
		m.Kind = kind;
	};

	for (size_t t = 0; t < triangles.size(); t++)
	{
		const MeshTriangle& tri = triangles[t];
		const PosedTriangle& posedTri = posedTriangles[t];
		if ((tri.PolyFlags & PF_Invisible) || !posedTri.Ok)
			continue;

		const vec3 v0 = posedTri.Corners[0];
		const vec3 v1 = posedTri.Corners[1];
		const vec3 v2 = posedTri.Corners[2];
		const vec3 normal = posedTri.Normal;
		const bool blended = posedTri.Blended;

		const MaterialResult* material = nullptr;
		for (int m = 0; m < materialCount && !material; m++)
			if (materials[m].TextureIndex == tri.TextureIndex && materials[m].PolyFlags == tri.PolyFlags)
				material = &materials[m];
		MaterialResult uncached;
		if (!material)
		{
			MaterialResult& slot = materialCount < MaxMaterials ? materials[materialCount++] : uncached;
			resolve(tri, slot);
			material = &slot;
		}
		if (material->Kind != 0.0f && material->Kind != 3.0f)
			geometry.HasMasked = true;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, material->Environment ? 1.0f : 0.0f);
		// w says the surface keeps no history: a texture that animates, or a
		// part being moved by a blend channel.
		attr.Albedo = vec4(material->Albedo.x, material->Albedo.y, material->Albedo.z, (blended || material->Animates) ? 1.0f : 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		// An unlit actor's whole mesh is drawn at its ScaleGlow, which its
		// instance carries: 1.25 says so. A polygon flagged unlit on a lit
		// actor, like the Dragon's Tooth blade, is drawn at full brightness.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, material->Unlit ? (actorUnlit ? 1.25f : 1.0f) : 0.0f);
		attr.UV01 = vec4(tri.Tex[0].U / 255.0f, tri.Tex[0].V / 255.0f,
		                 tri.Tex[1].U / 255.0f, tri.Tex[1].V / 255.0f);
		attr.UV2Tex = vec4(tri.Tex[2].U / 255.0f, tri.Tex[2].V / 255.0f,
		                   (float)material->Texture, material->Kind);

		// A mesh is instanced into whatever room the actor is standing in, so
		// its ambient comes from the instance rather than from here.
		attr.Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);

		// Each corner's smoothed normal, turned to agree with the face. A
		// vertex whose faces cancel out, or a face wound against its
		// neighbours, keeps the face's own.
		vec3 cornerNormals[3];
		for (int v = 0; v < 3; v++)
		{
			const vec3 sum = vertexNormals[tri.Vertex[v]];
			const float sum2 = dot(sum, sum);
			cornerNormals[v] = sum2 > 1e-8f ? sum * (1.0f / std::sqrt(sum2)) : normal;
			if (dot(cornerNormals[v], normal) <= 0.0f)
				cornerNormals[v] = normal;
		}
		SetCornerNormals(attr, posedTri.Corners, cornerNormals);
		SetUvDensity(attr, posedTri.Corners);

		geometry.Positions.push_back(v0);
		geometry.Positions.push_back(v1);
		geometry.Positions.push_back(v2);
		geometry.Attributes.push_back(attr);
	}

	if (reuseIndex >= 0)
		return geometry.Positions.empty() ? -1 : reuseIndex;

	if (geometry.Positions.empty())
	{
		Geometries.pop_back();
		MeshGeometry[key] = -1;
		return -1;
	}

	const int index = (int)Geometries.size() - 1;
	MeshGeometry[key] = index;
	GeometryAdded = true;

	// Say what the first few meshes actually produced. Instance counts alone
	// cannot tell a mesh built at the wrong scale from one not built at all.
	if (MeshesLogged < 6)
	{
		MeshesLogged++;
		// Each material's texture as it was resolved: slot, flags, and the
		// texture the trace was given for it - an object drawn white is one
		// with none, or with a format that could not be read.
		for (int m = 0; m < materialCount; m++)
		{
			const MaterialResult& r = materials[m];
			UTexture* t = (r.Texture >= 0 && r.Texture < (int)Textures.size()) ? Textures[r.Texture] : nullptr;
			debugf(TEXT("PathTracer mesh '%s' slot %d flags 0x%x kind %.0f%s: %s (%s, format %d, %dx%d)"),
				mesh->GetName(), (int)r.TextureIndex, (unsigned)r.PolyFlags, r.Kind, r.Environment ? TEXT(", environment mapped") : TEXT(""),
				t ? t->GetPathName() : TEXT("no texture"), t ? t->GetClass()->GetName() : TEXT("-"),
				t ? (int)t->Format : -1, t ? (int)t->USize : 0, t ? (int)t->VSize : 0);
		}
		vec3 lo = geometry.Positions[0], hi = geometry.Positions[0];
		for (const vec3& v : geometry.Positions)
		{
			lo = vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
			hi = vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
		}
		UTexture* resolved = skins[0] ? skins[0] : (mesh->Textures.Num() > 0 ? mesh->Textures(0) : nullptr);
		const vec3 resolvedAlbedo = AverageColour(resolved, vec3(0.6f, 0.6f, 0.6f));
		debugf(TEXT("PathTracer mesh '%s' resolved skin '%s' -> albedo %.3f %.3f %.3f"),
			mesh->GetName(),
			resolved ? resolved->GetName() : TEXT("none"),
			resolvedAlbedo.x, resolvedAlbedo.y, resolvedAlbedo.z);

		UTexture* firstSkin = mesh->Textures.Num() > 0 ? mesh->Textures(0) : nullptr;
		debugf(TEXT("PathTracer mesh '%s' skin '%s': MipZero %d %d %d, palette %d"),
			mesh->GetName(),
			firstSkin ? firstSkin->GetName() : TEXT("none"),
			firstSkin ? (int)firstSkin->MipZero.R : -1,
			firstSkin ? (int)firstSkin->MipZero.G : -1,
			firstSkin ? (int)firstSkin->MipZero.B : -1,
			(firstSkin && firstSkin->Palette) ? firstSkin->Palette->Colors.Num() : 0);
		debugf(TEXT("PathTracer mesh '%s': %s, %d tris, frame %d/%d, extent %.1f %.1f %.1f"),
			mesh->GetName(),
			useLod ? TEXT("lod") : TEXT("tris"),
			(int)(geometry.Positions.size() / 3),
			frameA, mesh->AnimFrames,
			hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
	}

	return index;
}

// Collect this frame's placements.
//
// The static world is always instance zero with an identity transform. Movers
// and mesh actors follow, each with the transform the engine currently has them
// at, so the top level structure is rebuilt every frame while the bottom level
// ones are built once per distinct shape.
// One actor's shape placed in the scene, if it has one to place: a mover's
// brush, a mesh, or a sprite. mask says which rays see it (InstanceMask).
// iterated is for what a render iterator hands out - one actor moved to each
// of its items in turn - which has no placement of its own to remember.
void LevelScene::PlaceActor(AActor* actor, uint32_t mask, bool iterated, PlaceCounts& counts)
{
	int geometryIndex = -1;
	FVector scale(1.0f, 1.0f, 1.0f);
	const bool isCharacterActor = (actor->DrawType == DT_Mesh && actor->Mesh && actor->Mesh->AnimFrames > 1);

	if (actor->DrawType == DT_Brush && actor->Brush)
	{
		counts.Brushes++;
		// Movers. The engine keeps the brush in its own space and moves the
		// actor, which is exactly the instancing an acceleration structure
		// wants - the geometry is built once and only the transform moves.
		geometryIndex = GeometryForBrush(actor->Brush);
	}
	else if (actor->DrawType == DT_Mesh && actor->Mesh)
	{
		// Vertex animation: the mesh holds every frame end to end, and
		// AnimFrame picks one. Quantised into a fixed number of buckets
		// rather than taken as it comes: a bottom level structure is built
		// per distinct pose, and an animating character walks through a new
		// one every frame, so the unquantised value builds structures
		// without bound until the cache fills and actors start vanishing.
		// AnimFrame is a fraction of the actor's CURRENT sequence, not of the
		// mesh's whole frame list. Spreading it over every frame the mesh
		// owns picks a pose out of whatever animation happens to live at that
		// offset, which is why characters cycled through unrelated motions.
		int frameA = 0, frameB = 0;
		float alpha = 0.0f;
		AnimationPose(actor->Mesh, actor->AnimSequence, actor->AnimFrame, frameA, frameB, alpha);

		// MultiSkins is where a character's appearance lives. Deliberately
		// NOT actor->Texture: on a pawn that is the editor's sprite icon -
		// S_Pawn, a little green man - and letting it override the skins
		// paints every character in the game the colour of an icon.
		// The order UMesh::GetTexture uses: the actor's per-material skin
		// first, then the mesh's own list, then the actor's single Skin
		// override. Taking only MultiSkins left anything that uses Skin -
		// the street signs among them - with no texture at all, falling
		// back to one averaged colour.
		//
		// Deliberately not actor->Texture: on a pawn that is the editor's
		// S_Pawn sprite icon.
		UTexture* skins[8] = {};
		for (int i = 0; i < 8; i++)
		{
			// GetSkin first, which is what UMesh::GetTexture does and what
			// reading MultiSkins directly skips. It is virtual, so a class
			// may answer differently from what the array holds.
			if (actor->GetSkin(i))
				skins[i] = actor->GetSkin(i);
			else if (actor->MultiSkins[i])
				skins[i] = actor->MultiSkins[i];
			else if (i != 0 && i < actor->Mesh->Textures.Num() && actor->Mesh->Textures(i))
				skins[i] = actor->Mesh->Textures(i);
			else if (actor->Skin)
				skins[i] = actor->Skin;
			else if (i < actor->Mesh->Textures.Num())
				skins[i] = actor->Mesh->Textures(i);
		}

		// Something that animates is rebuilt each frame at its exact pose;
		// anything with a single frame is a shape that can be shared.
		const float styleKind = KindFromStyle(actor->Style);
		const FCoords toLocal = ActorToLocal(actor);

		if (actor->Mesh->AnimFrames > 1)
			geometryIndex = AnimatedGeometryFor(actor, actor->Mesh, frameA, frameB, alpha, skins, styleKind, &toLocal);
		else
			geometryIndex = GeometryForMesh(actor->Mesh, frameA, frameB, 0.0f, skins, -1, styleKind, nullptr, nullptr, actor);
		if (geometryIndex < 0)
			counts.Skipped++;
		else
		{
			counts.Meshes++;
			// More than one animation frame means something that moves:
			// a person, rather than a chair.
			if (actor->Mesh->AnimFrames > 1)
				counts.Animated++;
		}
		const float s = actor->DrawScale != 0.0f ? actor->DrawScale : 1.0f;
		scale = FVector(s, s, s);
	}

	float spriteTransform[12];
	const bool isSprite = actor->DrawType == DT_Sprite || actor->DrawType == DT_SpriteAnimOnce;
	if (isSprite && !PlaceSprite(actor, geometryIndex, spriteTransform))
		return;

	if (geometryIndex < 0)
		return;

	SceneInstance instance;
	instance.GeometryIndex = geometryIndex;
	// A brush and a mesh take the pre-pivot in opposite directions. For a
	// brush it comes off the points before the rotation, the way
	// ABrush::ToWorld spells out; for a mesh actor it offsets the drawn mesh
	// instead. Measured both ways round: with one sign the seated characters
	// sit correctly and the doors ride up, with the other the reverse.
	const bool isBrush = (actor->DrawType == DT_Brush && actor->Brush);
	const FVector prePivot = isBrush ? -actor->PrePivot : actor->PrePivot;
	if (isSprite)
		memcpy(instance.Transform, spriteTransform, sizeof(instance.Transform));
	else
		MakeTransform(actor->Location, actor->Rotation, scale, prePivot, instance.Transform);
	// A sprite is lit by nothing, so its instance carries its glow where
	// anything else carries the zone's ambient. The engine draws it at
	// ScaleGlow brightness, which is how effects fade out. An unlit mesh
	// carries its own brightness the same way: see UnlitMeshGlow.
	const float glow = isSprite ? Clamp((float)actor->ScaleGlow, 0.0f, 4.0f) : UnlitMeshGlow(actor);
	// A lit mesh's ambient is the engine's, in displayed terms, for the shader
	// to add to its light; a mover's is its lightmap's, as on the level.
	const vec3 ambient = (isSprite || actor->bUnlit) ? vec3(glow, glow, glow)
		: isBrush ? LightmapAmbient(actor->Region.Zone) : MeshAmbient(actor, actor->Region.Zone);

	// Did this actor actually move or change shape since the last frame?
	// The trace uses it to throw away the accumulated history of the pixels
	// covering it. Judging that from the hit position alone needed a
	// distance tolerance, and anything moving slower than the tolerance -
	// a medical bot crossing a room - kept its history and smeared.
	//
	// Not for an iterator's items: the one proxy actor stands in for every
	// particle in turn, so it has no single last placement to compare.
	bool moved = true;
	if (!iterated)
	{
		PlacedPose pose;
		pose.GeometryIndex = geometryIndex;
		memcpy(pose.Transform, instance.Transform, sizeof(pose.Transform));
		auto previous = PreviousPoses.find(actor);
		moved = (previous == PreviousPoses.end()) || previous->second != pose;
		CurrentPoses[actor] = pose;

		if (previous != PreviousPoses.end())
		{
			instance.HasPrevious = true;
			memcpy(instance.PreviousTransform, previous->second.Transform, sizeof(instance.PreviousTransform));
		}
	}

	instance.Mask = mask;
	instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, InstanceFlags(moved || isSprite, actor->ScaleGlow, actor->Region.Zone));
	Instances.push_back(instance);
	if (!iterated)
		ActorInstances.push_back({ Instances.size() - 1, actor });

	if (!iterated && !isSprite && actor->DrawType == DT_Mesh && actor->Mesh && !actor->IsA(APawn::StaticClass()))
		FittingCandidates.push_back({ Instances.size() - 1, actor->Mesh->GetRenderBoundingBox(actor, 0) });
}

// The actors a window of the HUD's has hidden while it draws its view: the
// security camera, or the drone, it is seen from. Placed in the frame's scene
// before the window drew, they are taken out of what its view sees.
void LevelScene::HideFromWindows()
{
	for (const auto& placed : ActorInstances)
		if (placed.second->bHidden && placed.first < Instances.size())
			Instances[placed.first].Mask &= ~(uint32_t)InstanceSeenByWindows;
}

// A mesh with a light inside it - a hanging lamp's trough, a desk lamp's
// shade - throws no shadows. The engine lights the level with lightmaps
// that no mesh ever shadows, so a lamp's author put the light where it
// looked right and never saw the fitting stand in its way; traced, the
// fitting shut the light in with it, and the MJ12 lab's hangar floor came
// out at under half its brightness with the ceiling above it lit instead.
// A pawn keeps its shadows even with a light of its own. The bounds are the
// engine's, a few units larger for a light sitting just at the surface.
void LevelScene::UnshadowFittings()
{
	for (const FittingCandidate& fitting : FittingCandidates)
	{
		const FBox bounds = fitting.Bounds.ExpandBy(4.0f);
		for (const FVector& light : LightPositions)
		{
			if (light.X >= bounds.Min.X && light.X <= bounds.Max.X &&
				light.Y >= bounds.Min.Y && light.Y <= bounds.Max.Y &&
				light.Z >= bounds.Min.Z && light.Z <= bounds.Max.Z)
			{
				SceneInstance& instance = Instances[fitting.Instance];
				if (instance.Mask == InstanceSeenByAll)
					instance.Mask = InstanceCastsNoShadow;
				break;
			}
		}
	}
	FittingCandidates.clear();
	LightPositions.clear();
}

// A render iterator's items, each placed as the actor it hands out. The
// engine's own loop: Init with the viewer, then First, IsDone, Next, and
// CurrentItem for each - which for Deus Ex's particles moves the one proxy
// actor to the particle and sets its size and glow, and for a dead particle
// hands back the generator, which draws nothing. 469's iterators are
// initialised with the scene node rather than the viewer, and uninitialised
// after.
void LevelScene::PlaceIterated(AActor* actor, uint32_t mask, PlaceCounts& counts)
{
	URenderIterator* iterator = actor->RenderInterface;
	APlayerPawn* camera = Cast<APlayerPawn>(ViewActor);
	if (!camera)
		return;

#if defined(OLDUNREAL469SDK)
	if (!ViewFrame)
		return;
	iterator->Init(ViewFrame);
#else
	iterator->Init(camera);
#endif
	int items = 0;
	for (iterator->First(); !iterator->IsDone() && items < MaxIteratedItems; iterator->Next(), items++)
	{
		AActor* item = iterator->CurrentItem();
		// A generator turns its particles off by hiding the proxy.
		if (!item || item == actor || item->bHidden)
			continue;
		PlaceActor(item, mask, true, counts);
	}
#if defined(OLDUNREAL469SDK)
	iterator->UnInit();
#endif
}

void LevelScene::CollectDynamic(ULevel* level)
{
	guard(LevelScene::CollectDynamic);

	GeometryAdded = false;
	Instances.clear();
	ActorInstances.clear();
	FittingCandidates.clear();
	LightPositions.clear();
	Lights.clear();
	FogLights.clear();
	CurrentPoses.clear();
	MeshBuilds = 0;

	// Where the sky is seen from: the sky zone of whichever zone the viewer
	// is in, or failing that the level's only one. The engine draws it from
	// that point, so it never shows parallax, looking the view's own way
	// turned by the sky zone's rotation.
	// Strobe lights all share one switch, flipped whenever the clock moves.
	if (level && level->GetLevelInfo() && level->GetLevelInfo()->TimeSeconds != LastStrobeTime)
	{
		LastStrobeTime = level->GetLevelInfo()->TimeSeconds;
		StrobeOff = !StrobeOff;
	}

	HasSky = false;
	{
		AZoneInfo* zone = ViewActor ? ViewActor->Region.Zone : nullptr;
		AActor* sky = zone ? (AActor*)zone->SkyZone : nullptr;
		if (!sky && level)
		{
			for (INT i = 0; i < level->Actors.Num() && !sky; i++)
			{
				AActor* actor = level->Actors(i);
				if (actor && actor->IsA(ASkyZoneInfo::StaticClass()))
					sky = actor;
			}
		}
		if (sky)
		{
			HasSky = true;
			SkyOrigin = sky->Location;
			SkyRotation = sky->Rotation;
		}
	}

	if (Geometries.empty())
		return;

	// Counted so the log can say what kind of thing is actually being placed.
	// "403 instances" never distinguished a room full of furniture from a room
	// full of people, which is the whole question here.
	PlaceCounts counts;
	int hiddenCount = 0;

	for (int g = 0; g < StaticGeometries; g++)
	{
		SceneInstance world;
		world.GeometryIndex = g;
		MakeIdentity(world.Transform);
		Instances.push_back(world);
	}

	if (!level)
		return;

	DWORD lapStart = appCycles();
	auto lap = [&](int stage)
	{
		const DWORD now = appCycles();
		CollectStageMs[stage] += (DWORD)(now - lapStart) * GSecondsPerCycle * 1000.0;
		lapStart = now;
	};

	AActor* flashlightBeam = nullptr;
	const INT actorCount = level->Actors.Num();
	for (INT i = 0; i < actorCount; i++)
	{
		AActor* actor = level->Actors(i);
		if (!actor)
			continue;
		lapStart = appCycles();

		// The light augmentation's lights, which the flashlight stands in
		// for: see PlaceFlashlight. They are never drawn.
		if (UseFlashlight && IsFlashlightBeam(actor))
		{
			if (!flashlightBeam || actor->Owner == ViewActor)
				flashlightBeam = actor;
			lap(0);
			continue;
		}

		// Before the visibility rules: a light still lights the room when the
		// actor carrying it is not drawn, which is exactly what the player's
		// light augmentation is.
		AddLight(actor);
		if (actor->LightType != LT_None && actor->LightBrightness)
			LightPositions.push_back(actor->Location);
		lap(0);

		if (actor->bHidden)
		{
			if (actor->DrawType == DT_Mesh && actor->Mesh)
				hiddenCount++;
			continue;
		}

		// The engine's own visibility rules, which this was not applying at all.
		//
		// The viewpoint sits at head height inside the player's own mesh, so
		// drawing it fills the screen with the inside of JC's chest - flat brown
		// standing still, an arm sweeping past when the run animation plays. The
		// mesh is the right size; a first person camera is simply inside it,
		// which is why the engine never draws your own pawn - unless the view is
		// from behind, as a third person conversation's is. Skipping it outright
		// left JC out of every cutscene and every mirror. A mirror, and anything
		// else that bounces, still sees him; only the view itself and shadows
		// do not, the second because the light augmentation shines from inside
		// him. What only the owner may not see is the same.
		uint32_t mask = InstanceSeenByAll;
		if ((actor == ViewActor || actor == ViewTarget) && !ViewFromBehind)
			mask = InstanceSeenReflected;
		if (actor->bOwnerNoSee && actor->Owner == ViewActor)
			mask = InstanceSeenReflected;
		// In photo mode the body is seen from outside once the camera has
		// left its head, where it would otherwise start inside it.
		if (PhotoMode && (actor == ViewActor || (actor->bOwnerNoSee && actor->Owner == ViewActor)))
			mask = (ViewOrigin - PhotoEye).SizeSquared() > 48.0f * 48.0f ? InstanceSeenFromOutside : InstanceSeenReflected;
		if (actor->bOnlyOwnerSee && actor->Owner != ViewActor)
			continue;

		// Particles. A render iterator hands out one actor moved and scaled in
		// turn to each of its items, the way the engine draws them, and the
		// actor carrying it is drawn as well as whatever it lists. Deus Ex's
		// steam, smoke and sparks come this way, and so do its laser beams.
		// Left out, the engine's own sprite for each particle was all there
		// was, pasted over the picture with nothing in front of it.
		if (actor->RenderInterface)
			PlaceIterated(actor, mask, counts);
		lap(4);

		PlaceActor(actor, mask, false, counts);
		lap((actor->DrawType == DT_Mesh && actor->Mesh && actor->Mesh->AnimFrames > 1) ? 1 : 2);
		if (actor->IsA(APawn::StaticClass()) && PlaceHeldItem((APawn*)actor, mask))
			counts.Held++;
		lap(3);
	}

	lapStart = appCycles();
	PlaceFlashlight(flashlightBeam);
	// The engine gathers volumetric lights only while the player stands in
	// a fog zone (Render.dll, as it walks the level's leaves): anywhere
	// else, none glows at all.
	if (!ViewActor || !ViewActor->Region.Zone || !ViewActor->Region.Zone->bFogZone)
		FogLights.clear();
	lap(0);
	UnshadowFittings();
	lap(5);
	CollectDecals(level);
	lap(6);
	const size_t beforeViewModel = Instances.size();
	if (!PhotoMode)
		AddViewModel();
	lap(7);
	// No weapon this frame: the next one drawn has no previous placement.
	if (Instances.size() == beforeViewModel)
		HaveViewModelTransform = false;


	// This frame's placements become next frame's comparison. Swapped rather
	// than copied, and the old contents are cleared at the start of the next
	// pass, so an actor that has gone away stops being tracked.
	PreviousPoses.swap(CurrentPoses);

	if (!SummaryLogged)
	{
		SummaryLogged = true;
		debugf(TEXT("PathTracer placed: %d movers, %d meshes (%d animated), %d meshes skipped, %d hidden, %d held weapons, %d lights glowing in fog"),
			counts.Brushes, counts.Meshes, counts.Animated, counts.Skipped, hiddenCount, counts.Held, (int)FogLights.size());
	}

	unguard;
}




// A tool in the player's hands - a lockpick, a multitool, the key ring, a
// medkit. Deus Ex holds one in its player's inHand rather than as the pawn's
// Weapon, and draws it as a weapon is drawn (DeusExPlayer.RenderOverlays).
static AInventory* HeldTool(APawn* pawn)
{
	// Looked up once for the player's class rather than every frame.
	static UClass* lookedUp = nullptr;
	static UObjectProperty* property = nullptr;
	if (pawn->GetClass() != lookedUp)
	{
		lookedUp = pawn->GetClass();
		property = FindField<UObjectProperty>(lookedUp, TEXT("inHand"));
	}
	if (!property)
		return nullptr;
	AInventory* item = Cast<AInventory>(*(UObject**)((BYTE*)pawn + property->Offset));
	return (item && !item->IsA(AWeapon::StaticClass())) ? item : nullptr;
}

// The weapon or tool in the player's hands.
//
// The engine draws this as a separate view space pass with its own field of
// view, which this device never sees: it builds the scene from the level rather
// than from what it is handed. So it is placed here instead, at the view's own
// origin and oriented along the view's axes.
void LevelScene::AddViewModel()
{
	guard(LevelScene::AddViewModel);

	// Seen from behind, the view is not from the eyes it would be drawn at.
	APawn* pawn = Cast<APawn>(ViewActor);
	if (!pawn || ViewFromBehind)
		return;

#if defined(OLDUNREAL469SDK)
	// UT draws the first person weapon in RenderOverlays, which leaves it out
	// while it is hidden - as the hidden handedness hides it - and while a
	// zoom narrows the view. It draws the weapon's Mesh, which the weapon
	// swaps for its left handed one.
	AInventory* item = pawn->Weapon;
	APlayerPawn* player = Cast<APlayerPawn>(pawn);
	if (!item || pawn->Weapon->bHideWeapon || (player && player->DesiredFOV != player->DefaultFOV))
		return;
	UMesh* mesh = item->Mesh ? item->Mesh : item->PlayerViewMesh;
#else
	AInventory* item = pawn->Weapon ? (AInventory*)pawn->Weapon : HeldTool(pawn);
	if (!item)
		return;

	// Deus Ex puts the first person model on the inventory actor's own Mesh and
	// hides the actor, rather than filling in PlayerViewMesh - which is why the
	// earlier log showed NanoKeyRingPOV on a hidden actor.
	UMesh* mesh = item->PlayerViewMesh ? item->PlayerViewMesh : item->Mesh;
#endif

	if (!mesh || mesh->AnimFrames <= 0)
		return;

	UTexture* skins[8] = {};
	for (int i = 0; i < 8; i++)
	{
		if (item->GetSkin(i))
			skins[i] = item->GetSkin(i);
		else if (item->MultiSkins[i])
			skins[i] = item->MultiSkins[i];
		else if (i != 0 && i < mesh->Textures.Num() && mesh->Textures(i))
			skins[i] = mesh->Textures(i);
		else if (item->Skin)
			skins[i] = item->Skin;
		else if (i < mesh->Textures.Num())
			skins[i] = mesh->Textures(i);
	}

	int frameA = 0, frameB = 0;
	float alpha = 0.0f;
	AnimationPose(mesh, item->AnimSequence, item->AnimFrame, frameA, frameB, alpha);

	// The pose from the engine, as for characters: it blends into a new
	// sequence rather than snapping to it, which reading the keyframes here
	// could not do. Wherever the engine last put the weapon, the pose comes
	// back in the weapon's own space, and the placement below puts it in
	// front of the camera as before.
	const float styleKind = KindFromStyle(item->Style);
	const FCoords toLocal = ActorToLocal(item);
	const int geometryIndex = (mesh->AnimFrames > 1)
		? AnimatedGeometryFor(item, mesh, frameA, frameB, alpha, skins, styleKind, &toLocal)
		: GeometryForMesh(mesh, frameA, frameB, 0.0f, skins, -1, styleKind, nullptr, nullptr, item);

#if defined(OLDUNREAL469SDK)
	// UT's RenderOverlays places the weapon before drawing it - CalcDrawOffset,
	// which 469 scales for the field of view as FOVFix and WeaponFOVScale say,
	// the bob, and a roll for the hand it is held in - and it has not run yet
	// this frame. Where the weapon was left meanwhile is no guide: in a
	// network game the server's idea of where it is arrives between frames,
	// at the body's middle. So it is placed as the frame is sent, by
	// FinishViewModel, once RenderOverlays has put it where the engine draws
	// it; until then it stands where it is.
	const FCoords own = GMath.UnitCoords / item->Rotation;
	const FVector position = item->Location;
	const FVector axes[3] = { own.XAxis, own.YAxis, own.ZAxis };
	const float scale = item->DrawScale != 0.0f ? item->DrawScale : 1.0f;
	ViewModelIndex = (int)Instances.size();
	ViewModelItem = item;
#else
	// PlayerViewOffset is in the view's own terms: X ahead, Y to the right,
	// Z up. The scene node's axes are X right, Y down, Z forward, so up is
	// minus the down axis.
	const FVector up = -ViewDown;
	// Deus Ex stores this scaled by a hundred: the pistol reads 2200, 1000,
	// -1400, which taken literally puts the weapon 2200 units in front of the
	// camera and well outside the level.
	const FVector offset = item->PlayerViewOffset * 0.01f;
	const FVector position =
		ViewOrigin + ViewForward * offset.X + ViewRight * offset.Y + up * offset.Z;

	// A mesh faces along its own X, so that axis points down the view.
	const FVector axes[3] = { ViewForward, ViewRight, up };
	const float scale = item->PlayerViewScale != 0.0f ? item->PlayerViewScale : 1.0f;
#endif

	SceneInstance instance;
	instance.GeometryIndex = geometryIndex;
	instance.Mask = InstanceSeenByView;
	for (int col = 0; col < 3; col++)
	{
		instance.Transform[0 * 4 + col] = axes[col].X * scale;
		instance.Transform[1 * 4 + col] = axes[col].Y * scale;
		instance.Transform[2 * 4 + col] = axes[col].Z * scale;
	}
	instance.Transform[0 * 4 + 3] = position.X;
	instance.Transform[1 * 4 + 3] = position.Y;
	instance.Transform[2 * 4 + 3] = position.Z;

	const float glow = UnlitMeshGlow(item);
	const vec3 ambient = item->bUnlit ? vec3(glow, glow, glow) : MeshAmbient(item, ViewActor->Region.Zone);
	// Always counted as having moved: it rides the camera, and it bobs even
	// when the camera does not.
	instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, InstanceFlags(true, item->ScaleGlow, ViewActor->Region.Zone));
#if !defined(OLDUNREAL469SDK)
	if (HaveViewModelTransform)
	{
		instance.HasPrevious = true;
		memcpy(instance.PreviousTransform, ViewModelTransform, sizeof(instance.PreviousTransform));
	}
	memcpy(ViewModelTransform, instance.Transform, sizeof(ViewModelTransform));
	HaveViewModelTransform = true;
#endif
	Instances.push_back(instance);

	unguard;
}

// UT's first person weapon, where RenderOverlays has just put it for this
// frame's view (see AddViewModel), exactly as the engine draws it. One not
// within reach of the view was not drawn this frame, and is left out.
void LevelScene::FinishViewModel()
{
#if defined(OLDUNREAL469SDK)
	guard(LevelScene::FinishViewModel);

	const int index = ViewModelIndex;
	AInventory* item = ViewModelItem;
	ViewModelIndex = -1;
	ViewModelItem = nullptr;
	if (index < 0 || index >= (int)Instances.size() || !item)
		return;
	SceneInstance& instance = Instances[index];

	if ((item->Location - ViewOrigin).SizeSquared() > 128.0f * 128.0f)
	{
		instance.Mask = 0;
		HaveViewModelTransform = false;
		return;
	}

	const float scale = item->DrawScale != 0.0f ? item->DrawScale : 1.0f;
	MakeTransform(item->Location, item->Rotation, FVector(scale, scale, scale), item->PrePivot, instance.Transform);
	instance.HasPrevious = HaveViewModelTransform;
	if (HaveViewModelTransform)
		memcpy(instance.PreviousTransform, ViewModelTransform, sizeof(instance.PreviousTransform));
	memcpy(ViewModelTransform, instance.Transform, sizeof(ViewModelTransform));
	HaveViewModelTransform = true;

	unguard;
#endif
}

// The weapon in a character's hands. A held weapon is a hidden actor, so
// placing what the level lists as visible left every armed NPC gripping
// nothing. The engine draws it itself, straight after the character
// (Render.dll's DrawActorSprite): the pawn's Weapon, or empty handed its
// SelectedItem, with the item's ThirdPersonMesh and ThirdPersonScale swapped
// in, its rotation zeroed and the pawn's Style, in the frame DrawLodMesh left
// from the character's weapon triangle - the mesh's first special face.
bool LevelScene::PlaceHeldItem(APawn* pawn, uint32_t mask)
{
	guard(LevelScene::PlaceHeldItem);

	ULodMesh* lod = Cast<ULodMesh>(pawn->Mesh);
	if (pawn->DrawType != DT_Mesh || !lod || lod->SpecialFaces.Num() == 0 || lod->ModelVerts <= 0)
		return false;
	AInventory* item = pawn->Weapon ? (AInventory*)pawn->Weapon : pawn->SelectedItem;
	if (!item || !item->ThirdPersonMesh || item->ThirdPersonMesh->AnimFrames <= 0 || item->ThirdPersonScale == 0.0f)
		return false;
	const FMeshFace& face = lod->SpecialFaces(0);
	for (int i = 0; i < 3; i++)
		if (face.iWedge[i] >= lod->SpecialVerts)
			return false;

	// The character's pose in world space, which GetFrame hands back with the
	// attachment points first.
	INT request = lod->ModelVerts;
	HeldPoints.resize(lod->SpecialVerts + Max(lod->ModelVerts, lod->FrameVerts) + 1);
	lod->GetFrame(&HeldPoints[0], sizeof(FVector), GMath.UnitCoords, pawn, request);
	const FVector a = HeldPoints[face.iWedge[0]];
	const FVector b = HeldPoints[face.iWedge[1]];
	const FVector c = HeldPoints[face.iWedge[2]];

	// The frame DrawLodMesh builds: X along the first edge, Y across the
	// triangle, the origin halfway along the edge from the first corner to
	// the third. It works in the view's space, whose axes are mirrored against
	// the world's, so its two cross products change order here.
	const FVector x = (b - a).SafeNormal();
	const FVector y = ((a - c) ^ x).SafeNormal();
	const FVector z = x ^ y;
	const FVector origin = (a + c) * 0.5f;

	UMesh* mesh = item->ThirdPersonMesh;
	UTexture* skins[8] = {};
	for (int i = 0; i < 8; i++)
	{
		if (item->GetSkin(i))
			skins[i] = item->GetSkin(i);
		else if (item->MultiSkins[i])
			skins[i] = item->MultiSkins[i];
		else if (i != 0 && i < mesh->Textures.Num() && mesh->Textures(i))
			skins[i] = mesh->Textures(i);
		else if (item->Skin)
			skins[i] = item->Skin;
		else if (i < mesh->Textures.Num())
			skins[i] = mesh->Textures(i);
	}

	int frameA = 0, frameB = 0;
	float alpha = 0.0f;
	AnimationPose(mesh, item->AnimSequence, item->AnimFrame, frameA, frameB, alpha);
	const int geometryIndex = GeometryForMesh(mesh, frameA, frameA, 0.0f, skins, -1, KindFromStyle(pawn->Style), nullptr, nullptr, item);
	if (geometryIndex < 0)
		return false;

	// The item's own placement with its rotation zeroed, taken into the frame.
	const float s = item->ThirdPersonScale;
	const FVector axes[3] = { x, y, z };
	const FVector offset = x * (item->PrePivot.X * s) + y * (item->PrePivot.Y * s) + z * (item->PrePivot.Z * s);
	SceneInstance instance;
	instance.GeometryIndex = geometryIndex;
	instance.Mask = mask;
	for (int col = 0; col < 3; col++)
	{
		instance.Transform[0 * 4 + col] = axes[col].X * s;
		instance.Transform[1 * 4 + col] = axes[col].Y * s;
		instance.Transform[2 * 4 + col] = axes[col].Z * s;
	}
	instance.Transform[0 * 4 + 3] = origin.X + offset.X;
	instance.Transform[1 * 4 + 3] = origin.Y + offset.Y;
	instance.Transform[2 * 4 + 3] = origin.Z + offset.Z;

	// Its history is kept as any actor's is. The item is hidden, so nothing
	// else places it under the same key.
	PlacedPose pose;
	pose.GeometryIndex = geometryIndex;
	memcpy(pose.Transform, instance.Transform, sizeof(pose.Transform));
	auto previous = PreviousPoses.find(item);
	const bool moved = (previous == PreviousPoses.end()) || previous->second != pose;
	CurrentPoses[item] = pose;
	if (previous != PreviousPoses.end())
	{
		instance.HasPrevious = true;
		memcpy(instance.PreviousTransform, previous->second.Transform, sizeof(instance.PreviousTransform));
	}

	// Lit where the character is: the engine hands the pawn over as the
	// weapon's light sink.
	const float glow = UnlitMeshGlow(item);
	const vec3 ambient = item->bUnlit ? vec3(glow, glow, glow) : MeshAmbient(item, pawn->Region.Zone);
	instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, InstanceFlags(moved, item->ScaleGlow, pawn->Region.Zone));
	Instances.push_back(instance);
	return true;

	unguard;
}
