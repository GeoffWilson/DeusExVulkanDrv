#pragma once

#include "vec.h"
#include "SceneData.h"
#include "EmitterGrid.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

class UPathTracerRenderDevice;

// Turns the engine's level into geometry, lights and placements.
//
// Deliberately not built from what the engine pushes at a render device:
// DrawComplexSurface only describes what survived frustum and BSP culling, and a
// path tracer needs the geometry behind the camera as much as in front of it.
// The level is read out of UModel instead, through FSceneNode::Level.
class LevelScene : public SceneData
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

	ULevel* SourceLevel = nullptr;
	int SourceNodeCount = 0;

	// A baked light's shadow mask on one of the level's surfaces at a point,
	// 0 to 1, as the engine filters it before lighting the surface: 1 where
	// the light is clear all round, 0 where the surface's lightmap does not
	// have the light at all. -1 when the surface has no lightmap. For PT LOOK.
	static float BakedMaskAt(UModel* model, INT iSurf, AActor* light, const FVector& point);

	// Called as a window of the HUD's draws its view: see the definition.
	void HideFromWindows();

private:
	static void AnimationPose(UMesh* mesh, FName sequence, FLOAT animFrame, int& frameA, int& frameB, float& alpha);
	int AnimatedGeometryFor(AActor* actor, UMesh* mesh, int frameA, int frameB, float alpha, UTexture* const skins[8], float styleKind = 0.0f, const FCoords* toLocal = nullptr);
	void AddBrushPolys(UModel* brush, SceneGeometry& out);
	void AddBspSurfaces(UModel* model, SceneGeometry& out, bool skipPortals);
	void SetDetail(TriangleAttributes& attr, UTexture* texture);
	void AddLight(AActor* actor);
	void AddFogLight(AActor* actor, const FPlane& colour, float brightness);
	static bool IsFlashlightBeam(AActor* actor);
	void PlaceFlashlight(AActor* beam);

	// A unit quad carrying one texture in one style, shared by every sprite
	// showing it. Its size and facing come from the instance.
	int GeometryForSprite(UTexture* texture, float kind);
	bool PlaceSprite(AActor* actor, int& geometryIndex, float transform[12]);

	// What was placed, for the log.
	struct PlaceCounts
	{
		int Brushes = 0, Meshes = 0, Animated = 0, Skipped = 0, Held = 0;
	};
	void PlaceActor(AActor* actor, uint32_t mask, bool iterated, PlaceCounts& counts);
	// Meshes placed this frame that could be a light's fitting - any mesh
	// but a pawn's - with their bounds, and where every light is, however
	// bright it is at the moment: see UnshadowFittings.
	struct FittingCandidate { size_t Instance; FBox Bounds; };
	std::vector<FittingCandidate> FittingCandidates;
	std::vector<FVector> LightPositions;
	void UnshadowFittings();
	void PlaceIterated(AActor* actor, uint32_t mask, PlaceCounts& counts);
	// A particle generator holds 64; this is only a guard against an iterator
	// that never says it is done.
	static const int MaxIteratedItems = 1024;

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

	// A mesh's triangles as its faces and wedges lay them out, which never
	// change: gathered the first time it is posed rather than for every pose.
	struct MeshTriangle
	{
		// The corners as the engine numbers vertices when it poses and
		// smooths the mesh, and as indices into the stored keyframes - not
		// the same for a mesh converted from the older format.
		INT Vertex[3];
		INT KeyVertex[3];
		// UE1 stores mesh texture coordinates as a byte per axis spanning the
		// whole texture, so they divide out to 0..1 rather than needing the
		// texture's size the way a BSP surface does.
		FMeshUV Tex[3];
		DWORD PolyFlags;
		INT TextureIndex;
	};
	struct MeshTriangles
	{
		std::vector<MeshTriangle> Triangles;
		INT VertexCount = 0;
	};
	std::unordered_map<UMesh*, MeshTriangles> MeshTriangleCache;
	const MeshTriangles& TrianglesOf(UMesh* mesh);

	// Scratch for posing a mesh, kept from one to the next rather than
	// allocated for each.
	struct PosedTriangle
	{
		vec3 Corners[3];
		vec3 Normal;            // unit
		bool Ok = false;        // posed, and with an area to have a normal
		bool Blended = false;
	};
	std::vector<FVector> PosePoints;
	std::vector<PosedTriangle> PosedTriangles;
	std::vector<vec3> VertexNormals;

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
	// The lights the level's build baked into its lightmaps, with a shadow
	// mask on each surface they reach, which the engine counts at twice the
	// brightness of a light without one. Numbered from 1, as the shader
	// finds them in a surface's list.
	std::unordered_map<AActor*, uint32_t> BakedLightIds;
	// Which actor each of this frame's instances places, where it is one.
	std::vector<std::pair<size_t, AActor*>> ActorInstances;
	// Each lightmapped surface of the level's, by its index, as its record's
	// number plus one; and the records, light lists and masks as they are
	// gathered, put together into Lightmaps once the level is built.
	std::unordered_map<INT, uint32_t> LightmapRecords;
	UModel* LightmappedModel = nullptr;
	std::vector<uint32_t> LightmapRecordWords;
	std::vector<uint32_t> LightmapLightWords;
	std::vector<uint8_t> LightmapMaskBytes;
	uint32_t AddLightmap(UModel* model, INT iSurf);
	void FinishLightmaps();
	// The level's glowing triangles, as they are found, and each one's place
	// among all the level's triangles before they are split into the two
	// static geometries: see BuildStatic.
	std::vector<EmitterSource> EmitterSources;
	std::vector<uint32_t> EmitterTriangles;
	std::unordered_map<AActor*, PlacedPose> PreviousPoses;
	std::unordered_map<AActor*, PlacedPose> CurrentPoses;
	// The first person weapon's last placement, which is not an actor's
	// placement in the level and so is not among the poses.
	bool HaveViewModelTransform = false;
	float ViewModelTransform[12] = {};

