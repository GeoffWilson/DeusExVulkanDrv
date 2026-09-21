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
	vec3 AverageColour(UTexture* texture, vec3 fallback)
	{
		if (!texture)
			return fallback;

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
	PreviousPoses.clear();
	CurrentPoses.clear();
	Textures.clear();
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
	AddLights(level);

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
		DWORD skip = PF_Invisible;
		if (skipPortals)
			skip |= PF_FakeBackdrop | PF_Portal;
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
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);
		attr.Ambient = vec4(ambient.x, ambient.y, ambient.z, 0.0f);

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
				const bool translucent = (surf.PolyFlags & (PF_Translucent | PF_Modulated)) != 0;
				const bool mirrored = (surf.PolyFlags & PF_Mirrored) != 0;
				if (mirrored)
					MirroredSurfaces++;
				const float kind = mirrored ? 3.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f));
				if (kind == 1.0f || kind == 2.0f)
					out.HasMasked = true;
				attr.UV01 = vec4(uv0.x, uv0.y, uv1.x, uv1.y);
				attr.UV2Tex = vec4(uv2.x, uv2.y, (float)textureIndex, kind);
			}

			out.Positions.push_back(v0);
			out.Positions.push_back(v1);
			out.Positions.push_back(v2);
			out.Attributes.push_back(attr);
		}
	}

	unguard;
}

void LevelScene::AddLights(ULevel* level)
{
	guard(LevelScene::AddLights);

	const INT actorCount = level->Actors.Num();
	for (INT i = 0; i < actorCount; i++)
	{
		AActor* actor = level->Actors(i);
		if (!actor)
			continue;
		if (actor->LightType == LT_None || actor->LightBrightness == 0)
			continue;

		// FGetHSV is the engine's own conversion, so a light comes out the
		// colour its author saw. Note the engine's saturation runs the other way
		// round to the usual convention: 255 is white, 0 fully saturated.
		FPlane c = FGetHSV(actor->LightHue, actor->LightSaturation, 255);

		SceneLight light;
		// LightRadius is stored in units of 25, which is why a radius of 8
		// lights a whole room.
		light.PositionRadius = vec4(actor->Location.X, actor->Location.Y, actor->Location.Z, actor->LightRadius * 25.0f);

		const vec3 colour = SrgbToLinear(c.X, c.Y, c.Z);
		light.ColorBrightness = vec4(colour.x, colour.y, colour.z, actor->LightBrightness / 255.0f);

		Lights.push_back(light);
	}

	unguard;
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
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);
		// A mover is instanced into whatever room it stands in, so its ambient
		// comes from the instance.
		attr.Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);

		const int textureIndex = TextureFor(poly.Texture);
		const bool masked = poly.Texture && (poly.Texture->PolyFlags & PF_Masked) != 0;
		const bool translucent = (poly.PolyFlags & (PF_Translucent | PF_Modulated)) != 0;
		const bool mirrored = (poly.PolyFlags & PF_Mirrored) != 0;
		const float kind = mirrored ? 3.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f));
		if (kind == 1.0f || kind == 2.0f)
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
int LevelScene::TextureFor(UTexture* texture)
{
	if (!texture)
		return -1;

	auto it = TextureIndex.find(texture);
	if (it != TextureIndex.end())
		return it->second;

	if (Textures.size() >= (size_t)MaxTextures)
		return -1;

	const int index = (int)Textures.size();
	Textures.push_back(texture);
	TextureIndex[texture] = index;
	return index;
}

