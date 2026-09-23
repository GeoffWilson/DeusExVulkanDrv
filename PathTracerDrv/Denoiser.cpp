#include "Precomp.h"
#include "Denoiser.h"

#ifdef PATHTRACER_NRD

// NRD's own structures are laid out with the compiler's default packing; the
// engine's 4 byte packing must not reach them.
#pragma pack(push, 8)
#include <NRD.h>
#pragma pack(pop)

namespace
{
	VkFormat ToVulkan(nrd::Format format)
	{
		switch (format)
		{
		case nrd::Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
		case nrd::Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
		case nrd::Format::R8_UINT: return VK_FORMAT_R8_UINT;
		case nrd::Format::R8_SINT: return VK_FORMAT_R8_SINT;
		case nrd::Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
		case nrd::Format::RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
		case nrd::Format::RG8_UINT: return VK_FORMAT_R8G8_UINT;
		case nrd::Format::RG8_SINT: return VK_FORMAT_R8G8_SINT;
		case nrd::Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case nrd::Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
		case nrd::Format::RGBA8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
		case nrd::Format::RGBA8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
		case nrd::Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
		case nrd::Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
		case nrd::Format::R16_SNORM: return VK_FORMAT_R16_SNORM;
		case nrd::Format::R16_UINT: return VK_FORMAT_R16_UINT;
		case nrd::Format::R16_SINT: return VK_FORMAT_R16_SINT;
		case nrd::Format::R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
		case nrd::Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
		case nrd::Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
		case nrd::Format::RG16_UINT: return VK_FORMAT_R16G16_UINT;
		case nrd::Format::RG16_SINT: return VK_FORMAT_R16G16_SINT;
		case nrd::Format::RG16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
		case nrd::Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
		case nrd::Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
		case nrd::Format::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
		case nrd::Format::RGBA16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
		case nrd::Format::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case nrd::Format::R32_UINT: return VK_FORMAT_R32_UINT;
		case nrd::Format::R32_SINT: return VK_FORMAT_R32_SINT;
		case nrd::Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
		case nrd::Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
		case nrd::Format::RG32_SINT: return VK_FORMAT_R32G32_SINT;
		case nrd::Format::RG32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
		case nrd::Format::RGB32_UINT: return VK_FORMAT_R32G32B32_UINT;
		case nrd::Format::RGB32_SINT: return VK_FORMAT_R32G32B32_SINT;
		case nrd::Format::RGB32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
		case nrd::Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
		case nrd::Format::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
		case nrd::Format::RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case nrd::Format::R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		case nrd::Format::R10_G10_B10_A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
		case nrd::Format::R11_G11_B10_UFLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
		case nrd::Format::R9_G9_B9_E5_UFLOAT: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
		default: return VK_FORMAT_UNDEFINED;
		}
	}

	// NRD takes its matrices column major.
	void Store(float* out, const float m[4][4])
	{
		for (int col = 0; col < 4; col++)
			for (int row = 0; row < 4; row++)
				out[col * 4 + row] = m[row][col];
	}

	float Length(const vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
	float Dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	// View space as NRD reads it: x right, y up, z forward - left handed, the
	// way Direct3D has it. The trace's "up" vector points down the screen, so
	// view y is its negation.
	void WorldToView(const Denoiser::Camera& camera, float* out)
	{
		const float rl = Length(camera.Right), ul = Length(camera.Up), fl = Length(camera.Forward);
		const vec3 x = camera.Right * (1.0f / rl);
		const vec3 y = camera.Up * (-1.0f / ul);
		const vec3 z = camera.Forward * (1.0f / fl);
		const float m[4][4] = {
			{ x.x, x.y, x.z, -Dot(x, camera.Origin) },
			{ y.x, y.y, y.z, -Dot(y, camera.Origin) },
			{ z.x, z.y, z.z, -Dot(z, camera.Origin) },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		};
		Store(out, m);
	}

	// The projection the trace's rays amount to: a point at view (x, y, z)
	// lands at x / (z * |right|) across the screen, both ways from the middle.
	// Depth is a conventional left handed range; NRD only needs it to be one.
	void ViewToClip(const Denoiser::Camera& camera, float* out)
	{
		const float n = 1.0f, f = 100000.0f;
		const float m[4][4] = {
			{ 1.0f / Length(camera.Right), 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f / Length(camera.Up), 0.0f, 0.0f },
			{ 0.0f, 0.0f, f / (f - n), -n * f / (f - n) },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
		};
		Store(out, m);
	}
}

struct Denoiser::Impl
{
	VulkanDevice* Device = nullptr;
	nrd::Instance* Nrd = nullptr;
	const nrd::InstanceDesc* Desc = nullptr;
	nrd::LibraryDesc Library = {};

