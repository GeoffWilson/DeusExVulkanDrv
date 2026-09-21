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

std::unique_ptr<CachedTexture> TextureCache::Upload(const FTextureInfo& info, bool masked)
{
	guard(TextureCache::Upload);

	if (info.NumMips < 1 || !info.Mips[0] || !info.Mips[0]->DataPtr)
		return nullptr;

	const FMipmapBase* mip = info.Mips[0];
	const int width = mip->USize;
	const int height = mip->VSize;
	if (width <= 0 || height <= 0)
		return nullptr;

	std::vector<uint32_t> pixels((size_t)width * height, 0xffffffffu);

	switch (info.Format)
	{
	case TEXF_P8:
	{
		if (!info.Palette)
			return nullptr;
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

	cached->Set = renderer->AllocateTileDescriptorSet(cached->View.get());

	return cached;

	unguard;
}
