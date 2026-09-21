#include "Precomp.h"
#include "UPathTracerRenderDevice.h"
#include "Shaders.h"
#include <stdexcept>

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
	UseVSync = 1;

	new(GetClass(), TEXT("Bounces"), RF_Public) UIntProperty(CPP_PROPERTY(Bounces), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("Exposure"), RF_Public) UByteProperty(CPP_PROPERTY(Exposure), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("SkyIntensity"), RF_Public) UByteProperty(CPP_PROPERTY(SkyIntensity), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("MaxAccumulatedFrames"), RF_Public) UIntProperty(CPP_PROPERTY(MaxAccumulatedFrames), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("VkDeviceIndex"), RF_Public) UIntProperty(CPP_PROPERTY(VkDeviceIndex), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("VkDebug"), RF_Public) UBoolProperty(CPP_PROPERTY(VkDebug), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("UseVSync"), RF_Public) UBoolProperty(CPP_PROPERTY(UseVSync), TEXT("Display"), CPF_Config);

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
		deviceBuilder.SelectDevice(VkDeviceIndex);
		Device = deviceBuilder.Create(Instance);

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

		CreateTracePipeline();
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
		.DebugName("PathTracerSetLayout")
		.Create(Device.get());

	DescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2)
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

UBOOL UPathTracerRenderDevice::SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	guard(UPathTracerRenderDevice::SetRes);

	if (!Viewport->ResizeViewport(Fullscreen ? (BLIT_Fullscreen | BLIT_Direct3D) : (BLIT_HardwarePaint | BLIT_Direct3D), NewX, NewY, NewColorBytes))
		return 0;

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

	AccumView.reset();
	AccumImage.reset();
	OutputView.reset();
	OutputImage.reset();

	AccumImage = ImageBuilder()
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerAccum")
		.Create(Device.get());
	AccumView = ImageViewBuilder().Image(AccumImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT).DebugName("PathTracerAccumView").Create(Device.get());

	OutputImage = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		.DebugName("PathTracerOutput")
		.Create(Device.get());
	OutputView = ImageViewBuilder().Image(OutputImage.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerOutputView").Create(Device.get());

	TraceWidth = width;
	TraceHeight = height;
	AccumulatedFrames = 0;
	DescriptorsDirty = true;

	// Both images start undefined and the trace shader writes them as GENERAL.
	ExecuteImmediate([this](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(AccumImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	});

	debugf(TEXT("PathTracer buffers: %dx%d"), width, height);
}

void UPathTracerRenderDevice::ReleaseSwapChainResources()
{
}

void UPathTracerRenderDevice::UpdateDescriptors()
{
	if (!DescriptorsDirty || !Accel || !Accel->IsBuilt() || !AccumView)
		return;

	WriteDescriptors()
		.AddAccelerationStructure(DescriptorSet, 0, Accel->GetTopLevel())
		.AddStorageImage(DescriptorSet, 1, AccumView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(DescriptorSet, 2, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddBuffer(DescriptorSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetAttributeBuffer())
		.AddBuffer(DescriptorSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Accel->GetLightBuffer())
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
	if (Scene.SourceLevel == level && Scene.SourceNodeCount == level->Model->Nodes.Num() && Accel->IsBuilt())
		return;

	debugf(TEXT("PathTracer: building the scene"));

	if (!Scene.Build(level))
	{
		debugf(TEXT("PathTracer: nothing to build from"));
		return;
	}

	Accel->Build(Scene);
	DescriptorsDirty = true;
	AccumulatedFrames = 0;

	debugf(TEXT("PathTracer: %d triangles, %d lights"), Scene.TriangleCount(), (int)Scene.Lights.size());
}

void UPathTracerRenderDevice::SetSceneNode(FSceneNode* Frame)
{
	guardSlow(UPathTracerRenderDevice::SetSceneNode);

	// The first scene node of a frame is the player's view. Later ones are
	// mirrors, skyboxes and the weapon, which this device does not yet treat
	// separately - taking the first keeps the camera stable.
	if (HaveCamera)
		return;

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

	if (!Blit || !HaveCamera || !Accel || !Accel->IsBuilt() || !AccumImage)
		return;

	try
	{
		UpdateDescriptors();

		// Accumulate only while the view is still. Any movement and the samples
		// behind it describe a different picture, so start again.
		const bool cameraMoved =
			memcmp(&PushConstants.CameraOrigin, &LastCamera.CameraOrigin, sizeof(vec4) * 4) != 0;
		if (cameraMoved)
			AccumulatedFrames = 0;
		LastCamera = PushConstants;

		PushConstants.Counts[0] = FrameIndex++;
		PushConstants.Counts[1] = (uint32_t)Accel->LightCount();
		PushConstants.Counts[2] = (uint32_t)Max(Bounces, 1);
		PushConstants.Counts[3] = AccumulatedFrames;
		PushConstants.Params = vec4(
			0.2f + Exposure * (2.0f / 255.0f),
			SkyIntensity * (2.0f / 255.0f),
			0.5f,     // ray epsilon, in world units: these levels are big
			0.0f);

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

		commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, TracePipeline.get());
		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout.get(), 0, DescriptorSet);
		commands->pushConstants(PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePushConstants), &PushConstants);
		commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);

		// The trace writes the output image; the blit reads it.
		PipelineBarrier()
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

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
		vkWaitForFences(Device->device, 1, &handle, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(Device->device, 1, &handle);

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
void UPathTracerRenderDevice::DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags) {}
void UPathTracerRenderDevice::Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2) {}
void UPathTracerRenderDevice::Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z) {}
void UPathTracerRenderDevice::ClearZ(FSceneNode* Frame) {}
void UPathTracerRenderDevice::PushHit(const BYTE* Data, INT Count) {}
void UPathTracerRenderDevice::PopHit(INT Count, UBOOL bForce) {}
void UPathTracerRenderDevice::GetStats(TCHAR* Result) { Result[0] = 0; }
void UPathTracerRenderDevice::ReadPixels(FColor* Pixels) {}
