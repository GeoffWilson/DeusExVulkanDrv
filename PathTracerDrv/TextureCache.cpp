#include "Precomp.h"
#include "TextureCache.h"
#include "UPathTracerRenderDevice.h"

TextureCache::TextureCache(UPathTracerRenderDevice* renderer) : renderer(renderer)
{
}

TextureCache::~TextureCache()
{
	Clear();
}

void TextureCache::Clear()
{
	Textures.clear();
}

CachedTexture* TextureCache::Get(const FTextureInfo& info, bool masked)
{
	const uint64_t key = ((uint64_t)info.CacheID << 1) | (masked ? 1u : 0u);

	auto it = Textures.find(key);
	if (it != Textures.end())
		return it->second.get();

	auto cached = Upload(info, masked);
	CachedTexture* result = cached.get();
	Textures[key] = std::move(cached);
	return result;
}

// Expand a texture's top mip into RGBA8.
//
// Separated from the upload so that a texture which regenerates itself - fire,
// water, a computer screen - can be converted again into the image it already
// has, rather than being cached once at whatever frame it was first seen on.
bool TextureCache::ConvertPixels(const FTextureInfo& info, bool masked, std::vector<uint32_t>& pixels, int& width, int& height)
{
	guard(TextureCache::ConvertPixels);

	if (info.NumMips < 1 || !info.Mips[0] || !info.Mips[0]->DataPtr)
		return false;

	const FMipmapBase* mip = info.Mips[0];
	width = mip->USize;
	height = mip->VSize;
	if (width <= 0 || height <= 0)
		return false;

	pixels.assign((size_t)width * height, 0xffffffffu);

	switch (info.Format)
	{
	case TEXF_P8:
	{
		if (!info.Palette)
			return false;
		const BYTE* src = mip->DataPtr;
		for (size_t i = 0; i < pixels.size(); i++)
		{
			const BYTE index = src[i];
			const FColor& c = info.Palette[index];
			// Masked art uses palette entry zero as the hole. Everything else
			// is opaque; the engine's own alpha channel is not meaningful here.
			const uint32_t alpha = (masked && index == 0) ? 0u : 255u;
			pixels[i] = (alpha << 24) | ((uint32_t)c.B << 16) | ((uint32_t)c.G << 8) | (uint32_t)c.R;
		}

		break;
	}
	case TEXF_RGBA8:
	{
		const FColor* src = (const FColor*)mip->DataPtr;
		for (size_t i = 0; i < pixels.size(); i++)
			pixels[i] = ((uint32_t)src[i].A << 24) | ((uint32_t)src[i].B << 16) | ((uint32_t)src[i].G << 8) | (uint32_t)src[i].R;
		break;
	}
	case TEXF_RGB8:
	{
		const BYTE* src = mip->DataPtr;
		for (size_t i = 0; i < pixels.size(); i++)
			pixels[i] = 0xff000000u | ((uint32_t)src[i * 3 + 2] << 16) | ((uint32_t)src[i * 3 + 1] << 8) | (uint32_t)src[i * 3 + 0];
		break;
	}
	default:
		// Compressed and 16 bit formats are not handled yet. White rather than
		// nothing, so a tile that uses one is visible and obviously wrong
		// instead of silently missing.
		break;
	}

	// Masked art keeps the palette's colour in its transparent texels, and that
	// colour is usually black. Linear filtering then mixes it into every edge
	// and the sprite picks up a dark halo - the mouse cursor being the clearest
	// example, where the hole around the arrow is the whole tile. Spreading the
	// neighbouring opaque colour outwards leaves the alpha alone but gives the
	// filter something harmless to blend towards.
	if (masked)
	{
		std::vector<uint32_t> bled = pixels;
		for (int pass = 0; pass < 2; pass++)
		{
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					const size_t i = (size_t)y * width + x;
					if ((pixels[i] >> 24) != 0)
						continue;

					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							const int nx = x + dx, ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= width || ny >= height)
								continue;
							const size_t n = (size_t)ny * width + nx;
							if ((pixels[n] >> 24) == 0)
								continue;
							// Its colour, still fully transparent.
							bled[i] = pixels[n] & 0x00ffffffu;
							dy = dx = 2;
						}
					}
				}
			}
			pixels = bled;
		}
	}

	auto cached = std::make_unique<CachedTexture>();

	// Masked art that is not paletted has no index zero to key against. UE1's
	// convention there is a black colour key, and without it a masked texture in
	// one of these formats comes out fully opaque - the scope crosshair as a
	// black square with the cross inside it.
	//
	// Only when nothing in the image is already transparent, so a texture that
	// carries a real alpha channel keeps it.
	if (masked && info.Format != TEXF_P8)
	{
		bool anyTransparent = false;
		for (uint32_t px : pixels)
		{
			if ((px >> 24) != 255u)
			{
				anyTransparent = true;
				break;
			}
		}
		if (!anyTransparent)
		{
			for (uint32_t& px : pixels)
			{
				if ((px & 0x00ffffffu) == 0u)
					px = 0u;
			}
		}
	}

	return true;

	unguard;
}

