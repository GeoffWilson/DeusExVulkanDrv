#include "Precomp.h"
#include "Materials.h"

namespace
{
	struct Kind
	{
		const TCHAR* Name;
		float Roughness, Metalness, Reflectance;
	};

	// One row per group the game's texture packages use, and a few that only
	// come from names or actors. Rough enough to leave the diffuse picture the
	// game was lit for, with a sheen where a surface would have one: roughness
	// 0.8 and up is shaded as fully matte.
	//
	// Metal is only partly metallic. Deus Ex's metal textures are mostly
	// painted panels, grates and pipes, and at full metalness they read as
	// chrome, reflecting their own texture's dark colour.
	const Kind Kinds[] = {
		{ TEXT("Metal"),    0.40f, 0.70f, 0.04f },
		{ TEXT("Glass"),    0.05f, 0.00f, 0.04f },
		{ TEXT("Ceramic"),  0.25f, 0.00f, 0.04f },
		{ TEXT("Tiles"),    0.30f, 0.00f, 0.04f },
		{ TEXT("Stone"),    0.55f, 0.00f, 0.04f },
		{ TEXT("Wood"),     0.55f, 0.00f, 0.04f },
		{ TEXT("Water"),    0.05f, 0.00f, 0.02f },
		{ TEXT("Plastic"),  0.40f, 0.00f, 0.04f },
		{ TEXT("Leather"),  0.50f, 0.00f, 0.04f },
		{ TEXT("Flesh"),    0.65f, 0.00f, 0.03f },
		{ TEXT("Concrete"), 0.85f, 0.00f, 0.04f },
		{ TEXT("Brick"),    0.90f, 0.00f, 0.04f },
		{ TEXT("Stucco"),   0.90f, 0.00f, 0.04f },
		{ TEXT("Earth"),    1.00f, 0.00f, 0.04f },
		{ TEXT("Foliage"),  0.80f, 0.00f, 0.04f },
		{ TEXT("Textile"),  1.00f, 0.00f, 0.04f },
		{ TEXT("Paper"),    0.90f, 0.00f, 0.04f },
	};

	const Kind* FindKind(const TCHAR* name)
	{
		for (const Kind& kind : Kinds)
			if (!appStricmp(kind.Name, name))
				return &kind;
		return nullptr;
	}

	// Hints in a texture's own name, for mesh skins that sit in no telling
	// group. Checked in order, so the more specific come first.
	struct Hint
	{
		const TCHAR* Text;
		const TCHAR* Kind;
	};
	const Hint Hints[] = {
		{ TEXT("Leather"), TEXT("Leather") },
		{ TEXT("Marble"),  TEXT("Tiles") },
		{ TEXT("Chrome"),  TEXT("Metal") },
		{ TEXT("Steel"),   TEXT("Metal") },
		{ TEXT("Metal"),   TEXT("Metal") },
		{ TEXT("Wood"),    TEXT("Wood") },
		{ TEXT("Glass"),   TEXT("Glass") },
		{ TEXT("Plastic"), TEXT("Plastic") },
		{ TEXT("Tile"),    TEXT("Tiles") },
		{ TEXT("Fabric"),  TEXT("Textile") },
		{ TEXT("Cloth"),   TEXT("Textile") },
	};

	// What a decoration breaks into says what it is made of: DeusExDecoration's
	// FragType, read by name so the driver needs nothing from DeusEx.dll.
	struct Fragment
	{
		const TCHAR* Class;
		const TCHAR* Kind;
	};
	const Fragment Fragments[] = {
		{ TEXT("WoodFragment"),    TEXT("Wood") },
		{ TEXT("MetalFragment"),   TEXT("Metal") },
		{ TEXT("GlassFragment"),   TEXT("Glass") },
		{ TEXT("PlasticFragment"), TEXT("Plastic") },
		{ TEXT("PaperFragment"),   TEXT("Paper") },
		{ TEXT("FleshFragment"),   TEXT("Flesh") },
	};

	const TCHAR* Section = TEXT("PathTracerDrv.Materials");

	vec4 FromKind(const Kind& kind)
	{
		return vec4(kind.Roughness, kind.Metalness, kind.Reflectance, 0.0f);
	}

	// "roughness, metalness[, reflectance]" from the ini, over what the
	// material already was. False when the key is not there.
	bool FromIni(const TCHAR* key, vec4& material)
	{
		TCHAR value[256] = {};
		if (!GConfig || !GConfig->GetString(Section, key, value, ARRAY_COUNT(value)))
			return false;
		float parts[3] = { material.x, material.y, material.z };
		const TCHAR* p = value;
		for (int i = 0; i < 3 && *p; i++)
		{
			parts[i] = appAtof(p);
			while (*p && *p != ',')
				p++;
			if (*p == ',')
				p++;
		}
		material = vec4(Clamp(parts[0], 0.0f, 1.0f), Clamp(parts[1], 0.0f, 1.0f), Clamp(parts[2], 0.0f, 1.0f), 0.0f);
		return true;
	}

	// The group a texture was filed under, or its package when it has none.
	// A package named for its material - CoreTexMetal - is read as the group
	// it would have been.
	const TCHAR* GroupOf(UTexture* texture)
	{
		UObject* outer = texture->GetOuter();
		if (!outer)
			return TEXT("");
		const TCHAR* name = outer->GetName();
		if (!outer->GetOuter() && !appStrnicmp(name, TEXT("CoreTex"), 7))
			return name + 7;
		return name;
	}

	const TCHAR* FragmentOf(AActor* owner)
	{
		if (!owner)
			return nullptr;
		for (TFieldIterator<UObjectProperty> it(owner->GetClass()); it; ++it)
		{
			if (appStricmp(it->GetName(), TEXT("FragType")))
				continue;
			UObject* fragment = *(UObject**)((BYTE*)owner + it->Offset);
			if (!fragment)
				return nullptr;
			for (const Fragment& f : Fragments)
				if (!appStricmp(fragment->GetName(), f.Class))
					return f.Kind;
			return nullptr;
		}
		return nullptr;
	}
}

vec4 Materials::Matte()
{
	return vec4(1.0f, 0.0f, 0.04f, 0.0f);
}

vec4 Materials::For(UTexture* texture, AActor* owner, const TCHAR** name)
{
	if (name)
		*name = TEXT("matte");
	if (!texture)
		return Matte();

	const TCHAR* kindName = nullptr;
	const TCHAR* group = GroupOf(texture);
	if (FindKind(group))
		kindName = group;
	if (!kindName)
	{
		for (const Hint& hint : Hints)
		{
			if (appStrfind(texture->GetName(), hint.Text))
			{
				kindName = hint.Kind;
				break;
			}
		}
	}
	if (!kindName)
		kindName = FragmentOf(owner);

	vec4 material = Matte();
	if (kindName)
	{
		material = FromKind(*FindKind(kindName));
		if (name)
			*name = FindKind(kindName)->Name;

		// A group's line in the ini replaces the built in one.
		TCHAR key[128];
		appSprintf(key, TEXT("Group.%s"), kindName);
		FromIni(key, material);
	}

	// Polished stone is still stone to the footsteps.
	if (kindName && !appStricmp(kindName, TEXT("Stone")) && appStrfind(texture->GetName(), TEXT("Marble")))
		material.x = 0.25f;

	if (FromIni(texture->GetName(), material) && name)
		*name = TEXT("ini");
	return material;
}
