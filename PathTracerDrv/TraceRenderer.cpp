#include "TracePrecomp.h"
#include "TraceRenderer.h"
#include "TraceProtocol.h"
#include "AccelStructure.h"
#include "Denoiser.h"
#include "FrameUploads.h"
#include "RayReconstruction.h"
#include "Shaders.h"
#include <stdexcept>

// Matte: roughness 1, no metal, the usual reflectance. See Materials.h.
static const vec4 MatteMaterial(1.0f, 0.0f, 0.04f, 0.0f);

// A new accumulation and history, cleared and left GENERAL for the trace. Not
// merely made GENERAL, as everything else is: the accumulation's alpha counts
// a pixel's frames under a moving shadow, and the trace reads that before it
// looks at whether the device has thrown the history away. Whatever the
// memory last held could keep a pixel on a short history until the next
// resize, or off one altogether if it read as NaN.
static void StartCleared(VulkanCommandBuffer* cmd, VulkanImage* accum, VulkanImage* history)
{
	PipelineBarrier()
		.AddImage(accum, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
		.AddImage(history, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
		.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	const VkClearColorValue zero = {};
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	cmd->clearColorImage(accum->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	cmd->clearColorImage(history->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	PipelineBarrier()
		.AddImage(accum, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		.AddImage(history, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

TraceRenderer::TraceRenderer(GpuContext* context) : Context(context), Device(context->GetDevice())
{
	// Texturing needs to index the array by whatever each ray hit, which is
	// not uniform across a workgroup. Without it the trace still runs, with
	// one averaged colour per surface.
	CanSampleTextures =
		Device->EnabledFeatures.DescriptorIndexing.runtimeDescriptorArray &&
		Device->EnabledFeatures.DescriptorIndexing.shaderSampledImageArrayNonUniformIndexing;

	SceneSampler = SamplerBuilder()
		.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT)
		.DebugName("PathTracerSceneSampler")
		.Create(Device);

	// The white every unused slot points at.
	WhiteImage = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Size(1, 1)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerWhite")
		.Create(Device);
	WhiteView = ImageViewBuilder().Image(WhiteImage.get(), VK_FORMAT_R8G8B8A8_UNORM).DebugName("PathTracerWhiteView").Create(Device);
	{
		const uint32_t white = 0xffffffffu;
		auto staging = BufferBuilder().Size(4).Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY).DebugName("PathTracerWhiteStaging").Create(Device);
		memcpy(staging->Map(0, 4), &white, 4);
		staging->Unmap();
		VulkanImage* image = WhiteImage.get();
		VulkanBuffer* src = staging.get();
		Context->ExecuteImmediate([image, src](VulkanCommandBuffer* cmd)
		{
			PipelineBarrier()
				.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
				.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = { 1, 1, 1 };
			cmd->copyBufferToImage(src->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			PipelineBarrier()
				.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
				.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		});
	}

	Accel.reset(new AccelStructure(Context));
	for (auto& uploads : Uploads)
		uploads.reset(new FrameUploads(Device));
	CreateTracePipeline();
	CreateCompositePipeline();
	CreateFinishPipeline();
	Slots.resize(MaxTextures);
	ResetScene();
}

TraceRenderer::~TraceRenderer()
{
	vkDeviceWaitIdle(Device->device);
}

void TraceRenderer::CreateTracePipeline()
{
	DescriptorLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(19, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(21, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(22, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(23, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(24, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerSetLayout")
		.Create(Device);

	ViewLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerViewSetLayout")
		.Create(Device);

	// Set 0 once for each view, and set 1 for each view and each window.
	const int imageSets = MaxViews + (int)TraceProtocol::MaxInsets;
	DescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MaxViews)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MaxViews * (2 + GuideImageCount) + 3 * imageSets)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MaxViews * 9)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxViews * MaxTextures)
		.MaxSets(MaxViews + imageSets)
		.DebugName("PathTracerDescriptorPool")
		.Create(Device);

	for (View& view : Views)
	{
		view.SceneSet = DescriptorPool->allocate(DescriptorLayout.get());
		view.ImageSet = DescriptorPool->allocate(ViewLayout.get());
	}
	for (Inset& inset : Insets)
		inset.Set = DescriptorPool->allocate(ViewLayout.get());

	MaterialBuffer = BufferBuilder()
		.Size(MaxTextures * sizeof(vec4))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.DebugName("PathTracerMaterials")
		.Create(Device);
	WriteDescriptors materials;
	for (View& view : Views)
		materials.AddBuffer(view.SceneSet.get(), 20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MaterialBuffer.get());
	materials.Execute(Device);

	PipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(DescriptorLayout.get())
		.AddSetLayout(ViewLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants))
		.DebugName("PathTracerPipelineLayout")
		.Create(Device);

	TraceShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Trace.comp", Shaders::Trace())
		.DebugName("PathTracerTrace")
		.Create("PathTracerTrace", Device);

	TracePipeline = ComputePipelineBuilder()
		.Layout(PipelineLayout.get())
		.ComputeShader(TraceShader.get())
		.DebugName("PathTracerTracePipeline")
		.Create(Device);

	// The fog lights' shadow cubes, with the trace's own bindings.
	FogShadowShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/FogShadows.comp", Shaders::FogShadows())
		.DebugName("PathTracerFogShadows")
		.Create("PathTracerFogShadows", Device);

	FogShadowPipeline = ComputePipelineBuilder()
		.Layout(PipelineLayout.get())
		.ComputeShader(FogShadowShader.get())
		.DebugName("PathTracerFogShadowPipeline")
		.Create(Device);
}

// The pass after Ray Reconstruction: its output tonemapped, with the fog and
// the screen flash over it, into the output image.
void TraceRenderer::CreateFinishPipeline()
{
	FinishLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerFinishSetLayout")
		.Create(Device);

	FinishPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MaxViews)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MaxViews)
		.MaxSets(MaxViews)
		.DebugName("PathTracerFinishPool")
		.Create(Device);
	for (View& view : Views)
		view.FinishSet = FinishPool->allocate(FinishLayout.get());

	// The fog is traced at the render size and filtered up to the output's.
	FogSampler = SamplerBuilder()
		.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
		.DebugName("PathTracerFogSampler")
		.Create(Device);

	FinishPipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(FinishLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vec4) * 2)
		.DebugName("PathTracerFinishPipelineLayout")
		.Create(Device);

	FinishShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Finish.comp", Shaders::Finish())
		.DebugName("PathTracerFinish")
		.Create("PathTracerFinish", Device);

	FinishPipeline = ComputePipelineBuilder()
		.Layout(FinishPipelineLayout.get())
		.ComputeShader(FinishShader.get())
		.DebugName("PathTracerFinishPipeline")
		.Create(Device);
}

// The pass after the denoiser: the trace's emission and surface colours, NRD's
// lighting and the fog, into the output image.
void TraceRenderer::CreateCompositePipeline()
{
	DescriptorSetLayoutBuilder layout;
	for (int i = 0; i < 9; i++)
		layout.AddBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	CompositeLayout = layout.DebugName("PathTracerCompositeSetLayout").Create(Device);

	CompositePool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9 * MaxViews)
		.MaxSets(MaxViews)
		.DebugName("PathTracerCompositePool")
		.Create(Device);
	for (View& view : Views)
		view.CompositeSet = CompositePool->allocate(CompositeLayout.get());

	CompositePipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(CompositeLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vec4) * 2)
		.DebugName("PathTracerCompositePipelineLayout")
		.Create(Device);

	CompositeShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Composite.comp", Shaders::Composite())
		.DebugName("PathTracerComposite")
		.Create("PathTracerComposite", Device);

	CompositePipeline = ComputePipelineBuilder()
		.Layout(CompositePipelineLayout.get())
		.ComputeShader(CompositeShader.get())
		.DebugName("PathTracerCompositePipeline")
		.Create(Device);
}

// Everything the frames in flight are reading is about to go, so they are
// waited for first.
void TraceRenderer::ResetScene()
{
	Context->WaitForGpu();
	Accel->Reset();
	Scene = SceneData();
	Pending.clear();
	for (Slot& slot : Slots)
		slot = Slot();
	BoundTextures = 0;

	// Every slot starts valid and matte. A descriptor that is never read still
	// has to be something, and filling the array once is cheaper than tracking
	// which slots the shader might reach.
	WriteDescriptors writes;
	for (int i = 0; i < MaxTextures; i++)
		WriteTextureSlot(writes, i, WhiteView.get());
	writes.Execute(Device);
	auto* mapped = (vec4*)MaterialBuffer->Map(0, MaxTextures * sizeof(vec4));
	for (int i = 0; i < MaxTextures; i++)
		mapped[i] = MatteMaterial;
	MaterialBuffer->Unmap();

	DescriptorsDirty = true;
	for (View& view : Views)
		view.DenoiseRestart = true;
}

// A slot of the texture array, in every view's set.
void TraceRenderer::WriteTextureSlot(WriteDescriptors& writes, int index, VulkanImageView* image)
{
	for (View& view : Views)
		writes.AddCombinedImageSampler(view.SceneSet.get(), 6, index, image, SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// The texture filter's anisotropy, the device's MaxAnisotropy: how many
// samples it may take along a footprint that is long one way and short the
// other - a floor seen at a glance - which the trace hands over as its two
// axes (footprintAxes in Shaders.cpp). A sampler cannot be changed, so a new
// one goes into every slot of the array, once the frames in flight are done
// with the old.
void TraceRenderer::SetAnisotropy(uint32_t samples)
{
	const float limit = Device->PhysicalDevice.Properties.Properties.limits.maxSamplerAnisotropy;
	samples = std::min(samples, (uint32_t)std::max(limit, 1.0f));
	if (samples <= 1)
		samples = 1;
	if (samples == SceneAnisotropy)
		return;
	Context->WaitForGpu();
	SceneAnisotropy = samples;

	SamplerBuilder builder;
	builder.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT)
		.DebugName("PathTracerSceneSampler");
	if (samples > 1)
		builder.Anisotropy((float)samples);
	SceneSampler = builder.Create(Device);

	WriteDescriptors writes;
	for (int i = 0; i < MaxTextures; i++)
		WriteTextureSlot(writes, i, Slots[i].View ? Slots[i].View.get() : WhiteView.get());
	writes.Execute(Device);
	if (samples > 1)
		debugf("PathTracer texture filter: %ux anisotropic", samples);
	else
		debugf("PathTracer texture filter: trilinear");
}

void TraceRenderer::BindWhite(uint32_t index)
{
	WriteDescriptors writes;
	WriteTextureSlot(writes, (int)index, WhiteView.get());
	writes.Execute(Device);
}

// A new level's textures, or one first seen mid-level. It rewrites the
// materials and a descriptor the frames in flight read, so it waits for them:
// a stall, but not a per frame one.
// A texture with its mips as the engine stores them, levels of them end to
// end in pixels: sampled at the level the width of a ray's footprint calls
// for, as the other devices sample theirs, rather than always at the top -
// which is what made distant floors and walls sparkle.
void TraceRenderer::SetTexture(uint32_t index, uint32_t width, uint32_t height, uint32_t levels, uint32_t format, const uint32_t* pixels, const vec4& material)
{
	if (index >= (uint32_t)MaxTextures)
		return;
	Context->WaitForGpu();

	auto* mapped = (vec4*)MaterialBuffer->Map(0, MaxTextures * sizeof(vec4));
	mapped[index] = material;
	MaterialBuffer->Unmap();

	BoundTextures = std::max<size_t>(BoundTextures, index + 1);
	Slot& slot = Slots[index];
	slot = Slot();
	if (!CanSampleTextures || !width || !height || !pixels)
	{
		BindWhite(index);
		return;
	}

	slot.Width = width;
	slot.Height = height;
	slot.Levels = std::max(levels, 1u);
	slot.Format = format;
	slot.Material = material;
	// S3TC is sampled as it is stored. BC1 with its alpha: a block can mark
	// texels transparent, and some that are not masked use it anyway.
	const VkFormat vkFormat = format == TraceProtocol::TextureBc1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
	slot.Image = ImageBuilder()
		.Format(vkFormat)
		.Size(width, height, (int)slot.Levels)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerSceneTexture")
		.Create(Device);
	slot.View = ImageViewBuilder().Image(slot.Image.get(), vkFormat).DebugName("PathTracerSceneTextureView").Create(Device);

	const size_t bytes = TraceProtocol::MipChainBytes(format, width, height, slot.Levels);
	auto staging = BufferBuilder()
		.Size(bytes)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerTextureStaging")
		.Create(Device);
	memcpy(staging->Map(0, bytes), pixels, bytes);
	staging->Unmap();

	VulkanImage* image = slot.Image.get();
	VulkanBuffer* src = staging.get();
	const int levelCount = (int)slot.Levels;
	Context->ExecuteImmediate([image, src, width, height, levelCount, format](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levelCount)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy regions[16] = {};
		VkDeviceSize offset = 0;
		for (int i = 0; i < levelCount; i++)
		{
			const uint32_t w = std::max(width >> i, 1u), h = std::max(height >> i, 1u);
			regions[i].bufferOffset = offset;
			regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			regions[i].imageSubresource.mipLevel = (uint32_t)i;
			regions[i].imageSubresource.layerCount = 1;
			regions[i].imageExtent = { w, h, 1 };
			offset += (VkDeviceSize)TraceProtocol::MipBytes(format, w, h);
		}
		cmd->copyBufferToImage(src->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levelCount, regions);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levelCount)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	WriteDescriptors writes;
	WriteTextureSlot(writes, (int)index, slot.View.get());
	writes.Execute(Device);
}

void TraceRenderer::SetTexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels)
{
	if (index >= (uint32_t)MaxTextures || !Slots[index].Image || Slots[index].Width != width || Slots[index].Height != height)
		return;
	// A texture sent with its mips that has since started to change - a
	// script can give one an animation - has only its top level sent from
	// then on, and mips left as they were would show its first frame from a
	// distance. It becomes a single level instead, as a changing texture is.
	if (Slots[index].Levels > 1 || Slots[index].Format != TraceProtocol::TextureRgba8)
	{
		const vec4 material = Slots[index].Material;
		SetTexture(index, width, height, 1, TraceProtocol::TextureRgba8, pixels, material);
		return;
	}

	const size_t bytes = (size_t)width * height * 4;
	PendingPixels pending;
	pending.Index = index;
	pending.Staging = BufferBuilder()
		.Size(bytes)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerRealtimeStaging")
		.Create(Device);
	memcpy(pending.Staging->Map(0, bytes), pixels, bytes);
	pending.Staging->Unmap();
	Pending.push_back(std::move(pending));
}

// Copied into the images they already have, so nothing that points at those
// images has to change, and recorded into the frame's own command buffer
// rather than submitted one texture at a time.
void TraceRenderer::RecordTexturePixels(VulkanCommandBuffer* commands, FrameUploads& uploads)
{
	for (PendingPixels& pending : Pending)
	{
		Slot& slot = Slots[pending.Index];
		if (!slot.Image)
			continue;
		VulkanImage* image = slot.Image.get();
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { slot.Width, slot.Height, 1 };
		commands->copyBufferToImage(pending.Staging->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		uploads.Retire(std::move(pending.Staging));
	}
	Pending.clear();
}

// A view's images: the trace's at the render size, the picture at the output
// size - the same two sizes unless Ray Reconstruction is upscaling - and its
// own images when it is in use.
void TraceRenderer::Resize(View& view, int width, int height, int outputWidth, int outputHeight, bool forRayReconstruction)
{
	Context->WaitForGpu();

	view.AccumView.reset();
	view.AccumImage.reset();
	view.HistoryView.reset();
	view.HistoryImage.reset();
	view.OutputView.reset();
	view.OutputImage.reset();
	view.MotionView.reset();
	view.ReflectionMotionView.reset();
	for (int i = 0; i < GuideImageCount; i++)
	{
		view.GuideViews[i].reset();
		view.GuideImages[i].reset();
	}
	view.RrColorView.reset();
	view.RrColorImage.reset();
	view.RrDepthView.reset();
	view.RrDepthImage.reset();
	view.RrMotionView.reset();
	view.RrMotionImage.reset();
	view.RrOutputView.reset();
	view.RrOutputImage.reset();

	auto makeImage = [&](std::unique_ptr<VulkanImage>& image, std::unique_ptr<VulkanImageView>& imageView, VkFormat format, int w, int h, VkImageUsageFlags usage, const char* name)
	{
		image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.Usage(usage)
			.DebugName(name)
			.Create(Device);
		imageView = ImageViewBuilder().Image(image.get(), format).DebugName(name).Create(Device);
	};

	makeImage(view.AccumImage, view.AccumView, VK_FORMAT_R32G32B32A32_SFLOAT, width, height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "PathTracerAccum");

	// What each pixel was looking at last frame: the world position it hit and
	// which instance owned it. Compared against this frame to decide whether the
	// pixel's accumulated history still describes the same thing.
	makeImage(view.HistoryImage, view.HistoryView, VK_FORMAT_R32G32B32A32_SFLOAT, width, height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "PathTracerHistory");

	makeImage(view.OutputImage, view.OutputView, VK_FORMAT_R16G16B16A16_SFLOAT, outputWidth, outputHeight,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "PathTracerOutput");

	// In the trace shader's binding order. Depth and motion want the full
	// precision; the rest are colours and normals.
	for (int i = 0; i < GuideImageCount; i++)
	{
		const VkFormat format = GuideIsDepth(i) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
		makeImage(view.GuideImages[i], view.GuideViews[i], format, width, height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "PathTracerGuide");
	}

	// NRD reads motion from x and y, where the trace keeps the depth; a view
	// of the same image with its channels moved along says it without
	// another image or another pass.
	auto motionView = [&](VulkanImage* image)
	{
		VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		info.image = image->image;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		info.components = { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
		info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView handle = VK_NULL_HANDLE;
		vkCreateImageView(Device->device, &info, nullptr, &handle);
		return std::unique_ptr<VulkanImageView>(new VulkanImageView(Device, handle));
	};
	view.MotionView = motionView(view.GuideImages[1].get());
	view.ReflectionMotionView = motionView(view.GuideImages[9].get());

	// Ray Reconstruction's own. The depth and motion are bound whether or not
	// it is in use, so they are always made; the picture it reads and the one
	// it writes only when it is.
	const VkImageUsageFlags rrUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	makeImage(view.RrDepthImage, view.RrDepthView, VK_FORMAT_R32_SFLOAT, width, height, rrUsage, "PathTracerRrDepth");
	makeImage(view.RrMotionImage, view.RrMotionView, VK_FORMAT_R16G16_SFLOAT, width, height, rrUsage, "PathTracerRrMotion");
	if (forRayReconstruction)
	{
		makeImage(view.RrColorImage, view.RrColorView, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, rrUsage, "PathTracerRrColor");
		makeImage(view.RrOutputImage, view.RrOutputView, VK_FORMAT_R16G16B16A16_SFLOAT, outputWidth, outputHeight, rrUsage, "PathTracerRrOutput");
	}

	if (view.Denoise)
	{
		try
		{
			view.Denoise->Resize(width, height);
		}
		catch (const std::exception& e)
		{
			debugf("PathTracer denoiser failed: %s", e.what());
			view.Denoise.reset();
			DenoiseFailed = true;
		}
	}
	view.DenoiseRestart = true;

	view.TraceWidth = width;
	view.TraceHeight = height;
	view.OutputWidth = outputWidth;
	view.OutputHeight = outputHeight;
	view.TracingForRr = forRayReconstruction;
	DescriptorsDirty = true;

	// Everything starts undefined and the shaders use it as GENERAL.
	Context->ExecuteImmediate([&view](VulkanCommandBuffer* cmd)
	{
		StartCleared(cmd, view.AccumImage.get(), view.HistoryImage.get());
		PipelineBarrier barrier;
		for (VulkanImage* image : { view.OutputImage.get(), view.RrDepthImage.get(), view.RrMotionImage.get(), view.RrColorImage.get(), view.RrOutputImage.get() })
			if (image)
				barrier.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		for (int i = 0; i < GuideImageCount; i++)
			barrier.AddImage(view.GuideImages[i].get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		barrier.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	const char* which = &view == &Views[0] ? "" : " (right eye)";
	if (width == outputWidth && height == outputHeight)
		debugf("PathTracer buffers: %dx%d%s", width, height, which);
	else
		debugf("PathTracer buffers: traced at %dx%d for %dx%d%s", width, height, outputWidth, outputHeight, which);
}

// Made the first time it is wanted, so a game that never denoises neither
// builds NRD's pipelines nor risks them failing, and made again when materials
// are switched, since that changes the shape of its first signal. A failure
// switches denoising off rather than taking the helper down with it.
void TraceRenderer::EnsureDenoiser(View& view, bool wanted, bool materials)
{
	if (view.Denoise && view.Denoise->HasSpecular() != materials)
	{
		Context->WaitForGpu();
		view.Denoise.reset();
		view.DenoiseRestart = true;
		DescriptorsDirty = true;
	}

	if (!wanted || view.Denoise || DenoiseFailed || !view.TraceWidth)
		return;

	try
	{
		view.Denoise.reset(new Denoiser(Device, materials));
		view.Denoise->Resize(view.TraceWidth, view.TraceHeight);
	}
	catch (const std::exception& e)
	{
		debugf("PathTracer denoiser failed: %s", e.what());
		view.Denoise.reset();
		DenoiseFailed = true;
	}
	DescriptorsDirty = true;
	if (view.Denoise)
	{
		debugf("PathTracer denoiser: %s", view.Denoise->Problem());
		if (!view.Denoise->Available())
			DenoiseFailed = true;
	}
}

void TraceRenderer::WriteCompositeDescriptors(View& view)
{
	if (!view.CompositeSet || !view.OutputView || !view.Denoise || !view.Denoise->Available() || !view.Denoise->Output(0))
		return;

	VulkanDescriptorSet* set = view.CompositeSet.get();
	WriteDescriptors()
		.AddStorageImage(set, 0, view.OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 1, view.GuideViews[4].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 2, view.GuideViews[5].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 3, view.GuideViews[6].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 4, view.Denoise->Output(0), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 5, view.Denoise->Output(1), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 6, view.GuideViews[7].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 7, view.GuideViews[11].get(), VK_IMAGE_LAYOUT_GENERAL)
		// Never read without materials, but a descriptor still has to be valid.
		.AddStorageImage(set, 8, view.Denoise->SpecularOutput() ? view.Denoise->SpecularOutput() : view.GuideViews[10].get(), VK_IMAGE_LAYOUT_GENERAL)
		.Execute(Device);
}

// Only when something they point at was made again. The frame before may
// still be reading them, so it is waited for first: descriptors in use cannot
// be rewritten.
void TraceRenderer::WriteFinishDescriptors(View& view)
{
	if (!view.FinishSet || !view.OutputView || !view.RrOutputView)
		return;
	WriteDescriptors()
		.AddStorageImage(view.FinishSet.get(), 0, view.OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(view.FinishSet.get(), 1, view.RrOutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddCombinedImageSampler(view.FinishSet.get(), 2, view.GuideViews[7].get(), FogSampler.get(), VK_IMAGE_LAYOUT_GENERAL)
		// The flashlight's beam, in the emission image NRD would use.
		.AddCombinedImageSampler(view.FinishSet.get(), 3, view.GuideViews[4].get(), FogSampler.get(), VK_IMAGE_LAYOUT_GENERAL)
		.Execute(Device);
}

// Every view that has its images: the scene's bindings, which are the same in
// each, and its own.
void TraceRenderer::UpdateDescriptors()
{
	if (!DescriptorsDirty || !Accel->IsReady() || !Accel->GetInstanceDataBuffer() || !Accel->GetLightGridBuffer() || !Accel->GetLightmapBuffer() || !Accel->GetEmitterBuffer() || !FogShadowBuffer)
		return;
	for (int v = 0; v < ActiveViews; v++)
		if (!Views[v].AccumView || !Views[v].MotionBuffer)
			return;
	Context->WaitForGpu();

	for (View& view : Views)
	{
		if (!view.AccumView || !view.MotionBuffer)
			continue;
		VulkanDescriptorSet* set = view.SceneSet.get();
		WriteDescriptors writes;
		for (int i = 0; i < GuideImageCount; i++)
			writes.AddStorageImage(set, GuideBinding(i), view.GuideViews[i].get(), VK_IMAGE_LAYOUT_GENERAL);
		writes.AddBuffer(set, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, view.MotionBuffer.get());
		writes.AddBuffer(set, 26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FogShadowBuffer.get());
		writes
			.AddAccelerationStructure(set, 0, Accel->GetTopLevel())
			.AddStorageImage(view.ImageSet.get(), 0, view.AccumView.get(), VK_IMAGE_LAYOUT_GENERAL)
			// With Ray Reconstruction the trace's picture is its input, at the
			// render size; otherwise it is the picture.
			.AddStorageImage(view.ImageSet.get(), 1, view.TracingForRr ? view.RrColorView.get() : view.OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
			.AddStorageImage(view.ImageSet.get(), 2, view.HistoryView.get(), VK_IMAGE_LAYOUT_GENERAL)
			.AddStorageImage(set, 23, view.RrDepthView.get(), VK_IMAGE_LAYOUT_GENERAL)
			.AddStorageImage(set, 24, view.RrMotionView.get(), VK_IMAGE_LAYOUT_GENERAL)
			.AddBuffer(set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetAttributeBuffer())
			.AddBuffer(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightBuffer())
			.AddBuffer(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetInstanceDataBuffer())
			.AddBuffer(set, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightGridBuffer())
			.AddBuffer(set, 25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightmapBuffer())
			.AddBuffer(set, 27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetEmitterBuffer())
			.Execute(Device);
		WriteCompositeDescriptors(view);
		WriteFinishDescriptors(view);
	}

	DescriptorsDirty = false;
}

// Last frame's camera, this frame's jitter for Ray Reconstruction and how
// much the glowing surfaces light, the flashlight, where the middle of the
// view is, then each instance's last placement as three rows, in the
// order the top level structure numbers them. An instance with no last
// placement - new this frame, or one that never moves - is given its current
// one, which reads as not having moved. Each view has its own, for its own
// last camera.
void TraceRenderer::WriteMotion(View& view, const TraceProtocol::TraceCommand& frame, const vec4* previousCamera, vec4 shift, vec2 jitter, FrameUploads& uploads)
{
	const size_t header = 14;
	const size_t count = Scene.Instances.size();
	const size_t wanted = header + std::max<size_t>(count, 1) * 3;
	if (!view.MotionBuffer || wanted > view.MotionCapacity)
	{
		view.MotionCapacity = std::max<size_t>(wanted * 2, header + 256 * 3);
		uploads.Retire(std::move(view.MotionBuffer));
		view.MotionBuffer = BufferBuilder()
			.Size(view.MotionCapacity * sizeof(vec4))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			.MinAlignment(256)
			.DebugName("PathTracerMotion")
			.Create(Device);
		DescriptorsDirty = true;
	}

	auto* mapped = (vec4*)uploads.Write(view.MotionBuffer.get(), 0, wanted * sizeof(vec4));
	for (int i = 0; i < 4; i++)
		mapped[i] = previousCamera[i];
	mapped[4] = vec4(jitter.x, jitter.y, frame.GlowLighting, frame.Wetness);
	for (int i = 0; i < 3; i++)
		mapped[5 + i] = frame.Flashlight[i];
	for (int i = 0; i < 3; i++)
		mapped[8 + i] = frame.SkyAxes[i];
	mapped[11] = frame.PhotoLens;
	mapped[12] = vec4(frame.BumpMapping, std::max(frame.ToneCeiling, 1.0f), 0.0f, 0.0f);
	mapped[13] = shift;
	for (size_t i = 0; i < count; i++)
	{
		const SceneInstance& instance = Scene.Instances[i];
		const float* m = instance.HasPrevious ? instance.PreviousTransform : instance.Transform;
		for (int r = 0; r < 3; r++)
			mapped[header + i * 3 + r] = vec4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
	}
	// With no instances, the one placement the buffer is sized for.
	if (count == 0)
		for (int r = 0; r < 3; r++)
			mapped[header + r] = vec4(0.0f);
}

// The fog lights' shadow cubes, FogShadowSize squared floats a face, six
// faces a light: grown as a level asks for more, never shrunk. The shader
// binding needs a buffer even with no fog, so there is always room for one.
void TraceRenderer::EnsureFogShadows(uint32_t count, FrameUploads& uploads)
{
	const size_t wanted = (size_t)std::max<uint32_t>(count, 1u) * 6 * FogShadowSize * FogShadowSize;
	if (FogShadowBuffer && wanted <= FogShadowCapacity)
		return;
	FogShadowCapacity = wanted;
	uploads.Retire(std::move(FogShadowBuffer));
	FogShadowBuffer = BufferBuilder()
		.Size(FogShadowCapacity * sizeof(float))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
		.DebugName("PathTracerFogShadows")
		.Create(Device);
	DescriptorsDirty = true;
}

bool TraceRenderer::Record(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame, int slot, const EyeView* eyes, bool sideBySide)
{
	using namespace TraceProtocol;
	if (frame.Width == 0 || frame.Height == 0)
		return false;
	auto lapStart = std::chrono::steady_clock::now();
	auto lap = [&](int stage)
	{
		const auto t = std::chrono::steady_clock::now();
		RecordStageMs[stage] += std::chrono::duration<double, std::milli>(t - lapStart).count();
		lapStart = t;
	};

	// Which denoiser. Ray Reconstruction is started the first time it is
	// asked for, and where it cannot run - no NVIDIA GPU, an old driver, wine
	// without the pieces NGX needs - NRD stands in for it. PT VIEW shows the
	// trace's own parts, so it takes neither.
	const bool wantRr = frame.Denoise == DenoiseDlss && frame.ViewMode == 0;
	if (wantRr && !RrTried)
	{
		RrTried = true;
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring dir = exe;
		dir = dir.substr(0, dir.find_last_of(L'\\'));
		Rr.reset(new RayReconstruction(Device, dir));
		debugf("PathTracer Ray Reconstruction: %s", Rr->Status());
	}
	const bool useRr = wantRr && Rr && Rr->Available();
	const bool wantNrd = frame.Denoise == DenoiseNrd || (frame.Denoise == DenoiseDlss && !useRr);
	const int quality = (int)std::min(frame.DlssQuality, 4u);

	// The views: the screen's, or the headset's eyes. Going from one to the
	// other, every view's history describes some other view.
	const int viewCount = eyes ? 2 : 1;
	if ((eyes != nullptr) != TracedEyes)
		for (View& view : Views)
			view.DenoiseRestart = true;
	ActiveViews = viewCount;
	for (int v = 0; v < viewCount; v++)
	{
		View& view = Views[v];
		const uint32_t width = eyes ? eyes[v].Width : frame.Width;
		const uint32_t height = eyes ? eyes[v].Height : frame.Height;

		// The trace runs at the size Ray Reconstruction is trained for at
		// this quality, which it then upscales to the size asked for.
		uint32_t renderWidth = width, renderHeight = height;
		if (useRr)
			Rr->RenderSize(width, height, quality, renderWidth, renderHeight);
		if ((int)renderWidth != view.TraceWidth || (int)renderHeight != view.TraceHeight ||
			(int)width != view.OutputWidth || (int)height != view.OutputHeight || useRr != view.TracingForRr)
			Resize(view, (int)renderWidth, (int)renderHeight, (int)width, (int)height, useRr);

		if (frame.RestartDenoiser)
			view.DenoiseRestart = true;
		EnsureDenoiser(view, wantNrd, frame.Materials != 0);
	}

	// Remade when its sizes or quality change, which the frame in flight may
	// still be using.
	if (useRr)
		for (int v = 0; v < viewCount; v++)
			if (Rr->NeedsFeature(v, Views[v].TraceWidth, Views[v].TraceHeight, Views[v].OutputWidth, Views[v].OutputHeight, quality))
			{
				Context->WaitForGpu();
				break;
			}
	const vec2 jitter = useRr ? RayReconstruction::Jitter(frame.Frame) : vec2(0.0f, 0.0f);

	const auto now = std::chrono::steady_clock::now();
	float frameMs = 16.0f;
	if (LastRecordTime.time_since_epoch().count() != 0)
		frameMs = std::min(std::max(std::chrono::duration<float, std::milli>(now - LastRecordTime).count(), 1.0f), 100.0f);
	LastRecordTime = now;

	// The host's half of the frame, staged: new shapes and poses, the lights,
	// the placements and the motion. The frame before may still be reading
	// the buffers these end up in.
	FrameUploads& uploads = *Uploads[slot];
	uploads.Begin();
	Accel->HideStatic = (frame.DebugMode == 1);
	lap(0);
	const double lightsBefore = Accel->LightsMs;
	Accel->Update(Scene, uploads);
	lap(1);
	RecordStageMs[1] -= Accel->LightsMs - lightsBefore;
	RecordStageMs[2] += Accel->LightsMs - lightsBefore;
	for (int v = 0; v < viewCount; v++)
	{
		const vec4 shift = eyes ? vec4(eyes[v].Shift.x, eyes[v].Shift.y, eyes[v].PreviousShift.x, eyes[v].PreviousShift.y) : vec4(0.0f);
		WriteMotion(Views[v], frame, eyes ? eyes[v].PreviousCamera : frame.PreviousCamera, shift, jitter, uploads);
	}
	// Room for a shadow cube for each fog light, when the fog is drawn
	// with its shadows.
	const uint32_t fogShadows = ((frame.DisableBits & (32u | 65536u)) == 0u)
		? (uint32_t)std::min<size_t>(Scene.FogLights.size(), MaxFogShadows) : 0u;
	EnsureFogShadows(fogShadows, uploads);
	lap(3);
	if (Accel->AttributesChanged())
	{
		Accel->ClearAttributesChanged();
		DescriptorsDirty = true;
	}

	// Made on first use, and only where the queue can time anything.
	if (!Timestamps && frame.Timing)
	{
		const auto& props = Device->PhysicalDevice.Properties.Properties;
		if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f)
		{
			Timestamps = QueryPoolBuilder()
				.QueryType(VK_QUERY_TYPE_TIMESTAMP, TimestampCount * GpuContext::FramesInFlight)
				.DebugName("PathTracerTimestamps")
				.Create(Device);
			TimestampPeriodMs = props.limits.timestampPeriod * 1.0e-6;
		}
	}
	const bool timing = Timestamps && frame.Timing;
	const uint32_t firstStamp = (uint32_t)slot * TimestampCount;
	auto stamp = [&](uint32_t index)
	{
		if (timing)
			commands->writeTimestamp(index ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Timestamps.get(), firstStamp + index);
	};
	if (timing)
		commands->resetQueryPool(Timestamps.get(), firstStamp, TimestampCount);

	// Everything from here on reuses what the frame before used - the
	// buffers the uploads land in, the structures, the images, NRD's history
	// - so it waits for that frame to be done with them, on the GPU. The CPU
	// has already recorded this far without waiting.
	VkMemoryBarrier previous = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	previous.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	previous.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &previous, 0, nullptr, 0, nullptr);
	stamp(0);

	uploads.Record(commands);

	// Textures that generate themselves get their new frame before the trace
	// reads them.
	RecordTexturePixels(commands, uploads);
	lap(4);

	// New shapes and changed poses, then the top level structure.
	Accel->Record(commands, uploads);
	if (!Accel->IsReady())
		return false;
	stamp(1);
	lap(5);

	UpdateDescriptors();
	lap(6);

	// Whether a view is denoised by NRD: it wants it, and its denoiser could
	// be made.
	auto nrdFor = [&](const View& view) { return wantNrd && view.Denoise && view.Denoise->Available() && frame.ViewMode == 0; };

	// A view's camera, as the frame describes the screen's or as an eye is.
	// The screen flash rides in the frame's w, whichever it is.
	auto setCamera = [&](int v)
	{
		const vec4* camera = eyes ? eyes[v].Camera : frame.Camera;
		PushConstants.CameraOrigin = vec4(camera[0].x, camera[0].y, camera[0].z, frame.Camera[0].w);
		PushConstants.CameraRight = vec4(camera[1].x, camera[1].y, camera[1].z, frame.Camera[1].w);
		PushConstants.CameraUp = vec4(camera[2].x, camera[2].y, camera[2].z, frame.Camera[2].w);
		PushConstants.CameraForward = vec4(camera[3].x, camera[3].y, camera[3].z, frame.Camera[3].w);
		PushConstants.Disable = frame.DisableBits | ((frame.ViewMode || nrdFor(Views[v])) ? 64u : 0u) | (frame.Materials ? 0u : 128u) | (useRr ? 256u : 0u);
	};
	setCamera(0);
	PushConstants.Counts[0] = frame.Frame;
	PushConstants.Counts[1] = (uint32_t)Accel->LightCount();
	// The top byte, signed, in sixteenths: how many mip levels sharper to
	// sample than the traced pixels' size calls for, when they are fewer than
	// the output's - Ray Reconstruction rebuilds the finer detail, and NVIDIA
	// asks for textures at the output's resolution, log2(render / output).
	// The eyes are one size, so one serves both.
	const View& first = Views[0];
	const float mipBias = (first.OutputHeight > 0 && first.TraceHeight > 0 && first.TraceHeight < first.OutputHeight)
		? std::log2((float)first.TraceHeight / (float)first.OutputHeight) : 0.0f;
	const int mipBiasSteps = std::max(-128, std::min(127, (int)std::lround(mipBias * 16.0f)));
	PushConstants.Counts[2] = std::min(std::max(frame.Bounces, 1u), 255u) | (std::min(frame.GlossBounces, 255u) << 8) |
		(std::min(frame.LightSize, 255u) << 16) | ((uint32_t)(uint8_t)(int8_t)mipBiasSteps << 24);
	PushConstants.Counts[3] = frame.AccumulatedFrames;
	PushConstants.TextureCount = CanSampleTextures ? (uint32_t)BoundTextures : 0u;
	PushConstants.MaxSamples = std::max(frame.MaxSamples, 1u);
	PushConstants.Time = frame.Time;
	PushConstants.SkyOrigin = frame.SkyOrigin;
	PushConstants.Params = vec4(
		frame.Exposure,
		frame.SkyIntensity,
		frame.Lighting ? 1.0f : 0.0f,
		(float)(frame.ViewMode ? frame.ViewMode : frame.DebugMode));

	// The fog lights' shadow cubes, every frame, since anything that moves
	// can cast a shadow through the glow: a texel a thread, six faces a
	// light. The trace, the views in the HUD's windows among it, reads them.
	// Any view's sets serve: the scene's bindings are the same in each.
	if (fogShadows > 0)
	{
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, Views[0].SceneSet.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 1, Views[0].ImageSet.get());
		commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &PushConstants);
		commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, FogShadowPipeline.get());
		commands->dispatch(FogShadowSize / 8, FogShadowSize / 8, fogShadows * 6);
		VkMemoryBarrier written = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
	}

	commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, TracePipeline.get());
	for (int v = 0; v < viewCount; v++)
	{
		View& view = Views[v];
		setCamera(v);
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, view.SceneSet.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 1, view.ImageSet.get());
		commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &PushConstants);
		commands->dispatch((view.TraceWidth + 7) / 8, (view.TraceHeight + 7) / 8, 1);
	}
	stamp(2);
	lap(7);

	auto cameraOf = [](const vec4* p, vec2 shift)
	{
		Denoiser::Camera c;
		c.Origin = vec3(p[0].x, p[0].y, p[0].z);
		c.Right = vec3(p[1].x, p[1].y, p[1].z);
		c.Up = vec3(p[2].x, p[2].y, p[2].z);
		c.Forward = vec3(p[3].x, p[3].y, p[3].z);
		c.Shift = shift;
		return c;
	};
	uint32_t denoisedWith = DenoiseOff;
	for (int v = 0; v < viewCount; v++)
	{
		View& view = Views[v];
		const vec4* camera = eyes ? eyes[v].Camera : frame.Camera;
		const vec4* previousCamera = eyes ? eyes[v].PreviousCamera : frame.PreviousCamera;
		const vec2 shift = eyes ? eyes[v].Shift : vec2(0.0f, 0.0f);
		const vec2 previousShift = eyes ? eyes[v].PreviousShift : vec2(0.0f, 0.0f);

		// Denoised: NRD over the trace's split lighting, then the picture
		// rebuilt from it over the one the trace wrote.
		if (nrdFor(view))
		{
			VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);

			Denoiser::Inputs inputs[Denoiser::SignalCount];
			inputs[0].NormalRoughness = view.GuideViews[0].get();
			inputs[0].ViewZ = view.GuideViews[1].get();
			inputs[0].Motion = view.MotionView.get();
			inputs[0].Diffuse = view.GuideViews[2].get();
			inputs[0].Specular = view.Denoise->HasSpecular() ? view.GuideViews[10].get() : nullptr;
			inputs[1].NormalRoughness = view.GuideViews[8].get();
			inputs[1].ViewZ = view.GuideViews[9].get();
			inputs[1].Motion = view.ReflectionMotionView.get();
			inputs[1].Diffuse = view.GuideViews[3].get();
			view.Denoise->Denoise(commands, inputs, cameraOf(camera, shift), cameraOf(previousCamera, previousShift), view.DenoiseRestart, slot);
			view.DenoiseRestart = false;

			struct { vec4 Flash; vec4 Exposure; } finish;
			finish.Flash = vec4(frame.Camera[0].w, frame.Camera[1].w, frame.Camera[2].w, frame.Camera[3].w);
			finish.Exposure = vec4(frame.Exposure, view.Denoise->HasSpecular() ? 1.0f : 0.0f, (frame.DisableBits & 8192u) ? 1.0f : 0.0f, std::max(frame.ToneCeiling, 1.0f));
			commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, CompositePipeline.get());
			commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, CompositePipelineLayout.get(), 0, view.CompositeSet.get());
			commands->pushConstants(CompositePipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(finish), &finish);
			commands->dispatch((view.TraceWidth + 7) / 8, (view.TraceHeight + 7) / 8, 1);
			denoisedWith = DenoiseNrd;
		}
		else if (useRr)
		{
			// Ray Reconstruction over the trace's picture, denoised and
			// upscaled in one, then finished at the output's size. NGX's own
			// passes read and write in stages of their choosing, so the
			// barriers either side cover everything.
			VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);

			auto target = [](const std::unique_ptr<VulkanImage>& image, const std::unique_ptr<VulkanImageView>& imageView, VkFormat format)
			{
				RayReconstruction::Target t;
				t.Image = image.get();
				t.View = imageView.get();
				t.Format = (int)format;
				return t;
			};
			RayReconstruction::Inputs inputs;
			inputs.Color = target(view.RrColorImage, view.RrColorView, VK_FORMAT_R16G16B16A16_SFLOAT);
			inputs.NormalRoughness = target(view.GuideImages[0], view.GuideViews[0], VK_FORMAT_R16G16B16A16_SFLOAT);
			inputs.DiffuseAlbedo = target(view.GuideImages[5], view.GuideViews[5], VK_FORMAT_R16G16B16A16_SFLOAT);
			inputs.SpecularAlbedo = target(view.GuideImages[6], view.GuideViews[6], VK_FORMAT_R16G16B16A16_SFLOAT);
			inputs.Depth = target(view.RrDepthImage, view.RrDepthView, VK_FORMAT_R32_SFLOAT);
			inputs.Motion = target(view.RrMotionImage, view.RrMotionView, VK_FORMAT_R16G16_SFLOAT);
			inputs.Output = target(view.RrOutputImage, view.RrOutputView, VK_FORMAT_R16G16B16A16_SFLOAT);
			const bool reconstructed = Rr->Evaluate(commands, v, inputs, view.TraceWidth, view.TraceHeight, view.OutputWidth, view.OutputHeight,
				quality, jitter, view.DenoiseRestart, frameMs);
			view.DenoiseRestart = false;

			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
			if (reconstructed)
			{
				struct { vec4 Flash; vec4 Mode; } finish;
				finish.Flash = vec4(frame.Camera[0].w, frame.Camera[1].w, frame.Camera[2].w, frame.Camera[3].w);
				finish.Mode = vec4((frame.DisableBits & 8192u) ? 1.0f : 0.0f, std::max(frame.ToneCeiling, 1.0f), 0.0f, 0.0f);
				commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, FinishPipeline.get());
				commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, FinishPipelineLayout.get(), 0, view.FinishSet.get());
				commands->pushConstants(FinishPipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(finish), &finish);
				commands->dispatch((view.OutputWidth + 7) / 8, (view.OutputHeight + 7) / 8, 1);
			}
			denoisedWith = reconstructed ? DenoiseDlss : DenoiseOff;
		}
	}
	stamp(3);
	LastDenoiser = denoisedWith;
	lap(8);

	TracedEyes = eyes != nullptr;
	if (eyes)
	{
		RecordMirror(commands, (int)frame.Width, (int)frame.Height, sideBySide);
		RecordInsets(commands, frame, MirrorImage.get(), MirrorWidth, MirrorHeight);
	}
	else
		RecordInsets(commands, frame, Views[0].OutputImage.get(), Views[0].OutputWidth, Views[0].OutputHeight);
	stamp(4);
	lap(9);
	TimestampsPending[slot] = timing;
	return true;
}

