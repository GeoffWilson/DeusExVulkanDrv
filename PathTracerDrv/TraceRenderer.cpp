#include "TracePrecomp.h"
#include "TraceRenderer.h"
#include "TraceProtocol.h"
#include "AccelStructure.h"
#include "Denoiser.h"
#include "Shaders.h"
#include <stdexcept>

// Matte: roughness 1, no metal, the usual reflectance. See Materials.h.
static const vec4 MatteMaterial(1.0f, 0.0f, 0.04f, 0.0f);

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
	CreateTracePipeline();
	CreateCompositePipeline();
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
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
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
		.DebugName("PathTracerSetLayout")
		.Create(Device);

	DescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 + GuideImageCount)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures)
		.MaxSets(1)
		.DebugName("PathTracerDescriptorPool")
		.Create(Device);

	DescriptorSet = DescriptorPool->allocate(DescriptorLayout.get());

	MaterialBuffer = BufferBuilder()
		.Size(MaxTextures * sizeof(vec4))
		.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.DebugName("PathTracerMaterials")
		.Create(Device);
	WriteDescriptors()
		.AddBuffer(DescriptorSet.get(), 20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MaterialBuffer.get())
		.Execute(Device);

	PipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(DescriptorLayout.get())
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
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9)
		.MaxSets(1)
		.DebugName("PathTracerCompositePool")
		.Create(Device);
	CompositeSet = CompositePool->allocate(CompositeLayout.get());

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

