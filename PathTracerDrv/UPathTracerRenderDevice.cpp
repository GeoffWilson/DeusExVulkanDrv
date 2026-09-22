#include "Precomp.h"
#include "UPathTracerRenderDevice.h"
#include "Shaders.h"
#include <stdexcept>
#include <chrono>

static double NowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

IMPLEMENT_CLASS(UPathTracerRenderDevice);

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	debugf(TEXT("[%s] %s"), appFromAnsi(typestr), appFromAnsi(msg.c_str()));
}

void VulkanError(const char* text)
{
	throw std::runtime_error(text);
}

UPathTracerRenderDevice::UPathTracerRenderDevice()
{
}

void UPathTracerRenderDevice::StaticConstructor()
{
	guard(UPathTracerRenderDevice::StaticConstructor);

	SpanBased = 0;
	FullscreenOnly = 0;
	SupportsFogMaps = 0;
	SupportsDistanceFog = 0;
	SupportsTC = 0;
	PrecacheOnFlip = 0;
	SupportsLazyTextures = 0;

	Bounces = 3;
	Exposure = 128;
	SkyIntensity = 128;
	MaxAccumulatedFrames = 256;
	VkDeviceIndex = 0;
	VkDebug = 0;
	DebugMode = 0;
	LightScale = 100;
	UseVSync = 1;
	LogTimings = 0;

	new(GetClass(), TEXT("Bounces"), RF_Public) UIntProperty(CPP_PROPERTY(Bounces), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("Exposure"), RF_Public) UByteProperty(CPP_PROPERTY(Exposure), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("SkyIntensity"), RF_Public) UByteProperty(CPP_PROPERTY(SkyIntensity), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("MaxAccumulatedFrames"), RF_Public) UIntProperty(CPP_PROPERTY(MaxAccumulatedFrames), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("VkDeviceIndex"), RF_Public) UIntProperty(CPP_PROPERTY(VkDeviceIndex), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("VkDebug"), RF_Public) UBoolProperty(CPP_PROPERTY(VkDebug), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("DebugMode"), RF_Public) UIntProperty(CPP_PROPERTY(DebugMode), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("LightScale"), RF_Public) UIntProperty(CPP_PROPERTY(LightScale), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("UseVSync"), RF_Public) UBoolProperty(CPP_PROPERTY(UseVSync), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("LogTimings"), RF_Public) UBoolProperty(CPP_PROPERTY(LogTimings), TEXT("Display"), CPF_Config);

	unguard;
}

UBOOL UPathTracerRenderDevice::Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	guard(UPathTracerRenderDevice::Init);

	Viewport = InViewport;

	try
	{
		Instance = VulkanInstanceBuilder()
			.RequireSurfaceExtensions()
			.DebugLayer(VkDebug)
			.Create();

		Surface = VulkanSurfaceBuilder()
			.Win32Window((HWND)Viewport->GetWindow())
			.Create(Instance);

		auto deviceBuilder = VulkanDeviceBuilder();
		deviceBuilder.Surface(Surface);
		// Asked for rather than required. Requiring them makes device selection
		// fail with "no device meets the minimum requirements", which says
		// nothing about which requirement or why - and the usual reason here is
		// not the GPU at all but what the translation layer chose to pass on.
		deviceBuilder.OptionalRayQuery();
		// Needed to index the texture array by what the ray happened to hit,
		// which differs between neighbouring invocations. Optional: without it
		// the trace falls back to one averaged colour per surface.
		deviceBuilder.OptionalDescriptorIndexing();
		deviceBuilder.SelectDevice(VkDeviceIndex);
		Device = deviceBuilder.Create(Instance);

		// Texturing needs to index the array by whatever each ray hit, which is
		// not uniform across a workgroup. Without it the trace still runs.
		CanSampleTextures =
			Device->EnabledFeatures.DescriptorIndexing.runtimeDescriptorArray &&
			Device->EnabledFeatures.DescriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
		if (!CanSampleTextures)
			debugf(TEXT("PathTracer: no descriptor indexing, surfaces will use one averaged colour each."));

		if (!Device->EnabledFeatures.RayQuery.rayQuery || !Device->EnabledFeatures.AccelerationStructure.accelerationStructure)
		{
			debugf(TEXT("PathTracerDrv needs VK_KHR_ray_query and VK_KHR_acceleration_structure, which this device did not offer."));
			debugf(TEXT("The GPU almost certainly supports them. Whether a 32 bit client is told about them is a separate question:"));
			debugf(TEXT("  - Proton does not pass them through to 32 bit clients (every build tested, as of 2026-09)."));
			debugf(TEXT("  - Upstream wine 11.17 does."));
			debugf(TEXT("  - Native Windows uses the driver directly and has no such layer in the way."));
			debugf(TEXT("spike/vkrtcheck.cpp reports what any given environment actually offers."));
			Exit();
			return 0;
		}

		const auto& props = Device->PhysicalDevice.Properties.Properties;
		debugf(TEXT("PathTracer device: %s"), appFromAnsi(props.deviceName));

		SwapChain = VulkanSwapChainBuilder().Create(Device.get());

		CommandPool = CommandPoolBuilder()
			.QueueFamily(Device->GraphicsFamily)
			.DebugName("PathTracerCommandPool")
			.Create(Device.get());

		RenderFinishedFence = FenceBuilder().DebugName("PathTracerFence").Create(Device.get());
		ImageAvailableSemaphore = SemaphoreBuilder().DebugName("PathTracerImageAvailable").Create(Device.get());
		RenderFinishedSemaphore = SemaphoreBuilder().DebugName("PathTracerRenderFinished").Create(Device.get());

		Accel.reset(new AccelStructure(this));
		Textures.reset(new TextureCache(this));

		CreateTracePipeline();
		CreateTilePipeline();

		// Says the shaders compiled and the pipelines exist. Without it a
		// failure and a successful start that simply never rendered a level
		// look the same in the log: device named, then nothing.
		debugf(TEXT("PathTracer ready"));
	}
	catch (const std::exception& e)
	{
		debugf(TEXT("Could not create the path tracer: %s"), appFromAnsi(e.what()));
		Exit();
		return 0;
	}

	if (!SetRes(NewX, NewY, NewColorBytes, Fullscreen))
	{
		Exit();
		return 0;
	}

	return 1;
	unguard;
}