public:
	// How many distinct poses an animation is quantised into. Bounds the number
	// of structures at this many per mesh, at the cost of steppier movement.
	// One means a character is built once, exactly like a prop - which is the
	// only structural difference between the two, and props render.
	float LightScale = 1.0f;
	// The viewport's clock, which the engine pulses a glowing mesh by.
	double ViewportTime = 0.0;
	// A lit mesh's ambient as the engine gives it, in displayed terms.
	vec3 MeshAmbient(AActor* actor, AZoneInfo* zone) const;
	// Diagnostic: paint animated and specially shaped lights in bright,
	// obvious colours so they can be found.
	bool HighlightSpecialLights = false;
	// The light augmentation as a flashlight rather than its own two lights
	// (PlaceFlashlight), its brightness and how much of its beam the air
	// shows, 1 as designed. This frame's, as TraceCommand carries it, all
	// zero while it is off.
	bool UseFlashlight = true;
	float FlashlightBrightness = 1.0f;
	float FlashlightHaze = 1.0f;
	vec4 Flashlight[3] = {};
	bool StrobeOff = false;
	FLOAT LastStrobeTime = -1.0f;

	// Diagnostic: give characters a prop's geometry instead of their own.
	// Whose eyes this is being traced from. Set each frame from the scene node's
	// viewport, and used to apply the engine's owner visibility rules.
	AActor* ViewActor = nullptr;
	// The view is from behind the viewer rather than from its eyes - a third
	// person conversation - so its own body is drawn like anyone else's and
	// its first person weapon is not.
	bool ViewFromBehind = false;
	// Photo mode (the device's PT PHOTO): the view flies free, and the
	// viewer's body stays where it stood - its eyes at PhotoEye - shown as
	// anyone else's once the camera is away from them, with no first person
	// weapon at all.
	bool PhotoMode = false;
	FVector PhotoEye = FVector(0, 0, 0);

	// The view's own basis, as world space directions: X right, Y down,
	// Z forward, which is how the engine orients a scene node. Needed to place
	// the first person weapon, which lives in view space rather than in the
	// level.
	FVector ViewOrigin = FVector(0, 0, 0);
	FVector ViewRight = FVector(1, 0, 0);
	FVector ViewDown = FVector(0, 1, 0);
	FVector ViewForward = FVector(0, 0, 1);

	// The sky zone's viewpoint, if the level has one, and which way it faces:
	// the engine turns the view by the SkyZoneInfo's rotation before looking
	// into the skybox (see SkyAxes).
	bool HasSky = false;
	FVector SkyOrigin = FVector(0, 0, 0);
	FRotator SkyRotation = FRotator(0, 0, 0);

	// How many animated shapes were rebuilt this frame.
	int MeshBuilds = 0;
	// Where the gathering's time goes, summed until the device logs it:
	// lights, animated meshes, other actors, held weapons, particles,
	// fittings, decals, the view model, and of the animated meshes' time the
	// engine's own posing (GetFrame).
	static const int CollectStages = 9;
	double CollectStageMs[CollectStages] = {};
	void AddViewModel();
	bool PlaceHeldItem(APawn* pawn, uint32_t mask);
	// A character's posed points, reused from one to the next.
	std::vector<FVector> HeldPoints;

	// Every texture the scene references, in the order the shader's array binds
	// them. An index rather than a pointer travels into the attribute buffer.
	// The shader binds a fixed sized array; anything past it renders untextured
	// rather than wrong.
	static const int MaxTextures = 1024;
	std::vector<UTexture*> Textures;
	// Whether each entry is wanted with palette entry zero as a hole.
	std::vector<bool> TextureMasked;
	// What each entry is made of, as Materials packs it: the shader's material
	// buffer is indexed the same way as its texture array.
	std::vector<vec4> TextureMaterials;
	int MirroredCount() const { return MirroredSurfaces; }

	// Textures that are shown as one fixed frame of their animation - a sprite
	// that plays once picks its frame from how far through its life it is - so
	// must not be advanced the way a looping animation is.
	std::unordered_set<UTexture*> FixedFrames;
	// Masked by the texture's own flags, or by the caller's when the polygon
	// asks for it: the engine honours either. The owner is the actor a mesh
	// skin is being worn by, which can say what the skin is made of.
	int TextureFor(UTexture* texture, bool masked = false, AActor* owner = nullptr);

private:
};
