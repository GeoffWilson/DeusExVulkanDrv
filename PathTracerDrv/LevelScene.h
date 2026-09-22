#pragma once

#include "vec.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

class UPathTracerRenderDevice;

// One triangle's worth of shading data, indexed in the trace shader by the
// instance's custom index plus the primitive index. Normals are in object space
// and rotated into world space by the shader using the instance transform, so
// the same geometry can be instanced at any orientation.
struct TriangleAttributes
{
	vec4 Normal;
	vec4 Albedo;
	vec4 Emission;
	// The zone's ambient light. A property of the surface for level geometry,
	// which is why it lives here rather than only per instance: one room can be
	// lit and the next pitch dark, and the engine's own lighting says so.
	vec4 Ambient;
	// Texture coordinates at the three corners, and which texture they index.
	// Interpolated in the shader from the hit's barycentrics. Texture is stored
	// as a float purely to keep the record to whole vec4s; it is an integer
	// index into the bound texture array, or -1 for an untextured surface.
	vec4 UV01;      // u0 v0 u1 v1
	vec4 UV2Tex;    // u2 v2 texture unused
};

// A light as the engine describes it, converted to something physical.
struct SceneLight
{
	vec4 PositionRadius;
	vec4 ColorBrightness;
	// xyz which way a spotlight points; w the cosine of its cone's edge, or
	// -1 for a light that shines every way.
	vec4 DirectionCone;
	// x no incidence falloff, y distance measured horizontally (a cylinder),
	// z brightness changes from frame to frame, w zero for a disco light and
	// -1 for any other.
	vec4 Flags;
};

// Triangles that share a bottom level acceleration structure.
struct SceneGeometry
{
	// Whether anything in here needs its texture's alpha consulted before a hit
	// counts. Geometry without it can be marked opaque, which lets traversal
	// accept a hit without ever calling back into the shader.
	bool HasMasked = false;
	// Rebuilt every frame, so its acceleration structure has to be refitted
	// rather than built once.
	bool Dynamic = false;
	// Bumped whenever a dynamic geometry is rebuilt, so the device uploads and
	// rebuilds only what changed. An animating character changes every frame;
	// the decals change only when one is added or removed.
	uint32_t Version = 1;
	std::vector<vec3> Positions;
	std::vector<TriangleAttributes> Attributes;
};

// One placement of a geometry in the world, rebuilt every frame for anything
// that moves.
struct SceneInstance
{
	int GeometryIndex = 0;
	uint32_t AttributeBase = 0;
	float Transform[12] = {};   // 3x4, row major, as Vulkan wants it
	// An actor carries its zone's ambient with it, because the same mesh is
	// instanced in rooms with different lighting.
	vec4 Ambient = vec4(0.0f, 0.0f, 0.0f, 0.0f);
};

// Turns the engine's level into geometry, lights and placements.
//
// Deliberately not built from what the engine pushes at a render device:
// DrawComplexSurface only describes what survived frustum and BSP culling, and a
// path tracer needs the geometry behind the camera as much as in front of it.
// The level is read out of UModel instead, through FSceneNode::Level.
class LevelScene
{
public:
	// The static world. Returns false if there was nothing to build from.
	bool BuildStatic(ULevel* level);

	// The things that move: mover brushes and mesh actors. Called every frame.
	// Geometry is built once per distinct shape and cached; only the placements
	// change from frame to frame.
	void CollectDynamic(ULevel* level);

	void Clear();

	bool IsEmpty() const { return Geometries.empty(); }

	std::vector<SceneGeometry> Geometries;
	// How many of the first geometries are the level itself: the opaque part
	// and the part whose surfaces the shader has to judge.
	int StaticGeometries = 0;
	std::vector<SceneLight> Lights;

	// Rebuilt each frame. The first entry is always the static world.
	std::vector<SceneInstance> Instances;

	// Set when CollectDynamic added geometry, so the device knows the bottom
	// level structures and the attribute buffer need extending.
	bool GeometryAdded = false;

	ULevel* SourceLevel = nullptr;
	int SourceNodeCount = 0;

private:
	static void AnimationPose(UMesh* mesh, FName sequence, FLOAT animFrame, int& frameA, int& frameB, float& alpha);
	int AnimatedGeometryFor(AActor* actor, UMesh* mesh, int frameA, int frameB, float alpha, UTexture* const skins[8], float styleKind = 0.0f, const FCoords* toLocal = nullptr);
	void AddBrushPolys(UModel* brush, SceneGeometry& out);
	void AddBspSurfaces(UModel* model, SceneGeometry& out, bool skipPortals);
	void AddLight(AActor* actor);

	// A unit quad carrying one texture in one style, shared by every sprite
	// showing it. Its size and facing come from the instance.
	int GeometryForSprite(UTexture* texture, float kind);
	bool PlaceSprite(AActor* actor, int& geometryIndex, float transform[12]);

