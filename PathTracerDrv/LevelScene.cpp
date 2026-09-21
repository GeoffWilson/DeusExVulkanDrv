#include "Precomp.h"
#include "LevelScene.h"

namespace
{
	// The engine hands out display space colours. Everything here works in
	// linear light, so they have to be decoded on the way in.
	inline vec3 SrgbToLinear(FLOAT r, FLOAT g, FLOAT b)
	{
		return vec3(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f));
	}

	inline vec3 ToVec3(const FVector& v)
	{
		return vec3(v.X, v.Y, v.Z);
	}
}

void LevelScene::Clear()
{
	Positions.clear();
	Attributes.clear();
	Lights.clear();
	SourceLevel = nullptr;
	SourceNodeCount = 0;
}

bool LevelScene::Build(ULevel* level)
{
	guard(LevelScene::Build);

	Clear();

	if (!level || !level->Model)
		return false;

	AddSurfaces(level->Model);
	AddLights(level);

	SourceLevel = level;
	SourceNodeCount = level->Model->Nodes.Num();

	return !Positions.empty();

	unguard;
}

// Walk the BSP and turn every solid surface into triangles.
//
// A node carries a fan of vertices that all lie in its own plane, so the normal
// is the plane's and the fan triangulates without any smoothing to worry about -
// these surfaces are flat by construction.
void LevelScene::AddSurfaces(UModel* model)
{
	guard(LevelScene::AddSurfaces);

	const INT nodeCount = model->Nodes.Num();
	Positions.reserve(nodeCount * 6);
	Attributes.reserve(nodeCount * 2);

	for (INT i = 0; i < nodeCount; i++)
	{
		const FBspNode& node = model->Nodes(i);
		if (node.NumVertices < 3)
			continue;
		if (node.iSurf < 0 || node.iSurf >= model->Surfs.Num())
			continue;

		const FBspSurf& surf = model->Surfs(node.iSurf);

		// Invisible surfaces carry no light and are not meant to be seen.
		// Backdrop surfaces are the sky portal rather than geometry; leaving
		// them in would seal the level inside a box of whatever the sky texture
		// averages to, which is the opposite of what they mean.
		if (surf.PolyFlags & (PF_Invisible | PF_FakeBackdrop | PF_Portal))
			continue;

		// The node's plane is this surface's plane, already wound to face out.
		vec3 normal = vec3(node.Plane.X, node.Plane.Y, node.Plane.Z);

		vec3 albedo = vec3(0.5f, 0.5f, 0.5f);
		if (surf.Texture)
		{
			// MipZero is the texture's overall average colour, which the engine
			// computes when the texture is built. It stands in for sampling the
			// texture itself: enough to give every surface its own colour
			// without a bindless texture array behind the trace shader.
			const FColor& c = surf.Texture->MipZero;
			albedo = SrgbToLinear(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f);
		}

		// Nothing in this engine marks a surface as emissive. PF_Unlit is the
		// closest thing it has: the level author saying "do not light this, it
		// is already bright" - screens, signs, the glowing part of a light
		// fitting. Treating it as a weak emitter is a heuristic, and the main
		// thing a hand authored material table would replace.
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

			// Degenerate fans are common enough in shipped levels; an
			// acceleration structure would rather not be given them.
			const vec3 e1 = v1 - v0;
			const vec3 e2 = v2 - v0;
			const vec3 cr = cross(e1, e2);
			if (dot(cr, cr) <= 1e-6f)
				continue;

			Positions.push_back(v0);
			Positions.push_back(v1);
			Positions.push_back(v2);
			Attributes.push_back(attr);
		}
	}

	unguard;
}

// Collect the light actors.
//
// These are the emitters the engine used to bake its lightmaps, so they are the
// level author's own lighting rather than an approximation of it. What they are
// not is physical: brightness and radius are bytes on a curve chosen to look
// right through the engine's own falloff, and several LightEffect modes have no
// physical meaning at all.
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
		// colour the level author saw in the editor. Note the engine's
		// saturation runs the other way round to the usual convention: 255 is
		// white, 0 is fully saturated.
		FPlane c = FGetHSV(actor->LightHue, actor->LightSaturation, 255);

		SceneLight light;
		// LightRadius is stored in units of 25, which is the engine's own
		// scaling and the reason a radius of 8 lights a whole room.
		light.PositionRadius = vec4(actor->Location.X, actor->Location.Y, actor->Location.Z, actor->LightRadius * 25.0f);

		const vec3 colour = SrgbToLinear(c.X, c.Y, c.Z);
		light.ColorBrightness = vec4(colour.x, colour.y, colour.z, actor->LightBrightness / 255.0f);

		Lights.push_back(light);
	}

	unguard;
}