std::unique_ptr<CachedTexture> TextureCache::Upload(const FTextureInfo& info, bool masked, bool withDescriptorSet)
{
	guard(TextureCache::Upload);

	std::vector<uint32_t> pixels;
	int width = 0, height = 0;
	if (!ConvertPixels(info, masked, pixels, width, height))
		return nullptr;

	auto cached = std::make_unique<CachedTexture>();

	cached->Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerTileTexture")
		.Create(renderer->GetDevice());

	cached->View = ImageViewBuilder()
		.Image(cached->Image.get(), VK_FORMAT_R8G8B8A8_UNORM)
		.DebugName("PathTracerTileTextureView")
		.Create(renderer->GetDevice());

	const size_t byteSize = pixels.size() * sizeof(uint32_t);
	auto staging = BufferBuilder()
		.Size(byteSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerTileStaging")
		.Create(renderer->GetDevice());

	void* mapped = staging->Map(0, byteSize);
	memcpy(mapped, pixels.data(), byteSize);
	staging->Unmap();

	VulkanImage* image = cached->Image.get();
	VulkanBuffer* src = staging.get();
	renderer->ExecuteImmediate([image, src, width, height](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
		cmd->copyBufferToImage(src->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	});

	// What actually landed in the image, so a texture that comes out the wrong
	// colour on screen can be compared against what the engine says it is,
	// rather than reasoned about.
	if (withDescriptorSet)
		cached->Set = renderer->AllocateTileDescriptorSet(cached->View.get());

	return cached;

	unguard;
}


// The top mip of a texture read straight off the object rather than through
// UTexture::Lock. Lock expects to be called while the engine is handing
// surfaces to a render device, and this runs at a different point entirely;
// it also takes an FTextureInfo that the engine partly reads, which as an
// uninitialised local was undefined behaviour.
static bool MipPixels(UTexture* texture, bool masked, std::vector<uint32_t>& pixels, int& width, int& height)
{
	if (!texture || texture->Mips.Num() < 1)
		return false;
	FMipmap& mip = texture->Mips(0);
	if (mip.USize <= 0 || mip.VSize <= 0 || mip.DataArray.Num() <= 0)
		return false;

	// The mip has to actually hold a full image. A lazy array that did not load,
	// or a format whose bytes per pixel is not one, would otherwise be read past
	// its end - which produces whatever palette entries happen to follow.
	if (texture->Format == TEXF_P8 && mip.DataArray.Num() < mip.USize * mip.VSize)
		return false;

	FTextureInfo info = {};
	info.Texture = texture;
	info.NumMips = 1;
	info.Mips[0] = &mip;
	info.Format = (ETextureFormat)texture->Format;
	info.USize = mip.USize;
	info.VSize = mip.VSize;
	info.Palette = (texture->Palette && texture->Palette->Colors.Num() > 0)
		? &texture->Palette->Colors(0) : nullptr;

	// Indexing the lazy array is what pulls it off disk if it is not resident.
	mip.DataPtr = &mip.DataArray(0);
	return TextureCache::ConvertPixels(info, masked, pixels, width, height);
}

bool TextureCache::ScenePixels(UTexture* texture, bool masked, std::vector<uint32_t>& pixels, int& width, int& height)
{
	guard(TextureCache::ScenePixels);
	return MipPixels(texture, masked, pixels, width, height);
	unguard;
}

// bParametric textures are generated rather than stored, and bRealtime ones
// change as they are drawn. AnimNext means a chain of textures cycled through
// in turn - how the engine animates a screen or a television - so the pixels
// live on a different object each frame rather than being regenerated in
// place.
bool TextureCache::Animates(UTexture* texture)
{
	return texture && (texture->bRealtime || texture->bParametric || texture->AnimNext != nullptr);
}

bool TextureCache::AnimatedPixels(UTexture* texture, bool masked, double time, int width, int height, UTexture*& lastFrame, std::vector<uint32_t>& pixels)
{
	guard(TextureCache::AnimatedPixels);

	if (!Animates(texture))
		return false;

	// Get advances the texture and hands back the frame to read. For one that
	// regenerates itself that is the texture again; for an animation chain it
	// is whichever link is current, which is why following only the base
	// object left every screen and television on its first frame.
	//
	// The engine does this from inside Lock, which this device bypasses, so
	// nothing was asking them to advance at all.
	//
	// A texture that regenerates itself is locked first, as the engine's own
	// renderer locks it before drawing. That is where a WetTexture locks the
	// texture it ripples, which is what makes that texture's pixels readable;
	// advancing it with Get alone left it rippling nothing, and the Dragon's
	// Tooth blade drew without its core. The Fire package locks with no render
	// device itself, so none is needed here either.
	const bool regenerates = texture->bRealtime || texture->bParametric;
	if (regenerates)
	{
		FTextureInfo locked = {};
		texture->Lock(locked, time, 0, nullptr);
		texture->Unlock(locked);
	}
	UTexture* frame = texture->Get(time);
	if (!frame)
		return false;

	// An animation chain only needs sending when it has actually moved on,
	// which for a screen running at a few frames a second is rarely. One that
	// regenerates in place has no such tell and is always read again.
	if (!regenerates && frame == lastFrame)
		return false;
	lastFrame = frame;

	int w = 0, h = 0;
	if (!MipPixels(frame, masked, pixels, w, h) || w != width || h != height)
		return false;
	return true;

	unguard;
}
