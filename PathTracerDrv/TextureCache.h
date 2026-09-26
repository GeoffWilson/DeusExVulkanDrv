#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <unordered_set>

class UPathTracerRenderDevice;

// One uploaded texture, plus the descriptor set that binds it. The set is made
// once with the image because a tile draw binds nothing else.
struct CachedTexture
{
	// The object this came from, kept only for textures that regenerate.
	UTexture* Source = nullptr;
	bool Realtime = false;
	int Width = 0;
	int Height = 0;
	bool Masked = false;
	// Which link of an animation chain is currently in the image.
	UTexture* LastFrame = nullptr;
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

	// Expand a texture's top mip into RGBA8.
	static bool ConvertPixels(const FTextureInfo& info, bool masked, std::vector<uint32_t>& pixels, int& width, int& height);

	// A texture the trace references, as RGBA8 pixels for the helper that
	// traces. Takes a UTexture rather than an FTextureInfo because the scene
	// walks the level's own objects rather than being handed surfaces by the
	// engine. Masked says whether palette entry zero is a hole: the engine
	// masks by the polygon's flags as well as the texture's, so one texture can
	// be needed both ways.
	static bool ScenePixels(UTexture* texture, bool masked, std::vector<uint32_t>& pixels, int& width, int& height);

	// Whether a texture changes by itself - fire, water, a computer screen, the
	// laser sight's dot, or a chain of frames cycled through.
	static bool Animates(UTexture* texture);

	// An animated texture's frame at time, as pixels of the size it was first
	// sent at. lastFrame is which link of an animation chain was last sent, so
	// one that has not moved on is not sent again. False when there is
	// nothing new.
	static bool AnimatedPixels(UTexture* texture, bool masked, double time, int width, int height, UTexture*& lastFrame, std::vector<uint32_t>& pixels);

	void Clear();

private:
	std::unique_ptr<CachedTexture> Upload(const FTextureInfo& info, bool masked, bool withDescriptorSet = true);


	UPathTracerRenderDevice* renderer = nullptr;

	// Keyed on the engine's cache id and whether it was wanted masked: the same
	// texture can be drawn both ways in one frame and the alpha differs.
	std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> Textures;

};
