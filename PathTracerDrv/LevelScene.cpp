#include "Precomp.h"
#include "LevelScene.h"

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
	// room being pure black. The engine adds it to everything in the zone; the
	// light actors alone are nowhere near the whole picture.
	vec3 ZoneAmbient(AZoneInfo* zone)
	{
		if (!zone || zone->AmbientBrightness == 0)
			return vec3(0.0f, 0.0f, 0.0f);

		FPlane c = FGetHSV(zone->AmbientHue, zone->AmbientSaturation, 255);
		const float brightness = zone->AmbientBrightness / 255.0f;
		return vec3(std::pow(c.X, 2.2f), std::pow(c.Y, 2.2f), std::pow(c.Z, 2.2f)) * brightness;
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
	Lights.clear();
	Instances.clear();
	BrushGeometry.clear();
	MeshGeometry.clear();
	AverageCache.clear();
	ActorPoseKeys.clear();
	SpriteGeometry.clear();
	FixedFrames.clear();
	DecalGeometry = -1;
	DecalSignature = 0;
	ActorGeometry.clear();
	PreviousPoses.clear();
	CurrentPoses.clear();
	Textures.clear();
	TextureMasked.clear();
	TextureIndex.clear();
	GeometryAdded = false;
	MeshesLogged = 0;
	SummaryLogged = false;
	SourceLevel = nullptr;
	SourceNodeCount = 0;
}

bool LevelScene::BuildStatic(ULevel* level)
{
	guard(LevelScene::BuildStatic);

	Clear();

	if (!level || !level->Model)
		return false;

	Geometries.emplace_back();
	AddBspSurfaces(level->Model, Geometries.back(), true);
	// Lights are gathered per frame in CollectDynamic rather than here. The
	// light augmentation turns the player into a light, a thrown flare is a
	// light that moves, and a lamp that is shot out stops being one - none of
	// which a list built once at level load can express.

	SourceLevel = level;
	SourceNodeCount = level->Model->Nodes.Num();
	GeometryAdded = true;

	return !Geometries[0].Positions.empty();

	unguard;
}

