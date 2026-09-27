#include "Precomp.h"
#include "TextureCache.h"
#include "TraceProtocol.h"
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
	Changed.clear();
	Textures.clear();
}

CachedTexture* TextureCache::Get(const FTextureInfo& info, bool masked)
{
	const uint64_t key = ((uint64_t)info.CacheID << 1) | (masked ? 1u : 0u);

	auto it = Textures.find(key);
	if (it != Textures.end())
	{
		// One that draws itself says so as the engine ticks it: its new
		// picture is taken, once a frame, and the flag cleared, as the other
		// devices clear it once they have it.
		CachedTexture* cached = it->second.get();
		if (info.bRealtimeChanged && cached->ChangedFrame != Frame)
		{
			int width = 0, height = 0;
			if (ConvertPixels(info, masked, cached->NewPixels, width, height) && width == cached->Width && height == cached->Height)
			{
				cached->ChangedFrame = Frame;
				Changed.push_back(cached);
			}
			if (info.Texture)
				info.Texture->bRealtimeChanged = 0;
		}
		return cached;
	}

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
	case TEXF_DXT1:
	{
		// S3TC, as New Vision's textures are: 4x4 blocks of two RGB565
		// colours and two bits a texel choosing between them and the two
		// blended from them - or, when the first colour is the smaller, the
		// one halfway and a transparent black, which is how a masked texture
		// keeps its holes.
		const BYTE* src = mip->DataPtr;
		const int blocksWide = Max(1, (width + 3) / 4), blocksHigh = Max(1, (height + 3) / 4);
		auto expand = [](uint32_t c) -> uint32_t
		{
			const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
		};
		auto blend = [](uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t d) -> uint32_t
		{
			uint32_t out = 0;
			for (int k = 0; k < 3; k++)
				out |= ((((a >> (k * 8)) & 255u) * wa + ((b >> (k * 8)) & 255u) * wb) / d) << (k * 8);
			return out;
		};
		for (int by = 0; by < blocksHigh; by++)
		{
			for (int bx = 0; bx < blocksWide; bx++)
			{
				const BYTE* block = src + ((size_t)by * blocksWide + bx) * 8;
				const uint32_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
				const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | ((uint32_t)block[7] << 24);
				const uint32_t e0 = expand(c0), e1 = expand(c1);
				uint32_t colours[4];
				colours[0] = e0 | 0xff000000u;
				colours[1] = e1 | 0xff000000u;
				if (c0 > c1)
				{
					colours[2] = blend(e0, e1, 2, 1, 3) | 0xff000000u;
					colours[3] = blend(e0, e1, 1, 2, 3) | 0xff000000u;
				}
				else
				{
					colours[2] = blend(e0, e1, 1, 1, 2) | 0xff000000u;
					colours[3] = 0u;
				}
				for (int y = 0; y < 4; y++)
				{
					for (int x = 0; x < 4; x++)
					{
						const int px = bx * 4 + x, py = by * 4 + y;
						if (px < width && py < height)
							pixels[(size_t)py * width + px] = colours[(bits >> (2 * (y * 4 + x))) & 3u];
					}
				}
			}
		}
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
	//
	// Across the texture's edges too, where nothing beside a hole is opaque:
	// the scene's sampler repeats, so an edge is filtered with the opposite
	// one. Liberty Island's skyline is opaque down to its bottom row and a
	// hole along its top, and that row blended with the top's magenta key
	// drew a purple line under the city.
	if (masked)
	{
		std::vector<uint32_t> bled = pixels;
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				const size_t i = (size_t)y * width + x;
				if ((pixels[i] >> 24) != 0)
					continue;

				bool found = false;
				for (int across = 0; across < 2 && !found; across++)
				{
					for (int dy = -1; dy <= 1 && !found; dy++)
					{
						for (int dx = -1; dx <= 1 && !found; dx++)
						{
							int nx = x + dx, ny = y + dy;
							const bool outside = nx < 0 || ny < 0 || nx >= width || ny >= height;
							if (outside != (across == 1))
								continue;
							nx = (nx + width) % width;
							ny = (ny + height) % height;
							const size_t n = (size_t)ny * width + nx;
							if ((pixels[n] >> 24) == 0)
								continue;
							// Its colour, still fully transparent.
							bled[i] = pixels[n] & 0x00ffffffu;
							found = true;
						}
					}
				}
			}
		}
		pixels = bled;
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

std::unique_ptr<CachedTexture> TextureCache::Upload(const FTextureInfo& info, bool masked)
{
	guard(TextureCache::Upload);

	std::vector<uint32_t> pixels;
	int width = 0, height = 0;
	if (!ConvertPixels(info, masked, pixels, width, height))
		return nullptr;

	auto cached = std::make_unique<CachedTexture>();
	cached->Width = width;
	cached->Height = height;
	if (info.Texture)
		info.Texture->bRealtimeChanged = 0;

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

	return cached;

	unguard;
}


void TextureCache::RecordChanges(VulkanCommandBuffer* commands)
{
	for (CachedTexture* cached : Changed)
	{
		const size_t byteSize = cached->NewPixels.size() * sizeof(uint32_t);
		if (!cached->Staging)
		{
			cached->Staging = BufferBuilder()
				.Size(byteSize)
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("PathTracerTileChange")
				.Create(renderer->GetDevice());
		}
		void* mapped = cached->Staging->Map(0, byteSize);
		memcpy(mapped, cached->NewPixels.data(), byteSize);
		cached->Staging->Unmap();

		VulkanImage* image = cached->Image.get();
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { (uint32_t)cached->Width, (uint32_t)cached->Height, 1 };
		commands->copyBufferToImage(cached->Staging->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	Changed.clear();
}

// One mip of a texture read straight off the object rather than through
// UTexture::Lock. Lock expects to be called while the engine is handing
// surfaces to a render device, and this runs at a different point entirely;
// it also takes an FTextureInfo that the engine partly reads, which as an
// uninitialised local was undefined behaviour.
// A mip's data, off disk if it is not resident yet. This SDK's lazy arrays do
// not load themselves when indexed (LOAD_ON_DEMAND is off), and read as empty
// until something asks: the engine had loaded every top level by the time it
// was wanted here, but never the levels below, so every texture went to the
// helper without its mips.
//
// Through the engine's own vtable: the SDK declares TLazyArray<BYTE>'s
// functions imported, and neither import library exports Load, so a direct
// call cannot link. The pointer is volatile so the compiler, which knows the
// member's type, cannot call it directly anyway.
static void LoadMip(FMipmap& mip)
{
	FLazyLoader* volatile loader = &mip.DataArray;
	loader->Load();
}

// And back off again, for the S3TC sets, which nothing but the trace reads
// and which once in the helper are no use here: a level's worth held in a
// 32 bit process is hundreds of megabytes. The package keeps them on disk.
static void UnloadMips(TArray<FMipmap>& chain)
{
	for (INT i = 0; i < chain.Num(); i++)
	{
		FLazyLoader* volatile loader = &chain(i).DataArray;
		loader->Unload();
	}
}

// How many bytes a mip of this format and size holds.
static INT MipBytes(BYTE format, INT width, INT height)
{
	switch (format)
	{
	case TEXF_P8: return width * height;
	case TEXF_DXT1: return Max(1, (width + 3) / 4) * Max(1, (height + 3) / 4) * 8;
	case TEXF_RGB8: return width * height * 3;
	case TEXF_RGBA8: return width * height * 4;
	default: return 0;
	}
}

// The S3TC set of a texture that has one, which the trace uses when S3TC is
// on, as the other devices do: New Vision's packages keep each original and
// add a version at eight times the size, in S3TC, beside it (bHasComp and
// CompMips). A texture stored as S3TC outright has it in Mips.
static TArray<FMipmap>* CompressedChain(UTexture* texture)
{
	if (texture->bHasComp && texture->CompFormat == TEXF_DXT1 && texture->CompMips.Num() > 0)
		return &texture->CompMips;
	if (texture->Format == TEXF_DXT1 && texture->Mips.Num() > 0)
		return &texture->Mips;
	return nullptr;
}

static bool MipPixels(UTexture* texture, TArray<FMipmap>& chain, BYTE format, INT level, bool masked, std::vector<uint32_t>& pixels, int& width, int& height)
{
	if (!texture || chain.Num() <= level)
		return false;
	FMipmap& mip = chain(level);
	LoadMip(mip);
	if (mip.USize <= 0 || mip.VSize <= 0 || mip.DataArray.Num() <= 0)
		return false;

	// The mip has to actually hold a full image. A lazy array that did not load
	// would otherwise be read past its end - which produces whatever palette
	// entries happen to follow.
	if (mip.DataArray.Num() < MipBytes(format, mip.USize, mip.VSize))
		return false;

	FTextureInfo info = {};
	info.Texture = texture;
	info.NumMips = 1;
	info.Mips[0] = &mip;
	info.Format = (ETextureFormat)format;
	info.USize = mip.USize;
	info.VSize = mip.VSize;
	info.Palette = (texture->Palette && texture->Palette->Colors.Num() > 0)
		? &texture->Palette->Colors(0) : nullptr;

	mip.DataPtr = &mip.DataArray(0);
	return TextureCache::ConvertPixels(info, masked, pixels, width, height);
}

bool TextureCache::SceneMips(UTexture* texture, bool masked, bool s3tc, std::vector<uint32_t>& pixels, int& width, int& height, int& levels, uint32_t& format)
{
	guard(TextureCache::SceneMips);
	levels = 0;
	format = TraceProtocol::TextureRgba8;
	if (!texture)
		return false;

	TArray<FMipmap>* chain = &texture->Mips;
	BYTE chainFormat = texture->Format;
	if (TArray<FMipmap>* compressed = s3tc ? CompressedChain(texture) : nullptr)
	{
		chain = compressed;
		chainFormat = TEXF_DXT1;

		// Handed over as it is, blocks and all: the GPU reads S3TC itself, and
		// New Vision's textures unpacked would be eight times the size - a
		// level's worth runs to gigabytes. Not a masked one, whose holes are
		// looked for by colour when they are not in the blocks' alpha, and
		// whose edges want the colour spread into them (ConvertPixels).
		if (!masked)
		{
			pixels.clear();
			for (INT i = 0; i < chain->Num(); i++)
			{
				FMipmap& mip = (*chain)(i);
				LoadMip(mip);
				if (i == 0)
				{
					width = mip.USize;
					height = mip.VSize;
				}
				const INT bytes = MipBytes(TEXF_DXT1, mip.USize, mip.VSize);
				if (mip.USize != Max(1, width >> i) || mip.VSize != Max(1, height >> i) || mip.DataArray.Num() < bytes || width <= 0 || height <= 0)
					break;
				const size_t start = pixels.size();
				pixels.resize(start + bytes / 4);
				appMemcpy(&pixels[start], &mip.DataArray(0), bytes);
				levels++;
			}
			if (chain == &texture->CompMips)
				UnloadMips(*chain);
			if (levels > 0)
			{
				format = TraceProtocol::TextureBc1;
				return true;
			}
		}
	}

	const bool converted = MipPixels(texture, *chain, chainFormat, 0, masked, pixels, width, height);
	if (converted)
	{
		levels = 1;
		std::vector<uint32_t> level;
		for (INT i = 1; i < chain->Num(); i++)
		{
			int w = 0, h = 0;
			if (!MipPixels(texture, *chain, chainFormat, i, masked, level, w, h) || w != Max(1, width >> i) || h != Max(1, height >> i))
				break;
			pixels.insert(pixels.end(), level.begin(), level.end());
			levels++;
		}
	}
	if (chain == &texture->CompMips)
		UnloadMips(*chain);
	return converted;
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

// How fast an animation chain runs when the frame a surface starts it on sets
// no speed of its own. The engine steps such a chain once every frame it
// renders, so it ran two to four times as fast here, at 80 to 120 frames a
// second, as in a rasteriser at 30 to 50 - and a level can start a chain
// partway along: the radar screens use radar_A02 of a chain whose radar_A00
// sets 8 frames a second. So the speed any frame of the chain sets, after it
// or leading into it, or 30 frames a second where none does.
static float ChainRate(UTexture* texture)
{
	static std::unordered_map<UTexture*, std::pair<FName, float>> rates;
	auto found = rates.find(texture);
	if (found != rates.end() && found->second.first == texture->GetFName())
		return found->second.second;

	float rate = 0.0f;
	std::unordered_set<UTexture*> chain;
	for (UTexture* t = texture; t && !chain.count(t) && chain.size() < 1000; t = t->AnimNext)
	{
		chain.insert(t);
		if (rate <= 0.0f && t->MaxFrameRate > 0.0f)
			rate = t->MaxFrameRate;
	}
	for (bool grew = true; grew && rate <= 0.0f; )
	{
		grew = false;
		for (TObjectIterator<UTexture> it; it && rate <= 0.0f; ++it)
			if (it->AnimNext && chain.count(it->AnimNext) && !chain.count(*it))
			{
				chain.insert(*it);
				grew = true;
				if (it->MaxFrameRate > 0.0f)
					rate = it->MaxFrameRate;
			}
	}
	if (rate <= 0.0f)
		rate = 30.0f;
	rates[texture] = { texture->GetFName(), rate };
	return rate;
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
	// A chain that sets its own speed is left to the engine, which paces it by
	// the clock. One that does not is stepped here by the clock at the speed
	// ChainRate finds, round the frames the engine steps it round: on along
	// the chain from the one the surface uses, and back to it at the end.
	UTexture* frame = nullptr;
	if (!regenerates && texture->AnimNext && texture->MaxFrameRate <= 0.0f)
	{
		int length = 1;
		for (UTexture* t = texture->AnimNext; t && t != texture && length < 1000; t = t->AnimNext)
			length++;
		const long long step = (long long)std::floor(time * ChainRate(texture));
		int index = (int)(step % length);
		frame = texture;
		for (int i = 0; i < index; i++)
			frame = frame->AnimNext ? frame->AnimNext : texture;
	}
	else
		frame = texture->Get(time);
	if (!frame)
		return false;

	// An animation chain only needs sending when it has actually moved on,
	// which for a screen running at a few frames a second is rarely. One that
	// regenerates in place has no such tell and is always read again.
	if (!regenerates && frame == lastFrame)
		return false;
	lastFrame = frame;

	int w = 0, h = 0;
	if (!MipPixels(frame, frame->Mips, frame->Format, 0, masked, pixels, w, h) || w != width || h != height)
		return false;
	return true;

	unguard;
}
