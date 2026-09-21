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

		const FColor& c = texture->MipZero;
		if (c.R > 4 || c.G > 4 || c.B > 4)
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
	void MakeTransform(const FVector& location, const FRotator& rotation, const FVector& scale, float out[12])
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
		out[0 * 4 + 3] = location.X;
		out[1 * 4 + 3] = location.Y;
		out[2 * 4 + 3] = location.Z;
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

		vec3 normal = vec3(node.Plane.X, node.Plane.Y, node.Plane.Z);

		const vec3 albedo = AverageColour(surf.Texture, vec3(0.5f, 0.5f, 0.5f));

		// Nothing in this engine marks a surface as emissive. PF_Unlit is the
		// closest it has: the author saying "do not light this, it is already
		// bright". A heuristic, and the main thing a material table replaces.
		vec3 emission = vec3(0.0f, 0.0f, 0.0f);
		if (surf.PolyFlags & PF_Unlit)
			emission = albedo * 2.0f;

		// The zone in front of this node is the one the surface faces into.
		AZoneInfo* zone = nullptr;
		if (node.iZone[1] < FBspNode::MAX_ZONES)
			zone = model->Zones[node.iZone[1]].ZoneActor;
		const vec3 ambient = ZoneAmbient(zone);

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(emission.x, emission.y, emission.z, 0.0f);
		attr.Ambient = vec4(ambient.x, ambient.y, ambient.z, 0.0f);

		const INT vertPool = node.iVertPool;
		if (vertPool < 0 || vertPool + node.NumVertices > model->Verts.Num())
			continue;

		const vec3 v0 = ToVec3(model->Points(model->Verts(vertPool).pVertex));
		for (INT t = 1; t + 1 < node.NumVertices; t++)
		{
			const vec3 v1 = ToVec3(model->Points(model->Verts(vertPool + t).pVertex));
			const vec3 v2 = ToVec3(model->Points(model->Verts(vertPool + t + 1).pVertex));

			const vec3 e1 = v1 - v0;
			const vec3 e2 = v2 - v0;
			const vec3 cr = cross(e1, e2);
			if (dot(cr, cr) <= 1e-6f)
				continue;

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

int LevelScene::GeometryForBrush(UModel* brush)
{
	auto it = BrushGeometry.find(brush);
	if (it != BrushGeometry.end())
		return it->second;

	Geometries.emplace_back();
	SceneGeometry& geometry = Geometries.back();
	// A mover's brush is its own little model in its own local space, and its
	// portal flags mean nothing here.
	AddBspSurfaces(brush, geometry, false);

	const int index = (int)Geometries.size() - 1;
	BrushGeometry[brush] = index;
	GeometryAdded = true;
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

		vec3 emission = vec3(0.0f, 0.0f, 0.0f);
		if (tri.PolyFlags & PF_Unlit)
			emission = albedo * 2.0f;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(emission.x, emission.y, emission.z, 0.0f);
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
			UTexture* skins[8] = {};
			for (int i = 0; i < 8; i++)
				skins[i] = actor->MultiSkins[i];

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
		MakeTransform(actor->Location, actor->Rotation, scale, instance.Transform);
		const vec3 ambient = ZoneAmbient(actor->Region.Zone);
		// w marks a character, purely so the debug view can tell one from a
		// chair.
		instance.Ambient = vec4(ambient.x, ambient.y, ambient.z, isCharacterActor ? 1.0f : 0.0f);
		Instances.push_back(instance);

	}

	if (!SummaryLogged)
	{
		SummaryLogged = true;
		debugf(TEXT("PathTracer placed: %d movers, %d meshes (%d animated), %d meshes skipped, %d hidden"),
			brushCount, meshCount, animatedCount, skippedMesh, hiddenCount);
	}

	unguard;
}