// Only ever called with nothing in flight: the helper waits for the last frame
// before it applies a batch.
void TraceRenderer::ResetScene()
{
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
		writes.AddCombinedImageSampler(DescriptorSet.get(), 6, i, WhiteView.get(), SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	writes.Execute(Device);
	auto* mapped = (vec4*)MaterialBuffer->Map(0, MaxTextures * sizeof(vec4));
	for (int i = 0; i < MaxTextures; i++)
		mapped[i] = MatteMaterial;
	MaterialBuffer->Unmap();

	DescriptorsDirty = true;
	DenoiseRestart = true;
}

void TraceRenderer::BindWhite(uint32_t index)
{
	WriteDescriptors()
		.AddCombinedImageSampler(DescriptorSet.get(), 6, (int)index, WhiteView.get(), SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.Execute(Device);
}

void TraceRenderer::SetTexture(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels, const vec4& material)
{
	if (index >= (uint32_t)MaxTextures)
		return;

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
	slot.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerSceneTexture")
		.Create(Device);
	slot.View = ImageViewBuilder().Image(slot.Image.get(), VK_FORMAT_R8G8B8A8_UNORM).DebugName("PathTracerSceneTextureView").Create(Device);

	const size_t bytes = (size_t)width * height * 4;
	auto staging = BufferBuilder()
		.Size(bytes)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("PathTracerTextureStaging")
		.Create(Device);
	memcpy(staging->Map(0, bytes), pixels, bytes);
	staging->Unmap();

	VulkanImage* image = slot.Image.get();
	VulkanBuffer* src = staging.get();
	Context->ExecuteImmediate([image, src, width, height](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { width, height, 1 };
		cmd->copyBufferToImage(src->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	WriteDescriptors()
		.AddCombinedImageSampler(DescriptorSet.get(), 6, (int)index, slot.View.get(), SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.Execute(Device);
}

void TraceRenderer::SetTexturePixels(uint32_t index, uint32_t width, uint32_t height, const uint32_t* pixels)
{
	if (index >= (uint32_t)MaxTextures || !Slots[index].Image || Slots[index].Width != width || Slots[index].Height != height)
		return;

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
void TraceRenderer::RecordTexturePixels(VulkanCommandBuffer* commands)
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
		InFlightStaging.push_back(std::move(pending.Staging));
	}
	Pending.clear();
}

void TraceRenderer::Resize(int width, int height)
{
	vkDeviceWaitIdle(Device->device);

	AccumView.reset();
	AccumImage.reset();
	HistoryView.reset();
	HistoryImage.reset();
	OutputView.reset();
	OutputImage.reset();
	MotionView.reset();
	ReflectionMotionView.reset();
	for (int i = 0; i < GuideImageCount; i++)
	{
		GuideViews[i].reset();
		GuideImages[i].reset();
	}

	AccumImage = ImageBuilder()
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerAccum")
		.Create(Device);
	AccumView = ImageViewBuilder().Image(AccumImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT).DebugName("PathTracerAccumView").Create(Device);

	// What each pixel was looking at last frame: the world position it hit and
	// which instance owned it. Compared against this frame to decide whether the
	// pixel's accumulated history still describes the same thing.
	HistoryImage = ImageBuilder()
		.Size(width, height)
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerHistory")
		.Create(Device);
	HistoryView = ImageViewBuilder().Image(HistoryImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT).DebugName("PathTracerHistoryView").Create(Device);

	OutputImage = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		.DebugName("PathTracerOutput")
		.Create(Device);
	OutputView = ImageViewBuilder().Image(OutputImage.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerOutputView").Create(Device);

	// In the trace shader's binding order. Depth and motion want the full
	// precision; the rest are colours and normals.
	for (int i = 0; i < GuideImageCount; i++)
	{
		const VkFormat format = GuideIsDepth(i) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
		GuideImages[i] = ImageBuilder()
			.Format(format)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName("PathTracerGuide")
			.Create(Device);
		GuideViews[i] = ImageViewBuilder().Image(GuideImages[i].get(), format).DebugName("PathTracerGuideView").Create(Device);
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
		VkImageView view = VK_NULL_HANDLE;
		vkCreateImageView(Device->device, &info, nullptr, &view);
		return std::unique_ptr<VulkanImageView>(new VulkanImageView(Device, view));
	};
	MotionView = motionView(GuideImages[1].get());
	ReflectionMotionView = motionView(GuideImages[9].get());

	if (Denoise)
	{
		try
		{
			Denoise->Resize(width, height);
		}
		catch (const std::exception& e)
		{
			debugf("PathTracer denoiser failed: %s", e.what());
			Denoise.reset();
			DenoiseFailed = true;
		}
	}
	DenoiseRestart = true;

	TraceWidth = width;
	TraceHeight = height;
	DescriptorsDirty = true;

	// Everything starts undefined and the shaders use it as GENERAL.
	Context->ExecuteImmediate([this](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier barrier;
		barrier
			.AddImage(AccumImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(HistoryImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		for (int i = 0; i < GuideImageCount; i++)
			barrier.AddImage(GuideImages[i].get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
		barrier.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	debugf("PathTracer buffers: %dx%d", width, height);
}

// Made the first time it is wanted, so a game that never denoises neither
// builds NRD's pipelines nor risks them failing, and made again when materials
// are switched, since that changes the shape of its first signal. A failure
// switches denoising off rather than taking the helper down with it.
void TraceRenderer::EnsureDenoiser(bool wanted, bool materials)
{
	if (Denoise && Denoise->HasSpecular() != materials)
	{
		vkDeviceWaitIdle(Device->device);
		Denoise.reset();
		DenoiseRestart = true;
		DescriptorsDirty = true;
	}

	if (!wanted || Denoise || DenoiseFailed || !TraceWidth)
		return;

	try
	{
		Denoise.reset(new Denoiser(Device, materials));
		Denoise->Resize(TraceWidth, TraceHeight);
	}
	catch (const std::exception& e)
	{
		debugf("PathTracer denoiser failed: %s", e.what());
		Denoise.reset();
		DenoiseFailed = true;
	}
	DescriptorsDirty = true;
	if (Denoise)
	{
		debugf("PathTracer denoiser: %s", Denoise->Problem());
		if (!Denoise->Available())
			DenoiseFailed = true;
	}
}

void TraceRenderer::WriteCompositeDescriptors()
{
	if (!CompositeSet || !OutputView || !Denoise || !Denoise->Available() || !Denoise->Output(0))
		return;

	WriteDescriptors()
		.AddStorageImage(CompositeSet.get(), 0, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 1, GuideViews[4].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 2, GuideViews[5].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 3, GuideViews[6].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 4, Denoise->Output(0), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 5, Denoise->Output(1), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 6, GuideViews[7].get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(CompositeSet.get(), 7, GuideViews[11].get(), VK_IMAGE_LAYOUT_GENERAL)
		// Never read without materials, but a descriptor still has to be valid.
		.AddStorageImage(CompositeSet.get(), 8, Denoise->SpecularOutput() ? Denoise->SpecularOutput() : GuideViews[10].get(), VK_IMAGE_LAYOUT_GENERAL)
		.Execute(Device);
}

void TraceRenderer::UpdateDescriptors()
{
	if (!DescriptorsDirty || !Accel->IsReady() || !AccumView || !Accel->GetInstanceDataBuffer() || !Accel->GetLightGridBuffer() || !MotionBuffer)
		return;

	WriteDescriptors writes;
	for (int i = 0; i < GuideImageCount; i++)
		writes.AddStorageImage(DescriptorSet.get(), GuideBinding(i), GuideViews[i].get(), VK_IMAGE_LAYOUT_GENERAL);
	writes.AddBuffer(DescriptorSet.get(), 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MotionBuffer.get());
	writes
		.AddAccelerationStructure(DescriptorSet.get(), 0, Accel->GetTopLevel())
		.AddStorageImage(DescriptorSet.get(), 1, AccumView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(DescriptorSet.get(), 2, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(DescriptorSet.get(), 7, HistoryView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddBuffer(DescriptorSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetAttributeBuffer())
		.AddBuffer(DescriptorSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightBuffer())
		.AddBuffer(DescriptorSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetInstanceDataBuffer())
		.AddBuffer(DescriptorSet.get(), 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightGridBuffer())
		.Execute(Device);
	WriteCompositeDescriptors();

	DescriptorsDirty = false;
}

// Last frame's camera, then each instance's last placement as three rows, in
// the order the top level structure numbers them. An instance with no last
// placement - new this frame, or one that never moves - is given its current
// one, which reads as not having moved.
void TraceRenderer::WriteMotion(const vec4 (&previousCamera)[4])
{
	const size_t count = Scene.Instances.size();
	const size_t wanted = 4 + std::max<size_t>(count, 1) * 3;
	if (!MotionBuffer || wanted > MotionCapacity)
	{
		MotionCapacity = std::max<size_t>(wanted * 2, 4 + 256 * 3);
		MotionBuffer = BufferBuilder()
			.Size(MotionCapacity * sizeof(vec4))
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.MinAlignment(256)
			.DebugName("PathTracerMotion")
			.Create(Device);
		DescriptorsDirty = true;
	}

	auto* mapped = (vec4*)MotionBuffer->Map(0, wanted * sizeof(vec4));
	for (int i = 0; i < 4; i++)
		mapped[i] = previousCamera[i];
	for (size_t i = 0; i < count; i++)
	{
		const SceneInstance& instance = Scene.Instances[i];
		const float* m = instance.HasPrevious ? instance.PreviousTransform : instance.Transform;
		for (int r = 0; r < 3; r++)
			mapped[4 + i * 3 + r] = vec4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
	}
	MotionBuffer->Unmap();
}

bool TraceRenderer::Record(VulkanCommandBuffer* commands, const TraceProtocol::TraceCommand& frame)
{
	if (frame.Width == 0 || frame.Height == 0)
		return false;
	if ((int)frame.Width != TraceWidth || (int)frame.Height != TraceHeight)
		Resize((int)frame.Width, (int)frame.Height);

	if (frame.RestartDenoiser)
		DenoiseRestart = true;
	EnsureDenoiser(frame.Denoise != 0, frame.Materials != 0);
	const bool denoising = frame.Denoise && Denoise && Denoise->Available() && frame.ViewMode == 0;

	// New shapes get a bottom level structure the first time they are seen,
	// and dynamic ones that changed are rewritten.
	Accel->SyncGeometry(Scene);
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
				.QueryType(VK_QUERY_TYPE_TIMESTAMP, TimestampCount)
				.DebugName("PathTracerTimestamps")
				.Create(Device);
			TimestampPeriodMs = props.limits.timestampPeriod * 1.0e-6;
		}
	}
	const bool timing = Timestamps && frame.Timing;
	auto stamp = [&](uint32_t index)
	{
		if (timing)
			commands->writeTimestamp(index ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Timestamps.get(), index);
	};
	if (timing)
		commands->resetQueryPool(Timestamps.get(), 0, TimestampCount);
	stamp(0);

	// Textures that generate themselves get their new frame before the trace
	// reads them.
	RecordTexturePixels(commands);

	// The top level structure is rebuilt every frame, because the movers and
	// the actors have all moved since the last one.
	Accel->HideStatic = (frame.DebugMode == 1);
	Accel->BuildTopLevel(Scene, commands);
	if (!Accel->IsReady())
		return false;
	stamp(1);

	WriteMotion(frame.PreviousCamera);
	UpdateDescriptors();

	PushConstants.CameraOrigin = frame.Camera[0];
	PushConstants.CameraRight = frame.Camera[1];
	PushConstants.CameraUp = frame.Camera[2];
	PushConstants.CameraForward = frame.Camera[3];
	PushConstants.Disable = frame.DisableBits | ((frame.ViewMode || denoising) ? 64u : 0u) | (frame.Materials ? 0u : 128u);
	PushConstants.Counts[0] = frame.Frame;
	PushConstants.Counts[1] = (uint32_t)Accel->LightCount();
	PushConstants.Counts[2] = std::min(std::max(frame.Bounces, 1u), 255u) | (std::min(frame.GlossBounces, 255u) << 8);
	PushConstants.Counts[3] = frame.AccumulatedFrames;
	PushConstants.TextureCount = CanSampleTextures ? (uint32_t)BoundTextures : 0u;
	PushConstants.MaxSamples = std::max(frame.MaxSamples, 1u);
	PushConstants.Time = frame.Time;
	PushConstants.SkyOrigin = frame.SkyOrigin;
	PushConstants.Params = vec4(
		frame.Exposure,
		frame.SkyIntensity,
		0.5f,     // ray epsilon, in world units: these levels are big
		(float)(frame.ViewMode ? frame.ViewMode : frame.DebugMode));

	commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, TracePipeline.get());
	commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, DescriptorSet.get());
	commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &PushConstants);
	commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);
	stamp(2);

	// Denoised: NRD over the trace's split lighting, then the picture rebuilt
	// from it over the one the trace wrote.
	if (denoising)
	{
		VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);

		auto cameraOf = [](const vec4* p)
		{
			Denoiser::Camera c;
			c.Origin = vec3(p[0].x, p[0].y, p[0].z);
			c.Right = vec3(p[1].x, p[1].y, p[1].z);
			c.Up = vec3(p[2].x, p[2].y, p[2].z);
			c.Forward = vec3(p[3].x, p[3].y, p[3].z);
			return c;
		};
		Denoiser::Inputs inputs[Denoiser::SignalCount];
		inputs[0].NormalRoughness = GuideViews[0].get();
		inputs[0].ViewZ = GuideViews[1].get();
		inputs[0].Motion = MotionView.get();
		inputs[0].Diffuse = GuideViews[2].get();
		inputs[0].Specular = Denoise->HasSpecular() ? GuideViews[10].get() : nullptr;
		inputs[1].NormalRoughness = GuideViews[8].get();
		inputs[1].ViewZ = GuideViews[9].get();
		inputs[1].Motion = ReflectionMotionView.get();
		inputs[1].Diffuse = GuideViews[3].get();
		Denoise->Denoise(commands, inputs, cameraOf(frame.Camera), cameraOf(frame.PreviousCamera), DenoiseRestart);
		DenoiseRestart = false;
		stamp(3);

		struct { vec4 Flash; vec4 Exposure; } finish;
		finish.Flash = vec4(frame.Camera[0].w, frame.Camera[1].w, frame.Camera[2].w, frame.Camera[3].w);
		finish.Exposure = vec4(frame.Exposure, Denoise->HasSpecular() ? 1.0f : 0.0f, 0.0f, 0.0f);
		commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, CompositePipeline.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, CompositePipelineLayout.get(), 0, CompositeSet.get());
		commands->pushConstants(CompositePipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(finish), &finish);
		commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);
	}
	else
	{
		stamp(3);
	}
	stamp(4);
	TimestampsPending = timing;
	return true;
}

void TraceRenderer::FrameCompleted()
{
	InFlightStaging.clear();

	GpuTimed = false;
	if (!TimestampsPending || !Timestamps)
		return;
	TimestampsPending = false;

	uint64_t t[TimestampCount] = {};
	if (!Timestamps->getResults(0, TimestampCount, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT))
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
bool TraceRenderer::DenoiserActive() const { return Denoise && Denoise->Available(); }
const char* TraceRenderer::DenoiserStatus() const { return Denoise ? Denoise->Problem() : (DenoiseFailed ? "failed" : "not made yet"); }