// Walk a BSP and turn every solid surface into triangles.
//
// A node carries a fan of vertices in its own plane, so the normal is the
// plane's and the fan triangulates without any smoothing to reconstruct.
// Does this texture change by itself? Either it regenerates in place, or it is
// a link in an animation chain. A surface wearing one cannot reuse what was
// accumulated for it on earlier frames.
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
		const vec3 ambient = ZoneAmbient(zone);

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, TextureAnimates(surf.Texture) ? 1.0f : 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);
		// w: the surface is special lit, and only special lights reach it.
		attr.Ambient = vec4(ambient.x, ambient.y, ambient.z, (surf.PolyFlags & PF_SpecialLit) ? 1.0f : 0.0f);

		// A BSP surface has no stored texture coordinates: the engine derives
		// them from two axis vectors and an origin point, which is what lets one
		// texture run unbroken across many nodes. Projecting a vertex onto those
		// axes gives its position in texture space, in texels, which the texture
		// size turns into the 0..1 the sampler wants.
		const int textureIndex = TextureFor(surf.Texture);
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
				const bool masked = (surf.Texture->PolyFlags & PF_Masked) != 0;
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
			}
			if (surf.PolyFlags & PF_FakeBackdrop)
				attr.UV2Tex.w = 5.0f;

			out.Positions.push_back(v0);
			out.Positions.push_back(v1);
			out.Positions.push_back(v2);
			out.Attributes.push_back(attr);
		}
	}

	unguard;
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

	const vec3 colour = SrgbToLinear(c.X, c.Y, c.Z);
	light.ColorBrightness = vec4(colour.x, colour.y, colour.z, actor->LightBrightness / 255.0f * LightScale);

	Lights.push_back(light);

	unguardSlow;
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

		const int textureIndex = TextureFor(poly.Texture);
		const bool masked = poly.Texture && (poly.Texture->PolyFlags & PF_Masked) != 0;
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
	for (int i = 0; i < 4; i++)
	{
		mix((uint64_t)actor->BlendAnimSequence[i].GetIndex());
		mix(bits(actor->BlendAnimFrame[i]));
	}
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
	// Flagged as changed on the frame a decal arrives or goes, so the pixels
	// under it drop what they had accumulated of the bare wall.
	instance.Ambient = vec4(0.0f, 0.0f, 0.0f, changed ? 1.0f : 0.0f);
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
int LevelScene::TextureFor(UTexture* texture, bool masked)
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
	std::vector<FVector> enginePoints;
	const bool enginePose = owner && toLocal && useLod && lod->ModelVerts > 0;
	if (enginePose)
	{
		INT request = lod->ModelVerts;
		enginePoints.resize(lod->SpecialVerts + Max(lod->ModelVerts, lod->FrameVerts) + 1);
		lod->GetFrame(&enginePoints[0], sizeof(FVector), *toLocal, owner, request);
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
	bool blendActive = false;
	if (enginePose && keyframesValid)
	{
		for (int i = 0; i < 4; i++)
			if (owner->BlendAnimSequence[i] != NAME_None)
				blendActive = true;
	}

	if (reuseIndex < 0)
		Geometries.emplace_back();
	SceneGeometry& geometry = reuseIndex >= 0 ? Geometries[reuseIndex] : Geometries.back();
	geometry.Version++;
	geometry.Positions.clear();
	geometry.Attributes.clear();
	geometry.HasMasked = false;

	struct SourceTriangle
	{
		INT iVertex[3];
		// The same corners as indices into the stored keyframes, which for a
		// remapped mesh are not the ones the engine's pose is ordered by.
		INT keyVertex[3];
		// UE1 stores mesh texture coordinates as a byte per axis spanning the
		// whole texture, so they divide out to 0..1 rather than needing the
		// texture's size the way a BSP surface does.
		FMeshUV Tex[3];
		DWORD PolyFlags;
		INT TextureIndex;
	};

	std::vector<SourceTriangle> triangles;
	if (useLod)
	{
		triangles.reserve(lod->Faces.Num());
		for (INT i = 0; i < lod->Faces.Num(); i++)
		{
			const FMeshFace& face = lod->Faces(i);

			SourceTriangle tri = {};
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
				tri.iVertex[v] = enginePose ? iVertex : keyVertex;
				tri.keyVertex[v] = keyVertex;
			}
			if (ok)
				triangles.push_back(tri);
		}
	}
	else
	{
		triangles.reserve(mesh->Tris.Num());
		for (INT i = 0; i < mesh->Tris.Num(); i++)
		{
			const FMeshTri& src = mesh->Tris(i);
			SourceTriangle tri = {};
			tri.iVertex[0] = src.iVertex[0];
			tri.iVertex[1] = src.iVertex[1];
			tri.iVertex[2] = src.iVertex[2];
			tri.keyVertex[0] = src.iVertex[0];
			tri.keyVertex[1] = src.iVertex[1];
			tri.keyVertex[2] = src.iVertex[2];
			tri.Tex[0] = src.Tex[0];
			tri.Tex[1] = src.Tex[1];
			tri.Tex[2] = src.Tex[2];
			tri.PolyFlags = src.PolyFlags;
			tri.TextureIndex = src.TextureIndex;
			triangles.push_back(tri);
		}
	}

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

	for (const SourceTriangle& tri : triangles)
	{
		if (tri.PolyFlags & PF_Invisible)
			continue;

		FVector p[3];
		bool ok = true;
		bool blended = false;
		for (int v = 0; v < 3; v++)
		{
			FVector posed;
			if (enginePose)
			{
				const size_t index = (size_t)lod->SpecialVerts + (size_t)tri.iVertex[v];
				if (index >= enginePoints.size()) { ok = false; break; }
				posed = enginePoints[index];
				if (!blendActive)
				{
					p[v] = posed;
					continue;
				}
			}

			const INT index = base + tri.keyVertex[v];
			const INT indexB = baseB + tri.keyVertex[v];
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

		const vec3 v0 = ToVec3(p[0]);
		const vec3 v1 = ToVec3(p[1]);
		const vec3 v2 = ToVec3(p[2]);

		const vec3 cr = cross(v1 - v0, v2 - v0);
		const float len2 = dot(cr, cr);
		if (len2 <= 1e-6f)
			continue;

		const vec3 normal = cr * (1.0f / std::sqrt(len2));

		// The actor's own skin for this material first: Deus Ex's characters
		// carry no textures on the mesh at all, so without this every person in
		// the game is the same flat grey. The mesh's list is the fallback, which
		// is what props use.
		UTexture* skin = nullptr;
		if (tri.TextureIndex >= 0 && tri.TextureIndex < 8)
			skin = skins[tri.TextureIndex];
		if (!skin && tri.TextureIndex >= 0 && tri.TextureIndex < mesh->Textures.Num())
			skin = mesh->Textures(tri.TextureIndex);
		// Environment mapped: the reflected picture replaces the skin, looked
		// up by reflection direction in the shader rather than by the wedge's
		// coordinates.
		const bool environment = envMap && ((tri.PolyFlags & PF_Environment) || enviroAll);
		if (environment)
			skin = envMap;

		const vec3 albedo = AverageColour(skin, vec3(0.6f, 0.6f, 0.6f));

		bool unlit = actorUnlit;
		if (tri.PolyFlags & PF_Unlit)
			unlit = true;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, environment ? 1.0f : 0.0f);
		// w says the surface keeps no history: a texture that animates, or a
		// part being moved by a blend channel.
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, (blended || TextureAnimates(skin)) ? 1.0f : 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);

		// Masked by the polygon as well as by the texture, as the engine does:
		// a character with no glasses has a masked glasses slot showing a
		// texture that is nothing but palette entry zero.
		const bool masked = skin && ((skin->PolyFlags & PF_Masked) != 0 || (tri.PolyFlags & PF_Masked) != 0);
		const int textureIndex = TextureFor(skin, masked);
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
		if (kind != 0.0f && kind != 3.0f)
			geometry.HasMasked = true;
		attr.UV01 = vec4(tri.Tex[0].U / 255.0f, tri.Tex[0].V / 255.0f,
		                 tri.Tex[1].U / 255.0f, tri.Tex[1].V / 255.0f);
		attr.UV2Tex = vec4(tri.Tex[2].U / 255.0f, tri.Tex[2].V / 255.0f,
		                   (float)textureIndex, kind);

		// A mesh is instanced into whatever room the actor is standing in, so
		// its ambient comes from the instance rather than from here.
		attr.Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);

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
void LevelScene::CollectDynamic(ULevel* level)
{
	guard(LevelScene::CollectDynamic);

	GeometryAdded = false;
	Instances.clear();
	Lights.clear();
	CurrentPoses.clear();
	MeshBuilds = 0;

	// Where the sky is seen from: the sky zone of whichever zone the viewer
	// is in, or failing that the level's only one. The engine draws it from
	// that point with the view's own direction, so it never shows parallax.
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
		}
	}

	if (Geometries.empty())
		return;

	// Counted so the log can say what kind of thing is actually being placed.
	// "403 instances" never distinguished a room full of furniture from a room
	// full of people, which is the whole question here.
	int brushCount = 0, meshCount = 0, animatedCount = 0, skippedMesh = 0, hiddenCount = 0;

	{
		SceneInstance world;
		world.GeometryIndex = 0;
		MakeIdentity(world.Transform);
		Instances.push_back(world);
	}

	if (!level)
		return;

	const INT actorCount = level->Actors.Num();
	for (INT i = 0; i < actorCount; i++)
	{
		AActor* actor = level->Actors(i);
		if (!actor)
			continue;

		// Before the visibility rules: a light still lights the room when the
		// actor carrying it is not drawn, which is exactly what the player's
		// light augmentation is.
		AddLight(actor);

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
		// which is why the engine never draws your own pawn.
		if (actor == ViewActor)
			continue;
		if (actor->bOwnerNoSee && actor->Owner == ViewActor)
			continue;
		if (actor->bOnlyOwnerSee && actor->Owner != ViewActor)
			continue;

		int geometryIndex = -1;
		FVector scale(1.0f, 1.0f, 1.0f);
		const bool isCharacterActor = (actor->DrawType == DT_Mesh && actor->Mesh && actor->Mesh->AnimFrames > 1);

		if (actor->DrawType == DT_Brush && actor->Brush)
		{
			brushCount++;
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
				skippedMesh++;
			else
			{
				meshCount++;
				// More than one animation frame means something that moves:
				// a person, rather than a chair.
				if (actor->Mesh->AnimFrames > 1)
					animatedCount++;
			}
			const float s = actor->DrawScale != 0.0f ? actor->DrawScale : 1.0f;
			scale = FVector(s, s, s);
		}

		float spriteTransform[12];
		const bool isSprite = actor->DrawType == DT_Sprite || actor->DrawType == DT_SpriteAnimOnce;
		if (isSprite && !PlaceSprite(actor, geometryIndex, spriteTransform))
			continue;

		if (geometryIndex < 0)
			continue;

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
		// ScaleGlow brightness, which is how effects fade out.
		const float glow = Clamp((float)actor->ScaleGlow, 0.0f, 4.0f);
		const vec3 ambient = isSprite ? vec3(glow, glow, glow) : ZoneAmbient(actor->Region.Zone);

		// Did this actor actually move or change shape since the last frame?
		// The trace uses it to throw away the accumulated history of the pixels
		// covering it. Judging that from the hit position alone needed a
		// distance tolerance, and anything moving slower than the tolerance -
		// a medical bot crossing a room - kept its history and smeared.
		PlacedPose pose;
		pose.GeometryIndex = geometryIndex;
		memcpy(pose.Transform, instance.Transform, sizeof(pose.Transform));
		auto previous = PreviousPoses.find(actor);
		const bool moved = (previous == PreviousPoses.end()) || previous->second != pose;
		CurrentPoses[actor] = pose;

		instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, (moved || isSprite) ? 1.0f : 0.0f);
		Instances.push_back(instance);

	}

	CollectDecals(level);
	AddViewModel();


	// This frame's placements become next frame's comparison. Swapped rather
	// than copied, and the old contents are cleared at the start of the next
	// pass, so an actor that has gone away stops being tracked.
	PreviousPoses.swap(CurrentPoses);

	if (!SummaryLogged)
	{
		SummaryLogged = true;
		debugf(TEXT("PathTracer placed: %d movers, %d meshes (%d animated), %d meshes skipped, %d hidden"),
			brushCount, meshCount, animatedCount, skippedMesh, hiddenCount);
	}

	unguard;
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

	APawn* pawn = Cast<APawn>(ViewActor);
	if (!pawn || !pawn->Weapon)
		return;

	AInventory* item = pawn->Weapon;

	// Deus Ex puts the first person model on the inventory actor's own Mesh and
	// hides the actor, rather than filling in PlayerViewMesh - which is why the
	// earlier log showed NanoKeyRingPOV on a hidden actor.
	UMesh* mesh = item->PlayerViewMesh ? item->PlayerViewMesh : item->Mesh;

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

	SceneInstance instance;
	instance.GeometryIndex = geometryIndex;
	for (int col = 0; col < 3; col++)
	{
		instance.Transform[0 * 4 + col] = axes[col].X * scale;
		instance.Transform[1 * 4 + col] = axes[col].Y * scale;
		instance.Transform[2 * 4 + col] = axes[col].Z * scale;
	}
	instance.Transform[0 * 4 + 3] = position.X;
	instance.Transform[1 * 4 + 3] = position.Y;
	instance.Transform[2 * 4 + 3] = position.Z;

	const vec3 ambient = ZoneAmbient(ViewActor->Region.Zone);
	// Always counted as having moved: it rides the camera, and it bobs even
	// when the camera does not.
	instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, 1.0f);
	Instances.push_back(instance);

	unguard;
}