int LevelScene::GeometryForMesh(UMesh* mesh, int frame, UTexture* const skins[8])
{
	// The skin set is part of the identity. Hashed rather than compared, so two
	// actors in the same outfit share one structure instead of building another.
	uint64_t skinHash = 1469598103934665603ull;
	for (int i = 0; i < 8; i++)
	{
		skinHash ^= (uint64_t)(uintptr_t)skins[i];
		skinHash *= 1099511628211ull;
	}

	const uint64_t key = ((uint64_t)(uintptr_t)mesh << 20) ^ ((uint64_t)(frame & 0xfff) << 8) ^ (skinHash >> 16);

	auto it = MeshGeometry.find(key);
	if (it != MeshGeometry.end())
		return it->second;

	if ((int)MeshGeometry.size() >= MaxMeshGeometries)
		return -1;

	if (mesh->AnimFrames <= 0)
		return -1;

	frame = Clamp(frame, 0, mesh->AnimFrames - 1);

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
	const INT base = frame * frameVerts + specialVerts;
	if (base + (frameVerts - specialVerts) > mesh->Verts.Num())
		return -1;

	Geometries.emplace_back();
	SceneGeometry& geometry = Geometries.back();

	struct SourceTriangle
	{
		INT iVertex[3];
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
				if (remap)
				{
					if (iVertex >= lod->RemapAnimVerts.Num()) { ok = false; break; }
					iVertex = lod->RemapAnimVerts(iVertex);
				}
				tri.iVertex[v] = iVertex;
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
		for (int v = 0; v < 3; v++)
		{
			const INT index = base + tri.iVertex[v];
			if (index < 0 || index >= mesh->Verts.Num()) { ok = false; break; }
			// The mesh's own scale and origin are part of its definition rather
			// than of the actor placing it. Origin is documented as being "in
			// original coordinate system" - it is in raw vertex units, so it
			// comes off before the scale rather than after. Adding it afterwards
			// instead put every Deus Ex human 12200 units into the sky, because
			// GM_Trench and its relatives carry Origin.Z = 12200 while animals
			// and props carry zero - which is why the animals looked fine.
			p[v] = (mesh->Verts(index).Vector() - mesh->Origin) * mesh->Scale;
			if (rotateMesh)
				p[v] = meshCoords.XAxis * p[v].X + meshCoords.YAxis * p[v].Y + meshCoords.ZAxis * p[v].Z;
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
		const vec3 albedo = AverageColour(skin, vec3(0.6f, 0.6f, 0.6f));

		bool unlit = false;
		if (tri.PolyFlags & PF_Unlit)
			unlit = true;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		// w marks the surface as self lit. The colour it emits is whatever it
		// turns out to be once sampled, so it cannot be decided here: doing so
		// is what made unlit masked surfaces glow the key colour.
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, unlit ? 1.0f : 0.0f);

		const int textureIndex = TextureFor(skin);
		const bool masked = skin && (skin->PolyFlags & PF_Masked) != 0;
		const bool translucent = (tri.PolyFlags & (PF_Translucent | PF_Modulated)) != 0;
		const bool mirrored = (tri.PolyFlags & PF_Mirrored) != 0;
		const float kind = mirrored ? 3.0f : (translucent ? 2.0f : (masked ? 1.0f : 0.0f));
		if (kind == 1.0f || kind == 2.0f)
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
			frame, mesh->AnimFrames,
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
	CurrentPoses.clear();

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
			const FMeshAnimSeq* seq =
				actor->AnimSequence != NAME_None ? actor->Mesh->GetAnimSeq(actor->AnimSequence) : nullptr;
			const int startFrame = seq ? seq->StartFrame : 0;
			const int seqFrames = (seq && seq->NumFrames > 0) ? seq->NumFrames : Max(actor->Mesh->AnimFrames, 1);
			const int buckets = Max(PoseBuckets, 1);
			const int bucket = Clamp((int)(actor->AnimFrame * buckets), 0, buckets - 1);
			const int frame = startFrame + (bucket * seqFrames) / buckets;

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
				if (actor->MultiSkins[i])
					skins[i] = actor->MultiSkins[i];
				else if (i != 0 && i < actor->Mesh->Textures.Num() && actor->Mesh->Textures(i))
					skins[i] = actor->Mesh->Textures(i);
				else if (actor->Skin)
					skins[i] = actor->Skin;
				else if (i < actor->Mesh->Textures.Num())
					skins[i] = actor->Mesh->Textures(i);
			}

			geometryIndex = GeometryForMesh(actor->Mesh, frame, skins);
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
		MakeTransform(actor->Location, actor->Rotation, scale, prePivot, instance.Transform);
		const vec3 ambient = ZoneAmbient(actor->Region.Zone);

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

		instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, moved ? 1.0f : 0.0f);
		Instances.push_back(instance);

	}

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