	// Set 1 (NRD's constant buffer and samplers space): one set, written once.
	VkDescriptorSetLayout SharedLayout = VK_NULL_HANDLE;
	VkDescriptorPool SharedPool = VK_NULL_HANDLE;
	VkDescriptorSet SharedSet = VK_NULL_HANDLE;
	std::vector<VkSampler> Samplers;

	// Per pipeline: its resources set layout, pipeline layout and pipeline.
	std::vector<VkDescriptorSetLayout> ResourceLayouts;
	std::vector<VkPipelineLayout> Layouts;
	std::vector<VkPipeline> Pipelines;

	// The resources sets, allocated afresh each frame.
	VkDescriptorPool FramePool = VK_NULL_HANDLE;

	// Constants, a slice per dispatch. Host visible and rewritten every frame,
	// which is safe because the device waits for the previous frame first.
	std::unique_ptr<VulkanBuffer> Constants;
	uint32_t ConstantStride = 0;
	uint32_t ConstantSlices = 0;

	struct Texture
	{
		std::unique_ptr<VulkanImage> Image;
		std::unique_ptr<VulkanImageView> View;
	};
	std::vector<Texture> Pool;   // the permanent pool, then the transient
	Texture Outputs[SignalCount];
	Texture SpecularOutput;   // the first signal's glossy reflection
	bool NeedsLayout = true;

	int Width = 0, Height = 0;
	uint32_t FrameIndex = 0;

	Texture MakeTexture(VkFormat format, int width, int height, const char* name)
	{
		Texture t;
		t.Image = ImageBuilder()
			.Format(format)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName(name)
			.Create(Device);
		t.View = ImageViewBuilder().Image(t.Image.get(), format).DebugName(name).Create(Device);
		return t;
	}

