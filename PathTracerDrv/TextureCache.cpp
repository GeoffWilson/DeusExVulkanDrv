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
	SceneTextures.clear();
	// The white pixel is deliberately kept: it belongs to no level and the
	// texture array's unused slots still have to point somewhere valid.
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


CachedTexture* TextureCache::GetForScene(UTexture* texture)
{
	guard(TextureCache::GetForScene);

	if (!texture)
		return nullptr;

	auto it = SceneTextures.find(texture);
	if (it != SceneTextures.end())
		return it->second.get();

	// Entered as null first so a texture that cannot be uploaded is not retried
	// on every frame that references it.
	SceneTextures[texture] = nullptr;

	if (texture->Mips.Num() < 1)
		return nullptr;

	// The mip data is read straight off the object rather than through
	// UTexture::Lock. Lock expects to be called while the engine is handing
	// surfaces to a render device, and this runs at a different point entirely;
	// it also takes an FTextureInfo that the engine partly reads, which as an
	// uninitialised local was undefined behaviour.
	FMipmap& mip = texture->Mips(0);
	if (mip.USize <= 0 || mip.VSize <= 0 || mip.DataArray.Num() <= 0)
		return nullptr;

	// The mip has to actually hold a full image. A lazy array that did not load,
	// or a format whose bytes per pixel is not one, would otherwise be read past
	// its end - which produces whatever palette entries happen to follow.
	if (texture->Format == TEXF_P8 && mip.DataArray.Num() < mip.USize * mip.VSize)
		return nullptr;

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

	auto uploaded = Upload(info, (texture->PolyFlags & PF_Masked) != 0, false);

	CachedTexture* result = uploaded.get();
	if (result)
	{
		result->Source = texture;
		// bParametric textures are generated rather than stored, and bRealtime
		// ones change as they are drawn. Either way the copy taken at the first
		// sighting is only ever right for that one frame.
		// AnimNext means a chain of textures cycled through in turn - how the
		// engine animates a screen or a television - so the pixels live on a
		// different object each frame rather than being regenerated in place.
		result->Realtime = texture->bRealtime || texture->bParametric || texture->AnimNext != nullptr;
		result->Width = mip.USize;
		result->Height = mip.VSize;
		result->Masked = (texture->PolyFlags & PF_Masked) != 0;
	}
	SceneTextures[texture] = std::move(uploaded);
	return result;

	unguard;
}

CachedTexture* TextureCache::White()
{
	if (WhitePixel)
		return WhitePixel.get();

	// Built by hand rather than uploaded: there is no engine texture behind it.
	FTextureInfo info = {};
	FMipmapBase mip;
	BYTE pixel[4] = { 255, 255, 255, 255 };
	mip.DataPtr = pixel;
	mip.USize = 1;
	mip.VSize = 1;
	info.NumMips = 1;
	info.Mips[0] = &mip;
	info.Format = TEXF_RGBA8;
	info.USize = 1;
	info.VSize = 1;

	WhitePixel = Upload(info, false, false);
	return WhitePixel.get();
}


void TextureCache::RefreshRealtime(double time, VulkanCommandBuffer* commands, std::vector<std::unique_ptr<VulkanBuffer>>& keepAlive, const std::unordered_set<UTexture*>& fixedFrames)
{
	guard(TextureCache::RefreshRealtime);

	std::vector<uint32_t> pixels;

	for (auto& entry : SceneTextures)
	{
		CachedTexture* cached = entry.second.get();
		if (!cached || !cached->Source || !cached->Image)
			continue;

		UTexture* texture = cached->Source;

		// One frame of an animation shown on its own - a sprite that plays once
		// chooses it - stays that frame. Advancing it would loop the animation.
		if (fixedFrames.count(texture))
			continue;

		// Asked afresh every frame rather than remembered from the upload, since
		// a script can give a texture an animation chain after it was first seen.
		const bool animates = texture->bRealtime || texture->bParametric || texture->AnimNext != nullptr;
		if (!animates)
			continue;
		cached->Realtime = true;

		// Get advances the texture and hands back the frame to read. For one
		// that regenerates itself that is the texture again; for an animation
		// chain it is whichever link is current, which is why following only the
		// base object left every screen and television on its first frame.
		//
		// The engine does this from inside Lock, which this device bypasses, so
		// nothing was asking them to advance at all.
		UTexture* frame = texture->Get(time);
		if (!frame || frame->Mips.Num() < 1)
			continue;

		// An animation chain only needs uploading when it has actually moved on,
		// which for a screen running at a few frames a second is rarely. One
		// that regenerates in place has no such tell and is always re-read.
		const bool regenerates = texture->bRealtime || texture->bParametric;
		if (!regenerates && frame == cached->LastFrame)
			continue;
		cached->LastFrame = frame;
		FMipmap& mip = frame->Mips(0);
		if (mip.USize != cached->Width || mip.VSize != cached->Height || mip.DataArray.Num() <= 0)
			continue;
		if (frame->Format == TEXF_P8 && mip.DataArray.Num() < mip.USize * mip.VSize)
			continue;

		FTextureInfo info = {};
		info.Texture = frame;
		info.NumMips = 1;
		info.Mips[0] = &mip;
		info.Format = (ETextureFormat)frame->Format;
		info.USize = mip.USize;
		info.VSize = mip.VSize;
		info.Palette = (frame->Palette && frame->Palette->Colors.Num() > 0)
			? &frame->Palette->Colors(0) : nullptr;
		mip.DataPtr = &mip.DataArray(0);

		int width = 0, height = 0;
		if (!ConvertPixels(info, cached->Masked, pixels, width, height))
			continue;

		const size_t byteSize = pixels.size() * sizeof(uint32_t);
		auto staging = BufferBuilder()
			.Size(byteSize)
			.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
			.DebugName("PathTracerRealtimeStaging")
			.Create(renderer->GetDevice());

		void* mapped = staging->Map(0, byteSize);
		memcpy(mapped, pixels.data(), byteSize);
		staging->Unmap();

		// Copied into the image that already exists rather than making a new
		// one, so the texture array's descriptors stay valid.
		VulkanImage* image = cached->Image.get();
		VulkanBuffer* src = staging.get();
		{
			VulkanCommandBuffer* cmd = commands;
			PipelineBarrier()
				.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
				.Execute(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

			VkBufferImageCopy region = {};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
			cmd->copyBufferToImage(src->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			PipelineBarrier()
				.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
				.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}

		keepAlive.push_back(std::move(staging));
	}

	unguard;
}
