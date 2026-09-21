#pragma once

#include <memory>
#include <unordered_map>

class UPathTracerRenderDevice;

// One uploaded texture, plus the descriptor set that binds it. The set is made
// once with the image because a tile draw binds nothing else.
struct CachedTexture
{
	std::unique_ptr<VulkanImage> Image;
	std::unique_ptr<VulkanImageView> View;
	std::unique_ptr<VulkanDescriptorSet> Set;
};

// Uploads the engine's textures for the 2D pass.
//
// UE1 textures are mostly 8 bit paletted, which no GPU has sampled natively for
// twenty years, so they are expanded to RGBA8 on the way through. Index zero is
// the transparent one for masked art - the HUD and the fonts are almost entirely
// masked, so getting that wrong shows up immediately as black boxes behind every
// letter.
class TextureCache
{
public:
	TextureCache(UPathTracerRenderDevice* renderer);
	~TextureCache();

	// Returns null if the texture could not be represented.
	CachedTexture* Get(const FTextureInfo& info, bool masked);

	void Clear();

private:
	std::unique_ptr<CachedTexture> Upload(const FTextureInfo& info, bool masked);

	UPathTracerRenderDevice* renderer = nullptr;

	// Keyed on the engine's cache id and whether it was wanted masked: the same
	// texture can be drawn both ways in one frame and the alpha differs.
	std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> Textures;
};