	~Impl()
	{
		VkDevice d = Device->device;
		for (VkPipeline p : Pipelines) vkDestroyPipeline(d, p, nullptr);
		for (VkPipelineLayout l : Layouts) vkDestroyPipelineLayout(d, l, nullptr);
		for (VkDescriptorSetLayout l : ResourceLayouts) vkDestroyDescriptorSetLayout(d, l, nullptr);
		if (FramePool) vkDestroyDescriptorPool(d, FramePool, nullptr);
		if (SharedPool) vkDestroyDescriptorPool(d, SharedPool, nullptr);
		if (SharedLayout) vkDestroyDescriptorSetLayout(d, SharedLayout, nullptr);
		for (VkSampler s : Samplers) vkDestroySampler(d, s, nullptr);
		if (Nrd) nrd::DestroyInstance(*Nrd);
	}
};

Denoiser::Denoiser(VulkanDevice* device, bool specular) : I(std::make_unique<Impl>()), Specular(specular)
{
	I->Device = device;
	VkDevice d = device->device;

	if (!device->EnabledFeatures.Features.shaderStorageImageWriteWithoutFormat)
	{
		Status = "the device cannot write storage images without a format";
		return;
	}

	const nrd::DenoiserDesc denoisers[SignalCount] = {
		{ 0, specular ? nrd::Denoiser::RELAX_DIFFUSE_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE },
		{ 1, nrd::Denoiser::RELAX_DIFFUSE } };
	nrd::InstanceCreationDesc creation = {};
	creation.denoisers = denoisers;
	creation.denoisersNum = SignalCount;
	if (nrd::CreateInstance(creation, I->Nrd) != nrd::Result::SUCCESS)
	{
		Status = "NRD would not create an instance";
		return;
	}
	I->Desc = nrd::GetInstanceDesc(*I->Nrd);
	I->Library = *nrd::GetLibraryDesc();
	const nrd::InstanceDesc& desc = *I->Desc;
	const nrd::SPIRVBindingOffsets& offsets = I->Library.spirvBindingOffsets;

	// The two register spaces become two descriptor sets, numbered as the
	// spaces are. Written for NRD's current arrangement of them.
	if (desc.resourcesSpaceIndex > 1 || desc.constantBufferAndSamplersSpaceIndex > 1 ||
		desc.resourcesSpaceIndex == desc.constantBufferAndSamplersSpaceIndex)
	{
		Status = "NRD's register spaces are not the two this expects";
		return;
	}

	// Samplers and the constant buffer.
	std::vector<VkDescriptorSetLayoutBinding> shared;
	for (uint32_t i = 0; i < desc.samplersNum; i++)
	{
		VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		const bool nearest = desc.samplers[i] == nrd::Sampler::NEAREST_CLAMP;
		info.magFilter = info.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.maxLod = 16.0f;
		VkSampler sampler = VK_NULL_HANDLE;
		vkCreateSampler(d, &info, nullptr, &sampler);
		I->Samplers.push_back(sampler);

		VkDescriptorSetLayoutBinding b = {};
		b.binding = offsets.samplerOffset + desc.samplersBaseRegisterIndex + i;
		b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		shared.push_back(b);
	}
	const uint32_t constantBinding = offsets.constantBufferOffset + desc.constantBufferRegisterIndex;
	{
		VkDescriptorSetLayoutBinding b = {};
		b.binding = constantBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		shared.push_back(b);
	}
	VkDescriptorSetLayoutCreateInfo sharedInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	sharedInfo.bindingCount = (uint32_t)shared.size();
	sharedInfo.pBindings = shared.data();
	vkCreateDescriptorSetLayout(d, &sharedInfo, nullptr, &I->SharedLayout);

	// A slice of constants per dispatch, aligned as the device needs.
	const uint32_t alignment = (uint32_t)std::max<VkDeviceSize>(device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment, 16);
	I->ConstantStride = (desc.constantBufferMaxDataSize + alignment - 1) / alignment * alignment;
	I->ConstantSlices = 64;
	I->Constants = BufferBuilder()
		.Size((size_t)I->ConstantStride * I->ConstantSlices)
		.Usage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.DebugName("PathTracerDenoiserConstants")
		.Create(device);

	{
		VkDescriptorPoolSize sizes[] = {
			{ VK_DESCRIPTOR_TYPE_SAMPLER, std::max(desc.samplersNum, 1u) },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
		};
		VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		info.maxSets = 1;
		info.poolSizeCount = 2;
		info.pPoolSizes = sizes;
		vkCreateDescriptorPool(d, &info, nullptr, &I->SharedPool);

		VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		alloc.descriptorPool = I->SharedPool;
		alloc.descriptorSetCount = 1;
		alloc.pSetLayouts = &I->SharedLayout;
		vkAllocateDescriptorSets(d, &alloc, &I->SharedSet);

		std::vector<VkDescriptorImageInfo> samplerInfos(desc.samplersNum);
		std::vector<VkWriteDescriptorSet> writes;
		for (uint32_t i = 0; i < desc.samplersNum; i++)
		{
			samplerInfos[i].sampler = I->Samplers[i];
			VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			w.dstSet = I->SharedSet;
			w.dstBinding = offsets.samplerOffset + desc.samplersBaseRegisterIndex + i;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			w.pImageInfo = &samplerInfos[i];
			writes.push_back(w);
		}
		VkDescriptorBufferInfo bufferInfo = { I->Constants->buffer, 0, desc.constantBufferMaxDataSize };
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		w.dstSet = I->SharedSet;
		w.dstBinding = constantBinding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w.pBufferInfo = &bufferInfo;
		writes.push_back(w);
		vkUpdateDescriptorSets(d, (uint32_t)writes.size(), writes.data(), 0, nullptr);
	}

	// A tight layout per pipeline: its sampled inputs, then its outputs, each
	// a binding of its own in register order.
	for (uint32_t p = 0; p < desc.pipelinesNum; p++)
	{
		const nrd::PipelineDesc& pipeline = desc.pipelines[p];
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		for (uint32_t r = 0; r < pipeline.resourceRangesNum; r++)
		{
			const nrd::ResourceRangeDesc& range = pipeline.resourceRanges[r];
			const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
			const uint32_t base = (storage ? offsets.storageTextureAndBufferOffset : offsets.textureOffset) + desc.resourcesBaseRegisterIndex;
			for (uint32_t k = 0; k < range.descriptorsNum; k++)
			{
				VkDescriptorSetLayoutBinding b = {};
				b.binding = base + k;
				b.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				b.descriptorCount = 1;
				b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				bindings.push_back(b);
			}
		}
		VkDescriptorSetLayout resources = VK_NULL_HANDLE;
		VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layoutInfo.bindingCount = (uint32_t)bindings.size();
		layoutInfo.pBindings = bindings.data();
		vkCreateDescriptorSetLayout(d, &layoutInfo, nullptr, &resources);
		I->ResourceLayouts.push_back(resources);

		VkDescriptorSetLayout sets[2];
		sets[desc.resourcesSpaceIndex] = resources;
		sets[desc.constantBufferAndSamplersSpaceIndex] = I->SharedLayout;
		VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayoutInfo.setLayoutCount = 2;
		pipelineLayoutInfo.pSetLayouts = sets;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		vkCreatePipelineLayout(d, &pipelineLayoutInfo, nullptr, &layout);
		I->Layouts.push_back(layout);

		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = (size_t)pipeline.computeShaderSPIRV.size;
		moduleInfo.pCode = (const uint32_t*)pipeline.computeShaderSPIRV.bytecode;
		VkShaderModule module = VK_NULL_HANDLE;
		vkCreateShaderModule(d, &moduleInfo, nullptr, &module);

		VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pipelineInfo.stage.module = module;
		pipelineInfo.stage.pName = desc.shaderEntryPoint;
		pipelineInfo.layout = layout;
		VkPipeline created = VK_NULL_HANDLE;
		const VkResult result = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &created);
		vkDestroyShaderModule(d, module, nullptr);
		I->Pipelines.push_back(created);
		if (result != VK_SUCCESS)
		{
			debugf(TEXT("PathTracer denoiser: pipeline %d (%S) failed: %d"), (int)p, pipeline.shaderIdentifier, (int)result);
			Status = "an NRD pipeline would not build";
			return;
		}
	}