	// Decals - bullet holes, blood, scorch marks - which the engine attaches to
	// level surfaces while the game runs, so a world built once at load never
	// had them. Gathered into one geometry, rebuilt when the set changes.
	void CollectDecals(ULevel* level);
	int DecalGeometry = -1;
	uint64_t DecalSignature = 0;
	// Room reserved up front: a geometry's shading data cannot grow after it
	// is first built.
	static const int MaxDecals = 1024;
	std::unordered_map<uint64_t, int> SpriteGeometry;

	// Geometry index for a mover's brush, built on first sight.
	int GeometryForBrush(UModel* brush);
	// Geometry index for a mesh at a particular animation frame and skin set.
	// The skins are part of the key: Deus Ex puts a character's appearance on
	// the actor rather than the mesh, so two people sharing a mesh are only the
	// same shape if they are also wearing the same thing.
	int GeometryForMesh(UMesh* mesh, int frameA, int frameB, float alpha, UTexture* const skins[8], int reuseIndex = -1, float styleKind = 0.0f, AActor* owner = nullptr, const FCoords* toLocal = nullptr, AActor* envSource = nullptr);

	// One shot diagnostics, reset per level.
	int MeshesLogged = 0;
	bool SummaryLogged = false;

	std::unordered_map<void*, int> BrushGeometry;
	std::unordered_map<uint64_t, int> MeshGeometry;

	// One geometry per animated actor, rebuilt each frame at its exact pose.
	std::unordered_map<AActor*, int> ActorGeometry;
	// What each animated actor's shape was last built from, so an unchanged
	// pose is not rebuilt.
	std::unordered_map<AActor*, uint64_t> ActorPoseKeys;

	// A ceiling on how many poses are kept. Each one is a bottom level
	// structure, and a level with many characters could otherwise build them
	// without limit.
	static const int MaxMeshGeometries = 2048;

	std::unordered_map<uint64_t, int> TextureIndex;

	// What each actor's placement was last frame, so that an instance which has
	// actually moved can be told apart from one that merely looks different.
	// Two maps swapped each frame rather than one that grows for ever.
	struct PlacedPose
	{
		int GeometryIndex = -1;
		float Transform[12] = {};
		bool operator!=(const PlacedPose& other) const
		{
			if (GeometryIndex != other.GeometryIndex)
				return true;
			for (int i = 0; i < 12; i++)
				if (Transform[i] != other.Transform[i])
					return true;
			return false;
		}
	};
	int MirroredSurfaces = 0;
	std::unordered_map<AActor*, PlacedPose> PreviousPoses;
	std::unordered_map<AActor*, PlacedPose> CurrentPoses;

public:
	// How many distinct poses an animation is quantised into. Bounds the number
	// of structures at this many per mesh, at the cost of steppier movement.
	// One means a character is built once, exactly like a prop - which is the
	// only structural difference between the two, and props render.
	float LightScale = 1.0f;
	// Diagnostic: paint animated and specially shaped lights in bright,
	// obvious colours so they can be found.
	bool HighlightSpecialLights = false;
	bool StrobeOff = false;
	FLOAT LastStrobeTime = -1.0f;

	// Diagnostic: give characters a prop's geometry instead of their own.
	// Whose eyes this is being traced from. Set each frame from the scene node's
	// viewport, and used to apply the engine's owner visibility rules.
	AActor* ViewActor = nullptr;

	// The view's own basis, as world space directions: X right, Y down,
	// Z forward, which is how the engine orients a scene node. Needed to place
	// the first person weapon, which lives in view space rather than in the
	// level.
	FVector ViewOrigin = FVector(0, 0, 0);
	FVector ViewRight = FVector(1, 0, 0);
	FVector ViewDown = FVector(0, 1, 0);
	FVector ViewForward = FVector(0, 0, 1);

	// The sky zone's viewpoint, if the level has one.
	bool HasSky = false;
	FVector SkyOrigin = FVector(0, 0, 0);

	// How many animated shapes were rebuilt this frame.
	int MeshBuilds = 0;
	void AddViewModel();

	// Every texture the scene references, in the order the shader's array binds
	// them. An index rather than a pointer travels into the attribute buffer.
	// The shader binds a fixed sized array; anything past it renders untextured
	// rather than wrong.
	static const int MaxTextures = 1024;
	std::vector<UTexture*> Textures;
	// Whether each entry is wanted with palette entry zero as a hole.
	std::vector<bool> TextureMasked;
	int MirroredCount() const { return MirroredSurfaces; }

	// Textures that are shown as one fixed frame of their animation - a sprite
	// that plays once picks its frame from how far through its life it is - so
	// must not be advanced the way a looping animation is.
	std::unordered_set<UTexture*> FixedFrames;
	// Masked by the texture's own flags, or by the caller's when the polygon
	// asks for it: the engine honours either.
	int TextureFor(UTexture* texture, bool masked = false);

private:
};
