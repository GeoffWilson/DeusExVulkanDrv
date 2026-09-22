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

	// The same upload for a texture the trace references. Takes a UTexture
	// rather than an FTextureInfo because the scene walks the level's own
	// objects rather than being handed surfaces by the engine, and skips the
	// per-texture descriptor set: the trace binds one array, not one set each.
	// Masked says whether palette entry zero is a hole. The engine masks by the
	// polygon's flags as well as the texture's, so one texture can be needed
	// both ways.
	CachedTexture* GetForScene(UTexture* texture, bool masked);

	// A 1x1 white image, so that unused slots in the trace's texture array are
	// still valid descriptors.
	CachedTexture* White();

	// Regenerate the textures that change by themselves - fire, water, a
	// computer screen, the laser sight's dot - and copy them into the images
	// they already have, so nothing that points at those images has to change.
	// Recorded into the frame's own command buffer rather than submitted one
	// texture at a time: a submit-and-wait each is a stall each, and a room of
	// screens cost most of the frame rate. The staging buffers must outlive the
	// submission, so they are handed back to be released once it completes.
	void RefreshRealtime(double time, VulkanCommandBuffer* commands, std::vector<std::unique_ptr<VulkanBuffer>>& keepAlive, const std::unordered_set<UTexture*>& fixedFrames);

	void Clear();

private:
	std::unique_ptr<CachedTexture> Upload(const FTextureInfo& info, bool masked, bool withDescriptorSet = true);
	static bool ConvertPixels(const FTextureInfo& info, bool masked, std::vector<uint32_t>& pixels, int& width, int& height);

	UPathTracerRenderDevice* renderer = nullptr;

	// Keyed on the engine's cache id and whether it was wanted masked: the same
	// texture can be drawn both ways in one frame and the alpha differs.
	std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> Textures;

	// Keyed on the object itself: the scene refers to textures by pointer, and
	// the same texture is wanted once however many surfaces use it.
	std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> SceneTextures;
	std::unique_ptr<CachedTexture> WhitePixel;
};