	// Enough for a frame's dispatches, with room to spare.
	{
		const nrd::DescriptorPoolDesc& pool = desc.descriptorPoolDesc;
		VkDescriptorPoolSize sizes[] = {
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(pool.totalTexturesNum * 2, 1u) },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, std::max(pool.totalStorageTexturesNum * 2, 1u) },
		};
		VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		info.maxSets = std::max(pool.setsMaxNum * 2, 1u);
		info.poolSizeCount = 2;
		info.pPoolSizes = sizes;
		vkCreateDescriptorPool(d, &info, nullptr, &I->FramePool);
	}

	Instance = I->Nrd;
	Status = "ready";
	debugf(TEXT("PathTracer denoiser: NRD %d.%d.%d ReLAX, %d pipelines, %d + %d pool textures"),
		(int)I->Library.versionMajor, (int)I->Library.versionMinor, (int)I->Library.versionBuild,
		(int)desc.pipelinesNum, (int)desc.permanentPoolSize, (int)desc.transientPoolSize);
}

Denoiser::~Denoiser()
{
}

void Denoiser::Resize(int width, int height)
{
	if (!Available() || (width == I->Width && height == I->Height))
		return;

	const nrd::InstanceDesc& desc = *I->Desc;
	I->Pool.clear();
	auto addPool = [&](const nrd::TextureDesc* textures, uint32_t count)
	{
		for (uint32_t i = 0; i < count; i++)
		{
			const int factor = std::max<int>(textures[i].downsampleFactor, 1);
			I->Pool.push_back(I->MakeTexture(ToVulkan(textures[i].format),
				(width + factor - 1) / factor, (height + factor - 1) / factor, "PathTracerDenoiserPool"));
		}
	};
	addPool(desc.permanentPool, desc.permanentPoolSize);
	addPool(desc.transientPool, desc.transientPoolSize);
	for (int i = 0; i < SignalCount; i++)
		I->Outputs[i] = I->MakeTexture(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, "PathTracerDenoiserOutput");
	if (Specular)
		I->SpecularOutput = I->MakeTexture(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, "PathTracerDenoiserSpecularOutput");
	else
		I->SpecularOutput = Impl::Texture();

	I->Width = width;
	I->Height = height;
	I->NeedsLayout = true;
}