void UPathTracerRenderDevice::CreateTracePipeline()
{
	DescriptorLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LevelScene::MaxTextures, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerSetLayout")
		.Create(Device.get());

	DescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LevelScene::MaxTextures)
		.MaxSets(1)
		.DebugName("PathTracerDescriptorPool")
		.Create(Device.get());

	DescriptorSetOwner = DescriptorPool->allocate(DescriptorLayout.get());
	DescriptorSet = DescriptorSetOwner.get();

	PipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(DescriptorLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants))
		.DebugName("PathTracerPipelineLayout")
		.Create(Device.get());

	const char* prologue =
		"#version 460\r\n"
		"#extension GL_EXT_ray_query : enable\r\n";

	TraceShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Trace.comp", Shaders::Trace())
		.DebugName("PathTracerTrace")
		.Create("PathTracerTrace", Device.get());
	(void)prologue;

	TracePipeline = ComputePipelineBuilder()
		.Layout(PipelineLayout.get())
		.ComputeShader(TraceShader.get())
		.DebugName("PathTracerTracePipeline")
		.Create(Device.get());
}

std::unique_ptr<VulkanDescriptorSet> UPathTracerRenderDevice::AllocateTileDescriptorSet(VulkanImageView* view)
{
	auto set = TileDescriptorPool->allocate(TileSetLayout.get());
	WriteDescriptors()
		.AddCombinedImageSampler(set.get(), 0, view, TileSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.Execute(Device.get());
	return set;
}

void UPathTracerRenderDevice::CreateTilePipeline()
{
	SceneSampler = SamplerBuilder()
		.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT)
		.DebugName("PathTracerSceneSampler")
		.Create(Device.get());

	TileSampler = SamplerBuilder()
		.MinFilter(VK_FILTER_LINEAR)
		.MagFilter(VK_FILTER_LINEAR)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
		.DebugName("PathTracerTileSampler")
		.Create(Device.get());

	TileSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("PathTracerTileSetLayout")
		.Create(Device.get());

	// One set per cached texture. A generous ceiling: a Deus Ex menu touches a
	// few hundred distinct textures at most, and the pool is only reset when the
	// cache is flushed.
	TileDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096)
		.MaxSets(4096)
		.DebugName("PathTracerTileDescriptorPool")
		.Create(Device.get());

	TilePipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(TileSetLayout.get())
		.DebugName("PathTracerTilePipelineLayout")
		.Create(Device.get());

	// The traced image is loaded rather than cleared: the tiles go on top of it.
	//
	// GENERAL throughout rather than COLOR_ATTACHMENT_OPTIMAL. The output image
	// is written by a compute dispatch, drawn into here, and then blitted, and
	// the blit's barrier names GENERAL as the layout it starts from. Leaving the
	// pass in COLOR_ATTACHMENT_OPTIMAL made that barrier declare a layout the
	// image was not in, which leaves the contents undefined - a black frame
	// whenever the driver acted on it rather than ignoring it. GENERAL is valid
	// for a colour attachment and keeps one layout across the whole frame.
	TileRenderPass = RenderPassBuilder()
		.AddAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_GENERAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_GENERAL)
		.DebugName("PathTracerTileRenderPass")
		.Create(Device.get());

	TileVertexShader = ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource("shaders/Tile.vert", Shaders::TileVertex())
		.DebugName("PathTracerTileVertex")
		.Create("PathTracerTileVertex", Device.get());

	TileFragmentShader = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/Tile.frag", Shaders::TileFragment())
		.DebugName("PathTracerTileFragment")
		.Create("PathTracerTileFragment", Device.get());

	for (int mode = 0; mode < 3; mode++)
	{
		GraphicsPipelineBuilder builder;
		builder.AddVertexShader(TileVertexShader.get());
		builder.AddFragmentShader(TileFragmentShader.get());
		builder.AddVertexBufferBinding(0, sizeof(TileVertex));
		builder.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TileVertex, Position));
		builder.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TileVertex, TexCoord));
		builder.AddVertexAttribute(2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TileVertex, Color));
		builder.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
		builder.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);
		builder.Layout(TilePipelineLayout.get());
		builder.RenderPass(TileRenderPass.get());
		builder.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		builder.Cull(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
		builder.DepthStencilEnable(false, false, false);

		// The three ways this engine composites 2D art.
		VkPipelineColorBlendAttachmentState blend = {};
		blend.blendEnable = VK_TRUE;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		if (mode == 1)
		{
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		}
		else if (mode == 2)
		{
			// Modulate 2x, not a plain multiply: the result is
			// src*dst + dst*src, so mid grey is the identity and a modulated
			// texture's neutral areas leave the background alone. A plain
			// multiply halves it instead, which shows up as a dark rectangle
			// the size of the tile - the mouse cursor being the obvious one.
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
			blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
		}
		else
		{
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}
		builder.AddColorBlendAttachment(blend);

		builder.DebugName("PathTracerTilePipeline");
		TilePipelines[mode] = builder.Create(Device.get());
	}
}

