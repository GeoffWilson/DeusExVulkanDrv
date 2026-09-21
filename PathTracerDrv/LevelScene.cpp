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

		vec3 albedo = vec3(0.5f, 0.5f, 0.5f);
		if (surf.Texture)
		{
			// MipZero is the texture's overall average colour, which the engine
			// computes when the texture is built. It stands in for sampling the
			// texture itself.
			const FColor& c = surf.Texture->MipZero;
			albedo = SrgbToLinear(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f);
		}

		// Nothing in this engine marks a surface as emissive. PF_Unlit is the
		// closest it has: the author saying "do not light this, it is already
		// bright". A heuristic, and the main thing a material table replaces.
		vec3 emission = vec3(0.0f, 0.0f, 0.0f);
		if (surf.PolyFlags & PF_Unlit)
			emission = albedo * 2.0f;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(emission.x, emission.y, emission.z, 0.0f);

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

int LevelScene::GeometryForMesh(UMesh* mesh, int frame)
{
	const uint64_t key = ((uint64_t)(uintptr_t)mesh << 16) | (uint32_t)(frame & 0xffff);

	auto it = MeshGeometry.find(key);
	if (it != MeshGeometry.end())
		return it->second;

	if ((int)MeshGeometry.size() >= MaxMeshGeometries)
		return -1;

	if (mesh->FrameVerts <= 0 || mesh->AnimFrames <= 0)
		return -1;

	frame = Clamp(frame, 0, mesh->AnimFrames - 1);
	const INT base = frame * mesh->FrameVerts;
	if (base + mesh->FrameVerts > mesh->Verts.Num())
		return -1;

	Geometries.emplace_back();
	SceneGeometry& geometry = Geometries.back();

	const INT triCount = mesh->Tris.Num();
	geometry.Positions.reserve(triCount * 3);
	geometry.Attributes.reserve(triCount);

	for (INT i = 0; i < triCount; i++)
	{
		const FMeshTri& tri = mesh->Tris(i);
		if (tri.PolyFlags & PF_Invisible)
			continue;

		FVector p[3];
		bool ok = true;
		for (int v = 0; v < 3; v++)
		{
			const INT index = base + tri.iVertex[v];
			if (index < 0 || index >= mesh->Verts.Num()) { ok = false; break; }
			// The mesh's own scale and origin are part of its definition rather
			// than of the actor placing it.
			p[v] = (mesh->Verts(index).Vector() * mesh->Scale) + mesh->Origin;
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

		vec3 albedo = vec3(0.6f, 0.6f, 0.6f);
		if (tri.TextureIndex >= 0 && tri.TextureIndex < mesh->Textures.Num() && mesh->Textures(tri.TextureIndex))
		{
			const FColor& c = mesh->Textures(tri.TextureIndex)->MipZero;
			albedo = SrgbToLinear(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f);
		}

		vec3 emission = vec3(0.0f, 0.0f, 0.0f);
		if (tri.PolyFlags & PF_Unlit)
			emission = albedo * 2.0f;

		TriangleAttributes attr;
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(emission.x, emission.y, emission.z, 0.0f);

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
		if (!actor || actor->bHidden)
			continue;

		int geometryIndex = -1;
		FVector scale(1.0f, 1.0f, 1.0f);

		if (actor->DrawType == DT_Brush && actor->Brush)
		{
			// Movers. The engine keeps the brush in its own space and moves the
			// actor, which is exactly the instancing an acceleration structure
			// wants - the geometry is built once and only the transform moves.
			geometryIndex = GeometryForBrush(actor->Brush);
		}
		else if (actor->DrawType == DT_Mesh && actor->Mesh)
		{
			// Vertex animation: the mesh holds every frame end to end, and
			// AnimFrame picks one. Snapped to the nearest, since a bottom level
			// structure is built per pose and interpolating would mean a new one
			// every frame.
			const int frame = (int)(actor->AnimFrame * actor->Mesh->AnimFrames);
			geometryIndex = GeometryForMesh(actor->Mesh, frame);
			const float s = actor->DrawScale != 0.0f ? actor->DrawScale : 1.0f;
			scale = FVector(s, s, s);
		}

		if (geometryIndex < 0)
			continue;

		SceneInstance instance;
		instance.GeometryIndex = geometryIndex;
		MakeTransform(actor->Location, actor->Rotation, scale, instance.Transform);
		Instances.push_back(instance);
	}

	unguard;
}