VulkanImageView* Denoiser::Output(int signal) const { return I->Outputs[signal].View.get(); }
VulkanImageView* Denoiser::SpecularOutput() const { return I->SpecularOutput.View ? I->SpecularOutput.View.get() : nullptr; }

void Denoiser::Denoise(VulkanCommandBuffer* commands, const Inputs (&allInputs)[SignalCount], const Camera& now, const Camera& previous, bool restart)
{
	if (!Available() || !I->Width)
		return;

	VkDevice d = I->Device->device;
	const nrd::InstanceDesc& desc = *I->Desc;
	const nrd::SPIRVBindingOffsets& offsets = I->Library.spirvBindingOffsets;

	// New textures start undefined; NRD clears them itself when told the
	// history is garbage.
	bool clear = false;
	if (I->NeedsLayout)
	{
		PipelineBarrier barrier;
		for (auto& t : I->Pool)
			barrier.AddImage(t.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		for (auto& t : I->Outputs)
			barrier.AddImage(t.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		if (I->SpecularOutput.Image)
			barrier.AddImage(I->SpecularOutput.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		barrier.Execute(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		I->NeedsLayout = false;
		clear = true;
	}

	nrd::CommonSettings common = {};
	ViewToClip(now, common.viewToClipMatrix);
	ViewToClip(previous, common.viewToClipMatrixPrev);
	WorldToView(now, common.worldToViewMatrix);
	WorldToView(previous, common.worldToViewMatrixPrev);
	common.motionVectorScale[0] = 1.0f;
	common.motionVectorScale[1] = 1.0f;
	common.motionVectorScale[2] = 0.0f;
	common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = (uint16_t)I->Width;
	common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = (uint16_t)I->Height;
	// The trace reaches 100000 units; anything it marks as having no surface
	// sits beyond this.
	common.denoisingRange = 200000.0f;
	common.frameIndex = I->FrameIndex++;
	common.accumulationMode = clear ? nrd::AccumulationMode::CLEAR_AND_RESTART
		: (restart ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE);
	nrd::SetCommonSettings(*I->Nrd, common);

	nrd::RelaxSettings relax = {};
	for (int signal = 0; signal < SignalCount; signal++)
		nrd::SetDenoiserSettings(*I->Nrd, (nrd::Identifier)signal, &relax);

	vkResetDescriptorPool(d, I->FramePool, 0);
	uint8_t* constants = (uint8_t*)I->Constants->Map(0, (size_t)I->ConstantStride * I->ConstantSlices);
	uint32_t slice = 0;
	uint32_t constantOffset = 0;

	// One signal at a time, each against its own images. NRD's own
	// integration does the same when the signals' inputs differ.
	for (int signal = 0; signal < SignalCount; signal++)
	{
		const Inputs& inputs = allInputs[signal];
		const nrd::Identifier identifier = (nrd::Identifier)signal;
		const nrd::DispatchDesc* dispatches = nullptr;
		uint32_t dispatchCount = 0;
		nrd::GetComputeDispatches(*I->Nrd, &identifier, 1, dispatches, dispatchCount);

		auto viewFor = [&](const nrd::ResourceDesc& r) -> VkImageView
		{
			switch (r.type)
			{
			case nrd::ResourceType::PERMANENT_POOL: return I->Pool[r.indexInPool].View->view;
			case nrd::ResourceType::TRANSIENT_POOL: return I->Pool[desc.permanentPoolSize + r.indexInPool].View->view;
			case nrd::ResourceType::IN_MV: return inputs.Motion->view;
			case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return inputs.NormalRoughness->view;
			case nrd::ResourceType::IN_VIEWZ: return inputs.ViewZ->view;
			case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return inputs.Diffuse->view;
			case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return I->Outputs[signal].View->view;
			case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return inputs.Specular ? inputs.Specular->view : VK_NULL_HANDLE;
			case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return I->SpecularOutput.View ? I->SpecularOutput.View->view : VK_NULL_HANDLE;
			default: return VK_NULL_HANDLE;
			}
		};

		for (uint32_t n = 0; n < dispatchCount; n++)
		{
			const nrd::DispatchDesc& dispatch = dispatches[n];
			const nrd::PipelineDesc& pipeline = desc.pipelines[dispatch.pipelineIndex];

			VkDescriptorSet set = VK_NULL_HANDLE;
			VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			alloc.descriptorPool = I->FramePool;
			alloc.descriptorSetCount = 1;
			alloc.pSetLayouts = &I->ResourceLayouts[dispatch.pipelineIndex];
			if (vkAllocateDescriptorSets(d, &alloc, &set) != VK_SUCCESS)
				break;

			std::vector<VkDescriptorImageInfo> images(dispatch.resourcesNum);
			std::vector<VkWriteDescriptorSet> writes;
			uint32_t index = 0;
			bool missing = false;
			for (uint32_t r = 0; r < pipeline.resourceRangesNum; r++)
			{
				const nrd::ResourceRangeDesc& range = pipeline.resourceRanges[r];
				const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
				const uint32_t base = (storage ? offsets.storageTextureAndBufferOffset : offsets.textureOffset) + desc.resourcesBaseRegisterIndex;
				for (uint32_t k = 0; k < range.descriptorsNum; k++, index++)
				{
					images[index].imageView = viewFor(dispatch.resources[index]);
					images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					missing |= images[index].imageView == VK_NULL_HANDLE;
					VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
					w.dstSet = set;
					w.dstBinding = base + k;
					w.descriptorCount = 1;
					w.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
					w.pImageInfo = &images[index];
					writes.push_back(w);
				}
			}
			if (missing)
			{
				static bool logged = false;
				if (!logged)
				{
					logged = true;
					debugf(TEXT("PathTracer denoiser: dispatch %S wants a resource this does not supply"), dispatch.name ? dispatch.name : "?");
				}
				break;
			}
			vkUpdateDescriptorSets(d, (uint32_t)writes.size(), writes.data(), 0, nullptr);

			if (dispatch.constantBufferDataSize && !dispatch.constantBufferDataMatchesPreviousDispatch && slice < I->ConstantSlices)
			{
				constantOffset = slice * I->ConstantStride;
				memcpy(constants + constantOffset, dispatch.constantBufferData, dispatch.constantBufferDataSize);
				slice++;
			}

			// Every dispatch reads what the one before it wrote.
			VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);

			vkCmdBindPipeline(commands->buffer, VK_PIPELINE_BIND_POINT_COMPUTE, I->Pipelines[dispatch.pipelineIndex]);
			vkCmdBindDescriptorSets(commands->buffer, VK_PIPELINE_BIND_POINT_COMPUTE, I->Layouts[dispatch.pipelineIndex],
				desc.resourcesSpaceIndex, 1, &set, 0, nullptr);
			vkCmdBindDescriptorSets(commands->buffer, VK_PIPELINE_BIND_POINT_COMPUTE, I->Layouts[dispatch.pipelineIndex],
				desc.constantBufferAndSamplersSpaceIndex, 1, &I->SharedSet, 1, &constantOffset);
			vkCmdDispatch(commands->buffer, dispatch.gridWidth, dispatch.gridHeight, 1);
		}
	}
	I->Constants->Unmap();

	// What NRD wrote, for whatever reads it next.
	VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

#else

struct Denoiser::Impl {};

Denoiser::Denoiser(VulkanDevice*, bool specular) : I(std::make_unique<Impl>()), Specular(specular) { Status = "built without NRD (see cmake/build-nrd.sh)"; }
Denoiser::~Denoiser() {}
void Denoiser::Resize(int, int) {}
void Denoiser::Denoise(VulkanCommandBuffer*, const Inputs (&)[SignalCount], const Camera&, const Camera&, bool) {}
VulkanImageView* Denoiser::Output(int) const { return nullptr; }
VulkanImageView* Denoiser::SpecularOutput() const { return nullptr; }

#endif
