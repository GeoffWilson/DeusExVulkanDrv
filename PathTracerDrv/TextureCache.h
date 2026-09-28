#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <unordered_set>

class UPathTracerRenderDevice;

// One uploaded texture, plus the descriptor sets that bind it: one for each way
// a tile can ask for it to be sampled (see UPathTracerRenderDevice::TileSet),
// made the first time one does, since a tile draw binds nothing else.
struct CachedTexture
{
	// The object this came from, kept only for textures that regenerate.
	UTexture* Source = nullptr;
	bool Realtime = false;
	int Width = 0;
	int Height = 0;
	bool Masked = false;
	bool PaletteAlpha = false;
	// Which link of an animation chain is currently in the image.
	UTexture* LastFrame = nullptr;
	// A texture that draws itself - fire, the static the vision augmentation
	// dresses people in - with its new picture, waiting to go in before the
	// tiles are drawn (TextureCache::RecordChanges), the frame it was taken
	// on, and where it is staged.
	std::vector<uint32_t> NewPixels;
	uint32_t ChangedFrame = 0;
	std::unique_ptr<VulkanBuffer> Staging;
	std::unique_ptr<VulkanImage> Image;
	std::unique_ptr<VulkanImageView> View;
	std::unique_ptr<VulkanDescriptorSet> Sets[4];
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
	// PaletteAlpha, for 469's PF_Highlighted art, keeps a palette's own alpha
	// and leaves the colour as the engine premultiplied it.
	CachedTexture* Get(const FTextureInfo& info, bool masked, bool paletteAlpha = false);

	// Expand a texture's top mip into RGBA8.
	static bool ConvertPixels(const FTextureInfo& info, bool masked, std::vector<uint32_t>& pixels, int& width, int& height, bool paletteAlpha = false);

	// A texture the trace references, as RGBA8 pixels for the helper that
	// traces. Takes a UTexture rather than an FTextureInfo because the scene
	// walks the level's own objects rather than being handed surfaces by the
	// engine. Masked says whether palette entry zero is a hole: the engine
	// masks by the polygon's flags as well as the texture's, so one texture can
	// be needed both ways.
	//
	// With its mips as the package stores them - the levels the other
	// devices upload - end to end after it, top first; levels says how many,
	// stopping at the first that is missing or is not half the one above.
	// With s3tc, a texture that has an S3TC set comes as that instead, still
	// in its blocks unless it is masked; format says which
	// (TraceProtocol::TextureRgba8 or TextureBc1).
	static bool SceneMips(UTexture* texture, bool masked, bool s3tc, std::vector<uint32_t>& pixels, int& width, int& height, int& levels, uint32_t& format);

	// Whether a texture changes by itself - fire, water, a computer screen, the
	// laser sight's dot, or a chain of frames cycled through.
	static bool Animates(UTexture* texture);

	// An animated texture's frame at time, as pixels of the size it was first
	// sent at. lastFrame is which link of an animation chain was last sent, so
	// one that has not moved on is not sent again. False when there is
	// nothing new.
	static bool AnimatedPixels(UTexture* texture, bool masked, double time, int width, int height, UTexture*& lastFrame, std::vector<uint32_t>& pixels);

	void Clear();

	// A new frame: a texture that changes is taken again at most once a frame.
	void BeginFrame() { Frame++; }
	// The frame's changed textures copied in, once the frame before is done
	// drawing with them.
	void RecordChanges(VulkanCommandBuffer* commands);

private:
	uint32_t Frame = 1;
	std::vector<CachedTexture*> Changed;
	std::unique_ptr<CachedTexture> Upload(const FTextureInfo& info, bool masked, bool paletteAlpha);


	UPathTracerRenderDevice* renderer = nullptr;

	// Keyed on the engine's cache id and whether it was wanted masked or with
	// the palette's alpha: the same texture can be drawn more than one way in
	// a frame and the alpha differs.
	std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> Textures;

};