// With the eyes traced, the picture the device is handed for the screen: the
// left eye's, the middle of it cut to the screen's shape - or, for the test
// harness, both eyes whole, side by side.
void TraceRenderer::RecordMirror(VulkanCommandBuffer* commands, int width, int height, bool sideBySide)
{
	if (!MirrorImage || MirrorWidth != width || MirrorHeight != height)
	{
		// The last frame's hand over may still be copying out of the old one.
		Context->WaitForGpu();
		MirrorImage = ImageBuilder()
			.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName("PathTracerMirror")
			.Create(Device);
		MirrorWidth = width;
		MirrorHeight = height;
		MirrorFresh = true;
	}

	PipelineBarrier()
		.AddImage(MirrorImage.get(), MirrorFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
		.AddImage(Views[0].OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.AddImage(Views[1].OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	MirrorFresh = false;

	VkClearColorValue black = {};
	black.float32[3] = 1.0f;
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	commands->clearColorImage(MirrorImage->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
	PipelineBarrier()
		.AddImage(MirrorImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	auto blit = [&](const View& eye, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh)
	{
		VkImageBlit region = {};
		region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.srcOffsets[0] = { sx, sy, 0 };
		region.srcOffsets[1] = { sx + sw, sy + sh, 1 };
		region.dstSubresource = region.srcSubresource;
		region.dstOffsets[0] = { dx, dy, 0 };
		region.dstOffsets[1] = { dx + dw, dy + dh, 1 };
		commands->blitImage(eye.OutputImage->image, VK_IMAGE_LAYOUT_GENERAL, MirrorImage->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_LINEAR);
	};
	if (sideBySide)
	{
		for (int e = 0; e < 2; e++)
		{
			const View& eye = Views[e];
			const float scale = std::min((width / 2) / (float)eye.OutputWidth, height / (float)eye.OutputHeight);
			const int dw = std::max((int)(eye.OutputWidth * scale), 1), dh = std::max((int)(eye.OutputHeight * scale), 1);
			blit(eye, 0, 0, eye.OutputWidth, eye.OutputHeight, e * (width / 2) + (width / 2 - dw) / 2, (height - dh) / 2, dw, dh);
		}
	}
	else
	{
		const View& eye = Views[0];
		const float screen = width / (float)height, picture = eye.OutputWidth / (float)eye.OutputHeight;
		int sw = eye.OutputWidth, sh = eye.OutputHeight;
		if (picture > screen)
			sw = std::max((int)(eye.OutputHeight * screen), 1);
		else
			sh = std::max((int)(eye.OutputWidth / screen), 1);
		blit(eye, (eye.OutputWidth - sw) / 2, (eye.OutputHeight - sh) / 2, sw, sh, 0, 0, width, height);
	}

	// For the windows' views copied in over it, and the hand over after; the
	// eyes are read again on their way to the headset.
	PipelineBarrier()
		.AddImage(MirrorImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
		.AddImage(Views[0].OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
		.AddImage(Views[1].OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// The views the HUD draws in windows of their own - a security computer's
// cameras, the spy drone's, the targeting augmentation's zoom - each traced
// from its own viewpoint once the player's is done, at its window's size in
// output pixels, and copied into the output over it. Not denoised: each
// averages its own samples over the frames it holds still, as a view with
// denoising off does, in images of its own. The engine draws them over the
// view the same way, and the device draws them back at their place among
// the HUD's own drawing (see UPathTracerRenderDevice::SetSceneNode). With a
// headset's eyes traced, they go into the picture the screen is handed,
// which is where the device takes them from for the HUD.
void TraceRenderer::RecordInsets(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame, VulkanImage* target, int targetWidth, int targetHeight)
{
	static_assert(MaxInsets == (int)TraceProtocol::MaxInsets, "one set of images for each inset the protocol carries");
	const uint32_t count = std::min(frame.InsetCount, TraceProtocol::MaxInsets);
	for (uint32_t i = 0; i < count; i++)
	{
		const TraceProtocol::TraceInset& view = frame.Insets[i];
		const int width = (int)std::min<uint32_t>(view.Width, (uint32_t)std::max(targetWidth - (int)view.X, 0));
		const int height = (int)std::min<uint32_t>(view.Height, (uint32_t)std::max(targetHeight - (int)view.Y, 0));
		if (width <= 0 || height <= 0)
			continue;

		Inset& inset = Insets[i];
		bool fresh = false;
		if (inset.Width != width || inset.Height != height)
		{
			// A window that appears or changes size: rare, so it waits.
			Context->WaitForGpu();
			auto makeImage = [&](std::unique_ptr<VulkanImage>& image, std::unique_ptr<VulkanImageView>& imageView, VkFormat format, VkImageUsageFlags usage, const char* name)
			{
				image = ImageBuilder().Format(format).Size(width, height).Usage(usage).DebugName(name).Create(Device);
				imageView = ImageViewBuilder().Image(image.get(), format).DebugName(name).Create(Device);
			};
			makeImage(inset.Accum, inset.AccumView, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "PathTracerInsetAccum");
			makeImage(inset.Out, inset.OutView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "PathTracerInsetOut");
			makeImage(inset.History, inset.HistoryView, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "PathTracerInsetHistory");
			WriteDescriptors()
				.AddStorageImage(inset.Set.get(), 0, inset.AccumView.get(), VK_IMAGE_LAYOUT_GENERAL)
				.AddStorageImage(inset.Set.get(), 1, inset.OutView.get(), VK_IMAGE_LAYOUT_GENERAL)
				.AddStorageImage(inset.Set.get(), 2, inset.HistoryView.get(), VK_IMAGE_LAYOUT_GENERAL)
				.Execute(Device);
			inset.Width = width;
			inset.Height = height;
			fresh = true;
		}
		if (fresh)
		{
			StartCleared(commands, inset.Accum.get(), inset.History.get());
			PipelineBarrier()
				.AddImage(inset.Out.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
				.Execute(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}

		// The player's settings, from its own eye, at its own size, and with
		// nothing for a denoiser: the view is neutral to the screen flash,
		// which the engine puts over the whole screen.
		TracePushConstants constants = PushConstants;
		constants.CameraOrigin = vec4(view.Camera[0].x, view.Camera[0].y, view.Camera[0].z, 1.0f);
		constants.CameraRight = vec4(view.Camera[1].x, view.Camera[1].y, view.Camera[1].z, 0.0f);
		constants.CameraUp = vec4(view.Camera[2].x, view.Camera[2].y, view.Camera[2].z, 0.0f);
		constants.CameraForward = vec4(view.Camera[3].x, view.Camera[3].y, view.Camera[3].z, 0.0f);
		constants.Disable = (constants.Disable & ~(64u | 256u)) | 32768u;
		constants.Counts[2] &= 0x00ffffffu;
		constants.Counts[3] = fresh ? 0u : view.AccumulatedFrames;
		constants.Params.w = 0.0f;

		commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, TracePipeline.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, Views[0].SceneSet.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 1, inset.Set.get());
		commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &constants);
		commands->dispatch((width + 7) / 8, (height + 7) / 8, 1);

		// Over the output at the window: after the player's view is written
		// there, whichever pass wrote it, and before it is handed over.
		VkMemoryBarrier toCopy = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
		VkImageCopy copy = {};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = copy.srcSubresource;
		copy.dstOffset = { (int32_t)view.X, (int32_t)view.Y, 0 };
		copy.extent = { (uint32_t)width, (uint32_t)height, 1 };
		vkCmdCopyImage(commands->buffer, inset.Out->image, VK_IMAGE_LAYOUT_GENERAL, target->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		VkMemoryBarrier afterCopy = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		afterCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		afterCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &afterCopy, 0, nullptr, 0, nullptr);
	}
}

const char* TraceRenderer::DlssStatus() const
{
	return Rr ? Rr->Status() : (RrTried ? "could not be started" : "not asked for yet");
}

// Only the timestamps are read here. The frame's staging is reused, and what
// it retired freed, when its slot records again.
void TraceRenderer::FrameCompleted(int slot)
{
	GpuTimed = false;
	if (!TimestampsPending[slot] || !Timestamps)
		return;
	TimestampsPending[slot] = false;

	uint64_t t[TimestampCount] = {};
	if (!Timestamps->getResults((uint32_t)slot * TimestampCount, TimestampCount, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT))
		return;
	auto ms = [&](int a, int b) { return t[b] > t[a] ? (float)((double)(t[b] - t[a]) * TimestampPeriodMs) : 0.0f; };
	GpuMs[0] = ms(0, 1);
	GpuMs[1] = ms(1, 2);
	GpuMs[2] = ms(2, 3);
	GpuMs[3] = ms(3, 4);
	GpuTimed = true;
}

int TraceRenderer::LightCount() const { return Accel->LightCount(); }
int TraceRenderer::BottomCount() const { return Accel->BottomCount(); }
bool TraceRenderer::DenoiserActive() const { return Views[0].Denoise && Views[0].Denoise->Available(); }
const char* TraceRenderer::DenoiserStatus() const { return Views[0].Denoise ? Views[0].Denoise->Problem() : (DenoiseFailed ? "failed" : "not made yet"); }
