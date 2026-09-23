#pragma once

#include "vec.h"

// What a surface is made of, as the trace shades it: how rough it is, how
// metallic, and how much a non-metal reflects face on. Packed into a vec4 per
// texture, in the order the shader's material buffer holds them:
//   x roughness, 0 a mirror to 1 fully matte
//   y metalness, 0 to 1
//   z reflectance face on for the non-metal part (0.04 for most things)
//   w unused
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
// with an optional third value for the reflectance face on:
//   ChairLeatherTex1=0.35,0
//   Group.Metal=0.3,1
// The ini is read as the game starts, and each texture is classified when a
// level first uses it.
namespace Materials
{
	vec4 Matte();

	// The material for a texture, and the actor it is being used on if it is a
	// mesh's skin. Named for the log, too, when name is given.
	vec4 For(UTexture* texture, AActor* owner, const TCHAR** name = nullptr);
}
