#pragma once

#include "vec.h"

// What a surface is made of, as the trace shades it: how rough it is, how
// metallic, how much a non-metal reflects face on, and how deep its relief
// runs. Packed into a vec4 per texture, in the order the shader's material
// buffer holds them:
//   x roughness, 0 a mirror to 1 fully matte
//   y metalness, 0 to 1
//   z reflectance face on for the non-metal part (0.04 for most things) in
//     its fraction, and in its whole part the relief in eighths of a world
//     unit: how far the texture's brightest texel stands above its darkest,
//     for the bump mapping (reliefNormal in Shaders.cpp). The relief is none
//     where the texture's light and dark are not its shape.
//   w the texture's size, set by LevelScene
//
// Deus Ex has no material data for its renderer, but it does for its footsteps:
// every level texture sits in a group named for what it is - Metal, Wood,
// Stone, Textile - so the player sounds right walking on it. That is the main
// source here. Mesh skins have no such group, so they fall back on hints in the
// texture's name ("ChairLeatherTex1") and on what their actor breaks into when
// destroyed (a WoodFragment, a MetalFragment).
//
// Anything left unrecognised is fully matte, which shades exactly as every
// surface did before materials existed.
//
// Overrides live in the game's ini, in [PathTracerDrv.Materials]: a texture's
// own name, or "Group.<name>" for a whole group, set to "roughness, metalness"
// with an optional third value for the reflectance face on and a fourth for
// the relief in world units:
//   ChairLeatherTex1=0.35,0
//   Group.Metal=0.3,1
//   Group.Stone=0.55,0,0.04,4
// The ini is read as the game starts, and each texture is classified when a
// level first uses it.
namespace Materials
{
	vec4 Matte();

	// The z of a material: the reflectance and relief packed together, and
	// each taken back out of a material.
	float PackReflectance(float reflectance, float relief);
	float Reflectance(const vec4& material);
	float Relief(const vec4& material);

	// The material for a texture, and the actor it is being used on if it is a
	// mesh's skin. Named for the log, too, when name is given.
	vec4 For(UTexture* texture, AActor* owner, const TCHAR** name = nullptr);
}