// Draw the frame's collected tiles over the traced image.
void UPathTracerRenderDevice::RenderTiles(VulkanCommandBuffer* commands)
{
	if (TileVertices.empty() || !TileFramebuffer)
		return;

	const size_t byteSize = TileVertices.size() * sizeof(TileVertex);
	if (!TileVertexBuffer || TileVertexCapacity < byteSize)
	{
		// Grown rather than sized exactly, so a busy menu does not reallocate
		// every frame.
		TileVertexCapacity = byteSize * 2;
		TileVertexBuffer = BufferBuilder()
			.Size(TileVertexCapacity)
			.Usage(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("PathTracerTileVertices")
			.Create(Device.get());
	}

	void* mapped = TileVertexBuffer->Map(0, byteSize);
	memcpy(mapped, TileVertices.data(), byteSize);
	TileVertexBuffer->Unmap();

	// The trace wrote this image; the tiles are about to read and blend over it.
	// The layout does not change - only the ordering and visibility do.
	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	RenderPassBegin()
		.RenderPass(TileRenderPass.get())
		.Framebuffer(TileFramebuffer.get())
		.RenderArea(0, 0, TraceWidth, TraceHeight)
		.Execute(commands);

	VkViewport viewport = {};
	viewport.width = (float)TraceWidth;
	viewport.height = (float)TraceHeight;
	viewport.maxDepth = 1.0f;
	commands->setViewport(0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.extent.width = TraceWidth;
	scissor.extent.height = TraceHeight;
	commands->setScissor(0, 1, &scissor);

	VkBuffer vertexBuffers[] = { TileVertexBuffer->buffer };
	VkDeviceSize offsets[] = { 0 };
	commands->bindVertexBuffers(0, 1, vertexBuffers, offsets);

	int boundMode = -1;
	for (const TileBatch& batch : TileBatches)
	{
		if (!batch.Texture || !batch.Texture->Set || batch.VertexCount == 0)
			continue;

		if (batch.BlendMode != boundMode)
		{
			commands->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, TilePipelines[batch.BlendMode].get());
			boundMode = batch.BlendMode;
		}

		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, TilePipelineLayout.get(), 0, batch.Texture->Set.get());
		commands->draw(batch.VertexCount, 1, batch.FirstVertex, 0);
	}

	commands->endRenderPass();

	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// Keep the engine's cursor clip on the window we actually have.
//
// The engine clips the pointer to the window as it stands when ResizeViewport
// captures the mouse, but the fullscreen branch below restyles and moves that
// window afterwards, leaving the clip describing the rectangle it used to
// occupy. The engine goes on recentring the pointer in the middle of the window
// it now has, and that recentre is clamped to the stale rectangle, so the
// difference comes back as mouse movement every frame.
static void ReclipCursorToWindow(HWND hWnd)
{
	RECT clip = {};
	if (!GetClipCursor(&clip))
		return;

	RECT desktop =
	{
		GetSystemMetrics(SM_XVIRTUALSCREEN),
		GetSystemMetrics(SM_YVIRTUALSCREEN),
		GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
		GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
	};
	if (EqualRect(&clip, &desktop))
		return;

	RECT client = {};
	GetClientRect(hWnd, &client);
	MapWindowPoints(hWnd, nullptr, (POINT*)&client, 2);
	ClipCursor(&client);
}

class PathTracerSetResLock
{
public:
	PathTracerSetResLock(bool& value) : value(value) { value = true; }
	~PathTracerSetResLock() { value = false; }
	bool& value;
};

UBOOL UPathTracerRenderDevice::SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	guard(UPathTracerRenderDevice::SetRes);

	// The engine can re-enter this while the viewport is being resized.
	if (InSetResCall)
		return TRUE;
	PathTracerSetResLock lock(InSetResCall);

	if (NewX == 0 || NewY == 0)
		return 1;

	HWND window = (HWND)Viewport->GetWindow();

	if (!Fullscreen && FullscreenState.Enabled) // Leaving fullscreen
	{
		SetWindowLong(window, GWL_STYLE, FullscreenState.Style);
		SetWindowLong(window, GWL_EXSTYLE, FullscreenState.ExStyle);
		SetWindowPos(
			window,
			HWND_TOP,
			FullscreenState.WindowPos.left,
			FullscreenState.WindowPos.top,
			FullscreenState.WindowPos.right - FullscreenState.WindowPos.left,
			FullscreenState.WindowPos.bottom - FullscreenState.WindowPos.top,
			SWP_FRAMECHANGED | SWP_NOSENDCHANGING | SWP_NOACTIVATE | SWP_NOZORDER);

		FullscreenState.Enabled = false;
	}

	// Read the windowed state before the resize, not after: the engine's own
	// fullscreen handling moves and restyles the window as part of it, so
	// reading afterwards saves the fullscreen geometry as the one to go back to.
	const bool enteringFullscreen = Fullscreen && !FullscreenState.Enabled;
	if (enteringFullscreen)
	{
		GetWindowRect(window, &FullscreenState.WindowPos);
		FullscreenState.Style = GetWindowLong(window, GWL_STYLE);
		FullscreenState.ExStyle = GetWindowLong(window, GWL_EXSTYLE);
	}

	// BLIT_Fullscreen even though the window below is only borderless. The engine
	// keys a good deal off believing it is fullscreen - input capture, the
	// pointer clip, and whether the system cursor is shown - so telling it
	// otherwise leaves the desktop cursor on screen underneath the one the game
	// draws itself. The actual display mode change it asks for is undone by the
	// restyle that follows; UseDirectDraw=False in DeusEx.ini removes it
	// entirely, which a device presenting through Vulkan has no use for.
	if (!Viewport->ResizeViewport(Fullscreen ? (BLIT_Fullscreen | BLIT_Direct3D) : (BLIT_HardwarePaint | BLIT_Direct3D), NewX, NewY, NewColorBytes))
		return 0;

	if (enteringFullscreen)
	{
		HDC screenDC = GetDC(0);
		int screenWidth = GetDeviceCaps(screenDC, HORZRES);
		int screenHeight = GetDeviceCaps(screenDC, VERTRES);
		ReleaseDC(0, screenDC);

		SetWindowLong(window, GWL_STYLE, WS_OVERLAPPED | WS_VISIBLE);
		SetWindowLong(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
		SetWindowPos(window, HWND_TOP, 0, 0, screenWidth, screenHeight, SWP_FRAMECHANGED | SWP_NOSENDCHANGING | SWP_NOZORDER);

		FullscreenState.Enabled = true;
	}

	// Restyling a window can leave it behind whatever was in front of it, with
	// the engine still believing it has focus and swallowing the input that
	// would bring it back. Ask for the foreground explicitly on either
	// transition rather than relying on the restyle to carry it.
	SetForegroundWindow(window);
	SetFocus(window);

	ReclipCursorToWindow(window);

	SaveConfig();
	Flush(1);
	return 1;

	unguard;
}

void UPathTracerRenderDevice::CreateSwapChainResources()
{
	// Sized to the viewport rather than the window: the result is blitted, so
	// the two need not agree and the trace should cost what the game asked for.
	int width = Max((int)Viewport->SizeX, 1);
	int height = Max((int)Viewport->SizeY, 1);

	if (AccumImage && width == TraceWidth && height == TraceHeight)
		return;

	vkDeviceWaitIdle(Device->device);

	TileFramebuffer.reset();
	AccumView.reset();
	AccumImage.reset();
	HistoryView.reset();
	HistoryImage.reset();
	OutputView.reset();
	OutputImage.reset();

	AccumImage = ImageBuilder()
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerAccum")
		.Create(Device.get());
	AccumView = ImageViewBuilder().Image(AccumImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT).DebugName("PathTracerAccumView").Create(Device.get());

	// What each pixel was looking at last frame: the world position it hit and
	// which instance owned it. Compared against this frame to decide whether the
	// pixel's accumulated history still describes the same thing.
	HistoryImage = ImageBuilder()
		.Size(width, height)
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerHistory")
		.Create(Device.get());
	HistoryView = ImageViewBuilder().Image(HistoryImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT).DebugName("PathTracerHistoryView").Create(Device.get());

	OutputImage = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		.DebugName("PathTracerOutput")
		.Create(Device.get());
	OutputView = ImageViewBuilder().Image(OutputImage.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerOutputView").Create(Device.get());

	// The tile pass draws into the traced image, so its framebuffer follows the
	// image rather than the window.
	if (TileRenderPass)
	{
		TileFramebuffer = FramebufferBuilder()
			.RenderPass(TileRenderPass.get())
			.Size(width, height)
			.AddAttachment(OutputView.get())
			.DebugName("PathTracerTileFramebuffer")
			.Create(Device.get());
	}

	TraceWidth = width;
	TraceHeight = height;
	AccumulatedFrames = 0;
	DescriptorsDirty = true;

	// Both images start undefined and the trace shader writes them as GENERAL.
	ExecuteImmediate([this](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(AccumImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(HistoryImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	debugf(TEXT("PathTracer buffers: %dx%d"), width, height);
}

void UPathTracerRenderDevice::ReleaseSwapChainResources()
{
}

// Bind whatever textures the scene has registered since the last frame.
//
// Written incrementally: the registry only ever grows within a level, and
// rewriting a thousand descriptors every frame to add one is wasteful.
void UPathTracerRenderDevice::UpdateSceneTextures()
{
	guard(UPathTracerRenderDevice::UpdateSceneTextures);

	if (!CanSampleTextures || !DescriptorSet || !Textures || !SceneSampler)
		return;

	CachedTexture* white = Textures->White();
	if (!white || !white->View)
		return;

	// Every slot starts valid. A descriptor that is never read still has to be
	// something, and filling the array once is cheaper than tracking which
	// slots the shader might reach.
	if (!SceneTexturesInitialised)
	{
		SceneTexturesInitialised = true;
		WriteDescriptors writes;
		for (int i = 0; i < LevelScene::MaxTextures; i++)
			writes.AddCombinedImageSampler(DescriptorSet, 6, i, white->View.get(), SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		writes.Execute(Device.get());
	}

	// A new level empties the registry, so what is already bound can outnumber
	// what the scene now wants. Start again rather than leave stale slots.
	if (Scene.Textures.size() < BoundSceneTextures)
		BoundSceneTextures = 0;

	const size_t wanted = std::min<size_t>(Scene.Textures.size(), LevelScene::MaxTextures);
	if (wanted <= BoundSceneTextures)
		return;

	WriteDescriptors writes;
	for (size_t i = BoundSceneTextures; i < wanted; i++)
	{
		UTexture* texture = Scene.Textures[i];
		CachedTexture* cached = Textures->GetForScene(texture, Scene.TextureMasked[i]);
		VulkanImageView* view = (cached && cached->View) ? cached->View.get() : white->View.get();

		// Named, so a texture that comes out wrong on screen can be identified
		// rather than guessed at.
		if (!cached && TextureFailuresLogged < 24)
		{
			TextureFailuresLogged++;
			debugf(TEXT("PathTracer texture %d '%s' (%s) %dx%d could not be uploaded"),
				(int)i, texture->GetName(),
				texture->GetClass() ? texture->GetClass()->GetName() : TEXT("?"),
				(int)texture->USize, (int)texture->VSize);
		}
		writes.AddCombinedImageSampler(DescriptorSet, 6, (int)i, view, SceneSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	writes.Execute(Device.get());
	BoundSceneTextures = wanted;

	unguard;
}

void UPathTracerRenderDevice::UpdateDescriptors()
{
	if (!DescriptorsDirty || !Accel || !Accel->IsReady() || !AccumView || !Accel->GetInstanceDataBuffer())
		return;

	WriteDescriptors()
		.AddAccelerationStructure(DescriptorSet, 0, Accel->GetTopLevel())
		.AddStorageImage(DescriptorSet, 1, AccumView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(DescriptorSet, 2, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(DescriptorSet, 7, HistoryView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddBuffer(DescriptorSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetAttributeBuffer())
		.AddBuffer(DescriptorSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightBuffer())
		.AddBuffer(DescriptorSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetInstanceDataBuffer())
		.Execute(Device.get());

	DescriptorsDirty = false;
}

void UPathTracerRenderDevice::ExecuteImmediate(const std::function<void(VulkanCommandBuffer*)>& fn)
{
	auto commands = CommandPool->createBuffer();
	commands->begin();
	fn(commands.get());
	commands->end();

	auto fence = FenceBuilder().DebugName("PathTracerImmediate").Create(Device.get());
	QueueSubmit()
		.AddCommandBuffer(commands.get())
		.Execute(Device.get(), Device->GraphicsQueue, fence.get());

	VkFence handle = fence->fence;
	vkWaitForFences(Device->device, 1, &handle, VK_TRUE, std::numeric_limits<uint64_t>::max());
}

void UPathTracerRenderDevice::EnsureSceneBuilt(ULevel* level)
{
	if (!level || !level->Model)
		return;

	// Rebuild when the level changes. Comparing the node count as well catches
	// a level object being reused for a different map, which the engine does.
	// Rebuild from scratch only when the level changes. Comparing the node count
	// as well catches a level object being reused for a different map.
	if (Scene.SourceLevel != level || Scene.SourceNodeCount != level->Model->Nodes.Num())
	{
		debugf(TEXT("PathTracer: building the scene"));

		Accel->Reset();
		if (!Scene.BuildStatic(level))
		{
			debugf(TEXT("PathTracer: nothing to build from"));
			return;
		}

		debugf(TEXT("PathTracer: %d static triangles, %d lights, %d mirrored surfaces"),
			(int)(Scene.Geometries[0].Positions.size() / 3), (int)Scene.Lights.size(),
			Scene.MirroredCount());
	}

	// Every frame: where the movers and the mesh actors are now. New shapes get
	// a bottom level structure the first time they are seen.
	const size_t geometriesBefore = Scene.Geometries.size();
	Scene.LightScale = Max(LightScale, 1) / 100.0f;
	const double collectStart = NowMs();
	Scene.CollectDynamic(level);
	const double syncStart = NowMs();
	Accel->SyncGeometry(Scene);
	Timings.Collect += syncStart - collectStart;
	Timings.Sync += NowMs() - syncStart;

	// Only when something new appeared, so this says what is being traced
	// without filling the log every frame.
	if (Scene.Geometries.size() != geometriesBefore)
	{
		debugf(TEXT("PathTracer: %d shapes, %d instances this frame"),
			(int)Scene.Geometries.size(), (int)Scene.Instances.size());
	}

	if (Accel->AttributesChanged())
	{
		Accel->ClearAttributesChanged();
		DescriptorsDirty = true;
	}
}

void UPathTracerRenderDevice::SetSceneNode(FSceneNode* Frame)
{
	guardSlow(UPathTracerRenderDevice::SetSceneNode);

	// The first scene node of a frame is the player's view. Later ones are
	// mirrors, skyboxes and the weapon, which this device does not yet treat
	// separately - taking the first keeps the camera stable.
	if (HaveCamera)
		return;

	// Whose view this is, so the actor collection can skip the player's own body
	// and honour the owner-only visibility flags.
	Scene.ViewActor = Frame->Viewport ? Frame->Viewport->Actor : nullptr;

	// The view's basis, kept for placing the first person weapon.
	Scene.ViewOrigin = Frame->Coords.Origin;
	Scene.ViewRight = Frame->Coords.XAxis;
	Scene.ViewDown = Frame->Coords.YAxis;
	Scene.ViewForward = Frame->Coords.ZAxis;

	EnsureSceneBuilt(Frame->Level);

	// The engine projects a point as X * Proj.Z / Z, so Proj.Z carries the field
	// of view this node is actually rendered with - including the cinematic
	// cameras, which do not use the player's FovAngle.
	float halfWidth;
	if (Frame->Proj.Z > 0.0f)
		halfWidth = Frame->FX / (2.0f * Frame->Proj.Z);
	else
		halfWidth = 1.0f;

	const float aspect = Frame->FY / Frame->FX;

	// Coords, not Uncoords. FCoords transforms by dotting against its axes, so
	// Coords.XAxis/YAxis/ZAxis are the world space directions of the view's own
	// axes - exactly the basis needed to turn a pixel into a world ray. Uncoords
	// is the inverse, which for a rotation is the transpose, so using it applies
	// the rotation backwards: the picture comes out inverted and mouse yaw
	// arrives as roll.
	//
	// The engine's view axes are X right, Y down and Z forward (GMath.ViewCoords
	// maps view Y to minus world Z). The shader's uv.y also runs downward, so
	// the down-pointing Y axis is used as it stands.
	const FCoords& c = Frame->Coords;

	PushConstants.CameraOrigin = vec4(c.Origin.X, c.Origin.Y, c.Origin.Z, 0.0f);
	PushConstants.CameraRight = vec4(c.XAxis.X, c.XAxis.Y, c.XAxis.Z, 0.0f) * halfWidth;
	PushConstants.CameraUp = vec4(c.YAxis.X, c.YAxis.Y, c.YAxis.Z, 0.0f) * (halfWidth * aspect);
	PushConstants.CameraForward = vec4(c.ZAxis.X, c.ZAxis.Y, c.ZAxis.Z, 0.0f);

	HaveCamera = true;

	unguardSlow;
}

void UPathTracerRenderDevice::Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize)
{
	guard(UPathTracerRenderDevice::Lock);

	if (HitSize)
		*HitSize = 0;

	try
	{
		CreateSwapChainResources();
		HaveCamera = false;
		TileVertices.clear();
		TileBatches.clear();
	}
	catch (const std::exception& e)
	{
		debugf(TEXT("PathTracer could not begin a frame: %s"), appFromAnsi(e.what()));
	}

	unguard;
}

void UPathTracerRenderDevice::Unlock(UBOOL Blit)
{
	guard(UPathTracerRenderDevice::Unlock);

	// Deliberately not conditional on HaveCamera. A frame that draws no world -
	// a menu, or a conversation - never calls SetSceneNode, and returning here
	// left an acquired swap chain image unpresented, which the compositor shows
	// as black. Whether the world can be traced and whether the frame must be
	// presented are different questions.
	if (!Blit || !Accel || !AccumImage)
		return;

	const double frameStart = NowMs();

	try
	{
		// Accumulate only while the view is still. Any movement and the samples
		// behind it describe a different picture, so start again.
		const bool cameraMoved =
			memcmp(&PushConstants.CameraOrigin, &LastCamera.CameraOrigin, sizeof(vec4) * 4) != 0;
		// A door swinging past is as much a change as the camera turning, and
		// the instance count is a cheap proxy for the scene having moved. It
		// misses an actor that moves while the count holds, which is why the
		// accumulation is capped rather than trusted indefinitely.
		const bool sceneChanged = Scene.Instances.size() != LastInstanceCount;
		LastInstanceCount = Scene.Instances.size();
		if (cameraMoved || sceneChanged)
			AccumulatedFrames = 0;
		LastCamera = PushConstants;

		PushConstants.Counts[0] = FrameIndex++;
		PushConstants.Counts[1] = (uint32_t)Accel->LightCount();
		PushConstants.Counts[2] = (uint32_t)Max(Bounces, 1);
		PushConstants.Counts[3] = AccumulatedFrames;
		UpdateSceneTextures();
		PushConstants.TextureCount = (uint32_t)BoundSceneTextures;
		PushConstants.MaxSamples = (uint32_t)Max(MaxAccumulatedFrames, 1);
		PushConstants.Time = (Viewport && Viewport->Actor && Viewport->Actor->Level)
			? (float)fmod((double)Viewport->Actor->Level->TimeSeconds, 1000.0) : 0.0f;
		PushConstants.SkyOrigin = vec4(Scene.SkyOrigin.X, Scene.SkyOrigin.Y, Scene.SkyOrigin.Z, Scene.HasSky ? 1.0f : 0.0f);
		PushConstants.Params = vec4(
			0.2f + Exposure * (2.0f / 255.0f),
			SkyIntensity * (2.0f / 255.0f),
			0.5f,     // ray epsilon, in world units: these levels are big
			(float)DebugMode);

		int windowWidth = 0, windowHeight = 0;
		RECT box = {};
		GetClientRect((HWND)Viewport->GetWindow(), &box);
		windowWidth = box.right;
		windowHeight = box.bottom;
		if (windowWidth <= 0 || windowHeight <= 0)
			return;

		if (SwapChain->Lost() || SwapChain->Width() != windowWidth || SwapChain->Height() != windowHeight || UsingVsync != UseVSync)
		{
			UsingVsync = UseVSync;
			SwapChain->Create(windowWidth, windowHeight, UseVSync ? 2 : 3, UseVSync, false, false);
		}

		int imageIndex = SwapChain->AcquireImage(ImageAvailableSemaphore.get());
		if (imageIndex == -1)
			return;

		auto commands = CommandPool->createBuffer();
		commands->begin();

		// Textures that generate themselves get a chance to advance before the
		// trace reads them, recorded into this frame's command buffer rather
		// than submitted one at a time.
		const double refreshStart = NowMs();
		if (Textures && Viewport && Viewport->Actor && Viewport->Actor->Level)
			Textures->RefreshRealtime(Viewport->Actor->Level->TimeSeconds, commands.get(), RealtimeStaging, Scene.FixedFrames);
		Timings.Refresh += NowMs() - refreshStart;

		// The top level structure is rebuilt every frame, because the movers and
		// the actors have all moved since the last one.
		bool traceThisFrame = HaveCamera;
		if (traceThisFrame)
		{
			Accel->HideStatic = (DebugMode == 1);
			const double topStart = NowMs();
			Accel->BuildTopLevel(Scene, commands.get());
			Timings.TopLevel += NowMs() - topStart;
			traceThisFrame = Accel->IsReady();
		}

		// One line per frame for the first few seconds: the black frames are
		// intermittent, so the pattern is the evidence. Guessing at this from a
		// screenshot has been wrong twice.
		// Without a trace the output image keeps the last traced world, which is
		// what the engine expects behind a menu or a conversation.
		if (traceThisFrame)
		{
			UpdateDescriptors();

			commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, TracePipeline.get());
			commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, DescriptorSet);
			commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &PushConstants);
			commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);
		}

		// HUD, menus and console on top of the traced world.
		RenderTiles(commands.get());

		// The trace writes the output image; the blit reads it.
		// Waits on the tile pass as well as the trace: the HUD is drawn as a
		// colour attachment write, which the compute stage alone does not cover.
		PipelineBarrier()
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		// Letterbox: keep the traced image's aspect inside the window rather
		// than stretching it, the same as the other devices here.
		float scale = std::min(windowWidth / (float)TraceWidth, windowHeight / (float)TraceHeight);
		int dstWidth = (int)std::round(TraceWidth * scale);
		int dstHeight = (int)std::round(TraceHeight * scale);
		int dstX = (windowWidth - dstWidth) / 2;
		int dstY = (windowHeight - dstHeight) / 2;

		VkImageBlit blit = {};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1] = { TraceWidth, TraceHeight, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[0] = { dstX, dstY, 0 };
		blit.dstOffsets[1] = { dstX + dstWidth, dstY + dstHeight, 1 };

		commands->blitImage(
			OutputImage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			SwapChain->GetImage(imageIndex)->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_LINEAR);

		PipelineBarrier()
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0)
			.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		commands->end();

		QueueSubmit()
			.AddCommandBuffer(commands.get())
			.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ImageAvailableSemaphore.get())
			.AddSignal(RenderFinishedSemaphore.get())
			.Execute(Device.get(), Device->GraphicsQueue, RenderFinishedFence.get());

		SwapChain->QueuePresent(imageIndex, RenderFinishedSemaphore.get());

		VkFence handle = RenderFinishedFence->fence;
		const double waitStart = NowMs();
		vkWaitForFences(Device->device, 1, &handle, VK_TRUE, std::numeric_limits<uint64_t>::max());
		Timings.Wait += NowMs() - waitStart;
		vkResetFences(Device->device, 1, &handle);

		// The scene gathering happens earlier, in SetSceneNode, so it is
		// added to the frame's total rather than measured inside it.
		Timings.Total += NowMs() - frameStart;
		if (++Timings.Frames >= 300)
		{
			if (LogTimings && Timings.Logged < 100 && HaveCamera)
			{
				Timings.Logged++;
				const double n = Timings.Frames;
				debugf(TEXT("PathTracer ms/frame: collect %.2f sync %.2f refresh %.2f toplevel %.2f gpu-wait %.2f unlock %.2f | %d instances, %d textures, %d realtime staged, %d poses rebuilt"),
					Timings.Collect / n, Timings.Sync / n, Timings.Refresh / n, Timings.TopLevel / n,
					Timings.Wait / n, Timings.Total / n,
					(int)Scene.Instances.size(), (int)Scene.Textures.size(), (int)RealtimeStaging.size(), Scene.MeshBuilds);
			}
			const int logged = Timings.Logged;
			Timings = FrameTimings();
			Timings.Logged = logged;
		}

		// The submission is done with them now.
		RealtimeStaging.clear();

		if (AccumulatedFrames < (uint32_t)Max(MaxAccumulatedFrames, 1))
			AccumulatedFrames++;
	}
	catch (const std::exception& e)
	{
		debugf(TEXT("PathTracer frame failed: %s"), appFromAnsi(e.what()));
	}

	unguard;
}

void UPathTracerRenderDevice::Flush(UBOOL AllowPrecache)
{
	guard(UPathTracerRenderDevice::Flush);
	AccumulatedFrames = 0;
	TileVertices.clear();
	TileBatches.clear();
	if (Textures)
	{
		if (Device) vkDeviceWaitIdle(Device->device);
		Textures->Clear();

		// Clearing the cache destroys the image views that the trace's texture
		// array still points at, so those descriptors have to be rewritten
		// before anything samples them again. Going fullscreen calls Flush, and
		// the next dispatch read freed images.
		BoundSceneTextures = 0;
		SceneTexturesInitialised = false;
	}
	unguard;
}

UBOOL UPathTracerRenderDevice::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
{
	guard(UPathTracerRenderDevice::Exec);

	if (ParseCommand(&Cmd, TEXT("GetRes")))
	{
		// The same list the other devices here offer, including the letterboxed
		// modes: the engine's field of view is horizontal, so a wide screen
		// crops rather than widens.
		HDC screenDC = GetDC(0);
		int screenWidth = GetDeviceCaps(screenDC, HORZRES);
		int screenHeight = GetDeviceCaps(screenDC, VERTRES);
		ReleaseDC(0, screenDC);

		FString Str;
		Str += FString::Printf(TEXT("%ix%i "), screenWidth, screenHeight);
		Str += FString::Printf(TEXT("%ix%i "), (screenHeight * 4 + 2) / 3, screenHeight);
		Str += FString::Printf(TEXT("%ix%i "), (screenHeight * 16 + 8) / 9, screenHeight);
		Str += TEXT("1920x1080 1600x900 1280x720 1024x768 800x600 640x480");
		Ar.Log(*Str);
		return 1;
	}

	return URenderDevice::Exec(Cmd, Ar);

	unguard;
}

void UPathTracerRenderDevice::Exit()
{
	guard(UPathTracerRenderDevice::Exit);

	if (Device) vkDeviceWaitIdle(Device->device);

	Accel.reset();
	Scene.Clear();

	OutputView.reset();
	OutputImage.reset();
	AccumView.reset();
	AccumImage.reset();
	HistoryView.reset();
	HistoryImage.reset();

	TileFramebuffer.reset();
	TileVertexBuffer.reset();
	for (auto& p : TilePipelines) p.reset();
	TileFragmentShader.reset();
	TileVertexShader.reset();
	TileRenderPass.reset();
	TilePipelineLayout.reset();
	Textures.reset();
	TileDescriptorPool.reset();
	TileSetLayout.reset();
	TileSampler.reset();
	// Released here with everything else. Left to the member destructor it
	// outlived the device and destroyed itself against a dead handle, which
	// crashed on exit and brought up the safe mode prompt on the next launch.
	SceneSampler.reset();
	BoundSceneTextures = 0;
	SceneTexturesInitialised = false;

	TracePipeline.reset();
	TraceShader.reset();
	PipelineLayout.reset();
	DescriptorSetOwner.reset();
	DescriptorSet = nullptr;
	DescriptorPool.reset();
	DescriptorLayout.reset();

	RenderFinishedSemaphore.reset();
	ImageAvailableSemaphore.reset();
	RenderFinishedFence.reset();
	DrawCommands.reset();
	CommandPool.reset();
	SwapChain.reset();

	Device.reset();
	Surface.reset();
	Instance.reset();

	unguard;
}

// --- Everything the engine pushes that this device does not use --------------

void UPathTracerRenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet) {}
void UPathTracerRenderDevice::DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span) {}
// The engine's 2D drawing: HUD, menus, console, subtitles, the mouse cursor.
//
// Collected here rather than drawn, because the traced image does not exist yet
// when these arrive - the whole frame is traced in Unlock. Batches are merged
// while the texture and blend mode hold, which for a menu is most of it.
void UPathTracerRenderDevice::DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags)
{
	guardSlow(UPathTracerRenderDevice::DrawTile);

	if (!Textures || TraceWidth <= 0 || TraceHeight <= 0)
		return;

	// A texture can carry PF_Masked itself rather than the caller passing it.
	// Modulated art is excluded: its transparency is carried by the grey level
	// rather than by a palette hole, and punching alpha into it would leave
	// gaps where the texture happens to use index zero as a real colour.
	const DWORD flags = PolyFlags | (Info.Texture ? Info.Texture->PolyFlags : 0);
	const bool masked = (flags & PF_Masked) != 0 && (flags & PF_Modulated) == 0;
	CachedTexture* texture = Textures->Get(Info, masked);
	if (!texture)
		return;

	int blendMode = 0;
	if (flags & PF_Translucent)
		blendMode = 1;
	else if (flags & PF_Modulated)
		blendMode = 2;

	// What each piece of 2D art actually asks for. The crosshair of a scope
	// arrives as a black square, which means it is not asking for the additive
	// blend its artwork assumes.
	// Only once a level is up: the menu has enough art to use the whole budget
	// before anything in the game is drawn, which it has done twice now.
	// The engine gives tile positions in viewport pixels including the frame's
	// own offset, and texture coordinates in texels.
	const float x0 = (X + Frame->XB);
	const float y0 = (Y + Frame->YB);
	const float x1 = x0 + XL;
	const float y1 = y0 + YL;

	const float sx = 2.0f / (float)TraceWidth;
	const float sy = 2.0f / (float)TraceHeight;

	const float uScale = Info.USize > 0 ? 1.0f / (Info.UScale * Info.USize) : 0.0f;
	const float vScale = Info.VSize > 0 ? 1.0f / (Info.VScale * Info.VSize) : 0.0f;

	const float u0 = U * uScale;
	const float v0 = V * vScale;
	const float u1 = (U + UL) * uScale;
	const float v1 = (V + VL) * vScale;

	vec4 colour = vec4(Color.X, Color.Y, Color.Z, 1.0f);
	if (flags & PF_Modulated)
		colour = vec4(1.0f, 1.0f, 1.0f, 1.0f);

	TileVertex corners[4];
	corners[0] = { vec2(x0 * sx - 1.0f, y0 * sy - 1.0f), vec2(u0, v0), colour };
	corners[1] = { vec2(x1 * sx - 1.0f, y0 * sy - 1.0f), vec2(u1, v0), colour };
	corners[2] = { vec2(x1 * sx - 1.0f, y1 * sy - 1.0f), vec2(u1, v1), colour };
	corners[3] = { vec2(x0 * sx - 1.0f, y1 * sy - 1.0f), vec2(u0, v1), colour };

	if (TileBatches.empty() || TileBatches.back().Texture != texture || TileBatches.back().BlendMode != blendMode)
	{
		TileBatch batch;
		batch.Texture = texture;
		batch.BlendMode = blendMode;
		batch.FirstVertex = (int)TileVertices.size();
		batch.VertexCount = 0;
		TileBatches.push_back(batch);
	}

	const int order[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++)
		TileVertices.push_back(corners[order[i]]);
	TileBatches.back().VertexCount += 6;

	unguardSlow;
}
void UPathTracerRenderDevice::Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2) {}
void UPathTracerRenderDevice::Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z) {}
void UPathTracerRenderDevice::ClearZ(FSceneNode* Frame) {}
void UPathTracerRenderDevice::PushHit(const BYTE* Data, INT Count) {}
void UPathTracerRenderDevice::PopHit(INT Count, UBOOL bForce) {}
void UPathTracerRenderDevice::GetStats(TCHAR* Result) { Result[0] = 0; }
void UPathTracerRenderDevice::ReadPixels(FColor* Pixels) {}
