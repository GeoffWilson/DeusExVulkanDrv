#include "Precomp.h"
#include "UPathTracerRenderDevice.h"
#include "Shaders.h"
#include "Materials.h"
#include "TraceProtocol.h"
#include <stdexcept>
#include <chrono>
#include <thread>

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

// An ANSI string as the engine's text. Not the SDK's ANSI_TO_TCHAR: it sizes
// its stack buffer in bytes for a string of wide characters, so anything longer
// than a few words wrote past the end of it - which is what a timing line in
// the log did, taking the game down with it.
static FString Widen(const char* text)
{
	FString result;
	if (text)
	{
		TCHAR ch[2] = { 0, 0 };
		for (; *text; text++)
		{
			ch[0] = (TCHAR)(unsigned char)*text;
			result += ch;
		}
	}
	return result;
}

// Window and swap chain events, and any crash, to PathTracerEvents.log beside
// the game's log. The game's own log is written in blocks, and the block that
// would say what happened is the one lost when the process dies: this file is
// flushed line by line. Started afresh each time the device starts.
static void PathTracerEvent(const char* format, ...)
{
	FILE* f = fopen("PathTracerEvents.log", "a");
	if (!f)
		return;
	SYSTEMTIME now = {};
	GetLocalTime(&now);
	fprintf(f, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
	va_list args;
	va_start(args, format);
	vfprintf(f, format, args);
	va_end(args);
	fprintf(f, "\n");
	fclose(f);
}

// Where an address is: module and offset, which a disassembly or the build's
// map can turn into a function.
static void PathTracerDescribeAddress(const void* address, char* out, size_t size)
{
	HMODULE module = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module))
		GetModuleFileNameA(module, name, MAX_PATH);
	const char* base = strrchr(name, '\\');
	snprintf(out, size, "%s+0x%lx", base ? base + 1 : name, (unsigned long)((uintptr_t)address - (uintptr_t)module));
}

// The call stack from here, one module and offset per frame.
static void PathTracerLogStack(const char* label)
{
	void* frames[24] = {};
	const USHORT count = CaptureStackBackTrace(1, 24, frames, nullptr);
	PathTracerEvent("%s, stack:", label);
	for (USHORT i = 0; i < count; i++)
	{
		char where[MAX_PATH + 32];
		PathTracerDescribeAddress(frames[i], where, sizeof(where));
		PathTracerEvent("    %s", where);
	}
}

// Exceptions as they are raised, before anything handles them: faults, and
// C++ exceptions with the type thrown, each with the stack that raised it.
// Most C++ exceptions are the device's own error handling and are caught
// where they are thrown; one that is not ends up in the engine's error
// handler, which closes everything down before the game's log is written.
static LONG CALLBACK PathTracerExceptionLogger(EXCEPTION_POINTERS* info)
{
	static int logged = 0;
	const EXCEPTION_RECORD* record = info->ExceptionRecord;
	const DWORD code = record->ExceptionCode;
	// Thread naming and debug output are raised as exceptions and are noise.
	if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || logged >= 40)
		return EXCEPTION_CONTINUE_SEARCH;
	logged++;

	char where[MAX_PATH + 32];
	PathTracerDescribeAddress(record->ExceptionAddress, where, sizeof(where));

	// An MSVC C++ exception: the thrown type's name is in its throw info, as
	// a decorated name such as .?AVruntime_error@std@@. x86 keeps pointers
	// there directly.
	if (code == 0xE06D7363 && record->NumberParameters >= 3)
	{
		const char* type = "?";
		const DWORD* throwInfo = (const DWORD*)record->ExceptionInformation[2];
		if (throwInfo && throwInfo[3])
		{
			const DWORD* catchables = (const DWORD*)throwInfo[3];
			if (catchables[0] >= 1 && catchables[1])
			{
				const DWORD* catchable = (const DWORD*)catchables[1];
				if (catchable[1])
					type = (const char*)catchable[1] + 8;
			}
		}
		// The object itself, if it is a std::exception: its what().
		const char* what = "";
		if (strstr(type, "exception@std") || strstr(type, "error@std") || strstr(type, "Vulkan"))
		{
			const std::exception* e = (const std::exception*)record->ExceptionInformation[1];
			if (e)
				what = e->what();
		}
		char label[512];
		snprintf(label, sizeof(label), "C++ exception %s \"%s\"", type, what);
		PathTracerLogStack(label);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	char label[512];
	snprintf(label, sizeof(label), "exception %08lx at %s%s", (unsigned long)code, where,
		code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
			? (record->ExceptionInformation[0] ? " (writing)" : " (reading)") : "");
	PathTracerLogStack(label);
	return EXCEPTION_CONTINUE_SEARCH;
}

// The engine's window procedure, and the messages worth knowing about on the
// way to it: activation, minimising and restoring, resizing, and the
// system commands behind them.
//
// Also where alt-tab out of fullscreen is kept from undoing fullscreen. On
// losing focus the engine drops back to a window and minimises it; on being
// restored it destroys this device and makes a new one in fullscreen. Under
// wine the restyle that follows takes the focus away again a moment later,
// and the engine, seeing that as another alt-tab, drops out and minimises
// once more - so the game could never be brought back. While the window is
// fullscreen the engine is not told it has lost focus at all: the window
// stays as it is, underneath whatever the player switched to, and coming
// back is only raising it. Windows' own handling of those messages still
// happens, and the mouse is released here as the engine would have released
// it.
static WNDPROC PathTracerEngineWndProc = nullptr;
static UPathTracerRenderDevice* PathTracerWindowDevice = nullptr;
static LRESULT CALLBACK PathTracerWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	const bool fullscreen = PathTracerWindowDevice && PathTracerWindowDevice->IsFullscreenWindow();
	const bool losingFocus =
		(message == WM_ACTIVATEAPP && !wParam) ||
		(message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) ||
		message == WM_KILLFOCUS;
	if (fullscreen && losingFocus)
	{
		PathTracerEvent("kept fullscreen through message %04x", message);
		ClipCursor(nullptr);
		ReleaseCapture();
		return DefWindowProc(window, message, wParam, lParam);
	}

	switch (message)
	{
	case WM_ACTIVATEAPP: PathTracerEvent("WM_ACTIVATEAPP %s", wParam ? "active" : "inactive"); break;
	case WM_ACTIVATE: PathTracerEvent("WM_ACTIVATE %d%s", (int)LOWORD(wParam), HIWORD(wParam) ? " minimised" : ""); break;
	case WM_SIZE: PathTracerEvent("WM_SIZE type %d %dx%d", (int)wParam, (int)LOWORD(lParam), (int)HIWORD(lParam)); break;
	case WM_SYSCOMMAND: PathTracerEvent("WM_SYSCOMMAND %04x", (unsigned)(wParam & 0xfff0)); break;
	case WM_SETFOCUS: PathTracerEvent("WM_SETFOCUS"); break;
	case WM_KILLFOCUS: PathTracerEvent("WM_KILLFOCUS"); break;
	}
	return CallWindowProc(PathTracerEngineWndProc, window, message, wParam, lParam);
}

static void* PathTracerExceptionHandle = nullptr;

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
	UseDenoiser = 1;
	FPSLimit = 120;
	GlossBounces = 1;
	UseMaterials = 1;

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
	new(GetClass(), TEXT("Denoise"), RF_Public) UBoolProperty(CPP_PROPERTY(UseDenoiser), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("Materials"), RF_Public) UBoolProperty(CPP_PROPERTY(UseMaterials), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("GlossBounces"), RF_Public) UIntProperty(CPP_PROPERTY(GlossBounces), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("FPSLimit"), RF_Public) UIntProperty(CPP_PROPERTY(FPSLimit), TEXT("Display"), CPF_Config);

	unguard;
}

UBOOL UPathTracerRenderDevice::Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	guard(UPathTracerRenderDevice::Init);

	Viewport = InViewport;
	DenoiseEnabled = UseDenoiser != 0;
	MaterialsEnabled = UseMaterials != 0;

	// Started afresh once per run: the engine can make a new device mid
	// session, and what led up to that is the part worth keeping.
	static bool eventsStarted = false;
	if (!eventsStarted)
		remove("PathTracerEvents.log");
	eventsStarted = true;
	PathTracerEvent("Init %dx%d %s", (int)NewX, (int)NewY, Fullscreen ? "fullscreen" : "windowed");
	if (!PathTracerExceptionHandle)
		PathTracerExceptionHandle = AddVectoredExceptionHandler(1, PathTracerExceptionLogger);
	if (!PathTracerEngineWndProc)
	{
		HWND window = (HWND)InViewport->GetWindow();
		PathTracerEngineWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)PathTracerWndProc);
		SubclassedWindow = window;
	}
	PathTracerWindowDevice = this;

	try
	{
		Instance = VulkanInstanceBuilder()
			.RequireSurfaceExtensions()
			.DebugLayer(VkDebug)
			.Create();

		Surface = VulkanSurfaceBuilder()
			.Win32Window((HWND)Viewport->GetWindow())
			.Create(Instance);

		// This device only presents: the tracing is the helper's. What it does
		// need is to take the helper's frame over on the GPU, which is two
		// extensions every driver tested offers a 32-bit client.
		auto deviceBuilder = VulkanDeviceBuilder();
		deviceBuilder.Surface(Surface);
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
		deviceBuilder.SelectDevice(VkDeviceIndex);
		Device = deviceBuilder.Create(Instance);

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

		Textures.reset(new TextureCache(this));
		CreateTilePipeline();

		if (!StartTracer())
		{
			Exit();
			return 0;
		}

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

// Everything about how an actor is drawn, for when one draws wrongly: its
// flags, glow and skins, then each of its mesh's materials with the texture,
// and what is in it. PT WEAPON and PT LOOK.
static void DescribeActor(AActor* actor, UMesh* mesh)
{
	auto nameOf = [](UObject* o) { return o ? o->GetName() : TEXT("none"); };
	auto classOf = [](UObject* o) { return o ? o->GetClass()->GetName() : TEXT("-"); };
	debugf(TEXT("PT: %s (%s) mesh %s drawtype %d style %d unlit %d specialLit %d glow %.2f ambientglow %d fatness %d envmap %d light %d/%d brightness %d"),
		actor->GetName(), classOf(actor), nameOf(mesh), (int)actor->DrawType, (int)actor->Style, (int)actor->bUnlit, (int)actor->bSpecialLit,
		(float)actor->ScaleGlow, (int)actor->AmbientGlow, (int)actor->Fatness, (int)actor->bMeshEnviroMap,
		(int)actor->LightType, (int)actor->LightEffect, (int)actor->LightBrightness);
	debugf(TEXT("  skin %s texture %s zone %s ambient %d/%d/%d"), nameOf(actor->Skin), nameOf(actor->Texture),
		nameOf(actor->Region.Zone), actor->Region.Zone ? (int)actor->Region.Zone->AmbientBrightness : -1,
		actor->Region.Zone ? (int)actor->Region.Zone->AmbientHue : -1, actor->Region.Zone ? (int)actor->Region.Zone->AmbientSaturation : -1);
	for (int i = 0; i < 8; i++)
		debugf(TEXT("  multiskin %d: %s (%s)  getskin %s  mesh texture %s"), i, nameOf(actor->MultiSkins[i]), classOf(actor->MultiSkins[i]),
			nameOf(actor->GetSkin(i)), (mesh && i < mesh->Textures.Num()) ? nameOf(mesh->Textures(i)) : TEXT("-"));

	auto describe = [&](UTexture* t, const TCHAR* label)
	{
		if (!t)
			return;
		int nonZero = 0, total = 0;
		if (t->Mips.Num() > 0)
		{
			FMipmap& mip = t->Mips(0);
			total = mip.DataArray.Num();
			for (int b = 0; b < total; b++)
				nonZero += mip.DataArray(b) != 0;
		}
		debugf(TEXT("    %s %s: %dx%d format %d mips %d realtime %d parametric %d palette %d colours, mip 0 %d of %d bytes non-zero"),
			label, t->GetName(), (int)t->USize, (int)t->VSize, (int)t->Format, t->Mips.Num(),
			(int)t->bRealtime, (int)t->bParametric, t->Palette ? t->Palette->Colors.Num() : -1, nonZero, total);
	};

	ULodMesh* lod = Cast<ULodMesh>(mesh);
	if (!lod)
		return;
	std::vector<int> faces(lod->Materials.Num(), 0);
	for (INT f = 0; f < lod->Faces.Num(); f++)
		if (lod->Faces(f).MaterialIndex < faces.size())
			faces[lod->Faces(f).MaterialIndex]++;
	for (INT m = 0; m < lod->Materials.Num(); m++)
	{
		const FMeshMaterial& material = lod->Materials(m);
		UTexture* texture = (material.TextureIndex < mesh->Textures.Num()) ? mesh->Textures(material.TextureIndex) : nullptr;
		debugf(TEXT("  material %d: flags %08x texture index %d (%s, %s, texture flags %08x) %d faces"),
			m, (DWORD)material.PolyFlags, (int)material.TextureIndex, nameOf(texture), classOf(texture),
			texture ? (DWORD)texture->PolyFlags : 0u, faces[m]);
		describe(texture, TEXT("texture"));
		if (!texture)
			continue;
		{
			const TCHAR* kind = nullptr;
			const vec4 m = Materials::For(texture, actor, &kind);
			debugf(TEXT("    material %s: roughness %.2f metalness %.2f reflectance %.2f (group %s)"),
				kind, m.x, m.y, m.z, texture->GetOuter() ? texture->GetOuter()->GetName() : TEXT("none"));
		}
		for (TFieldIterator<UObjectProperty> it(texture->GetClass()); it; ++it)
			if (!appStricmp(it->GetName(), TEXT("SourceTexture")))
				describe(*(UTexture**)((BYTE*)texture + it->Offset), TEXT("source"));
	}
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

	// The helper's frame was copied into this image; the tiles are about to
	// read and blend over it. The layout does not change - only the ordering
	// and visibility do.
	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

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
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
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

	// Whether this is the player changing mode, or the engine dropping out of
	// fullscreen because the player has switched to something else. Only the
	// first should end with the game in front.
	const bool wasActive = GetForegroundWindow() == window;
	PathTracerEvent("SetRes %dx%d %s (was %s, %s, %s)", (int)NewX, (int)NewY, Fullscreen ? "fullscreen" : "windowed",
		FullscreenState.Enabled ? "fullscreen" : "windowed", wasActive ? "foreground" : "background",
		IsIconic(window) ? "minimised" : "not minimised");

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
	// transition rather than relying on the restyle to carry it - but only
	// when the game had it. Alt-tabbing out of fullscreen reaches here too,
	// as the engine drops back to a window, and taking the foreground then
	// snatched it back from whatever the player had switched to.
	if (wasActive)
	{
		SetForegroundWindow(window);
		SetFocus(window);
		ReclipCursorToWindow(window);
	}

	SaveConfig();
	Flush(1);
	return 1;

	unguard;
}

void UPathTracerRenderDevice::CreateSwapChainResources()
{
	// Sized to the viewport rather than the window: the result is blitted, so
	// the two need not agree and the trace should cost what the game asked for.
	// The helper follows whatever size the frames are asked for at.
	int width = Max((int)Viewport->SizeX, 1);
	int height = Max((int)Viewport->SizeY, 1);

	if (OutputImage && width == TraceWidth && height == TraceHeight)
		return;
	PathTracerEvent("trace buffers %dx%d -> %dx%d", TraceWidth, TraceHeight, width, height);

	vkDeviceWaitIdle(Device->device);

	TileFramebuffer.reset();
	OutputView.reset();
	OutputImage.reset();

	OutputImage = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
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
	DenoiseRestart = true;

	// Black until the helper's first frame arrives, and in the GENERAL layout
	// the frame keeps it in throughout.
	VulkanImage* image = OutputImage.get();
	ExecuteImmediate([image](VulkanCommandBuffer* cmd)
	{
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue black = {};
		black.float32[3] = 1.0f;
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	});

	debugf(TEXT("PathTracer buffers: %dx%d"), width, height);
}

void UPathTracerRenderDevice::ReleaseSwapChainResources()
{
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

void UPathTracerRenderDevice::WaitForPreviousFrame()
{
	if (!FramePending)
		return;
	FramePending = false;

	if (Device && RenderFinishedFence)
	{
		VkFence handle = RenderFinishedFence->fence;
		const double waitStart = NowMs();
		vkWaitForFences(Device->device, 1, &handle, VK_TRUE, std::numeric_limits<uint64_t>::max());
		Timings.Wait += NowMs() - waitStart;
		vkResetFences(Device->device, 1, &handle);
	}

	// The submission is done with them now.
	PendingCommands.reset();
}

// A timing line to the game's log, and to PathTracerTimings.log beside it. The
// game's log is written in blocks, and a block not yet full when the game
// closes under wine was lost; this one is flushed line by line.
void UPathTracerRenderDevice::WriteTimingLine(const char* line)
{
	debugf(TEXT("%s"), *Widen(line));
	if (FILE* f = fopen("PathTracerTimings.log", "a"))
	{
		fprintf(f, "%s\n", line);
		fclose(f);
	}
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

		// Everything the last frame used is about to be destroyed, and the
		// helper starts again with the new level.
		WaitForPreviousFrame();
		SceneReset = true;
		DenoiseRestart = true;
		if (!Scene.BuildStatic(level))
		{
			debugf(TEXT("PathTracer: nothing to build from"));
			return;
		}

		debugf(TEXT("PathTracer: %d static triangles, %d lights, %d mirrored surfaces"),
			(int)(Scene.Geometries[0].Positions.size() / 3), (int)Scene.Lights.size(),
			Scene.MirroredCount());
	}

	// Every frame: where the movers and the mesh actors are now. New shapes are
	// sent to the helper when the frame is.
	const size_t geometriesBefore = Scene.Geometries.size();
	Scene.LightScale = Max(LightScale, 1) / 100.0f;
	Scene.HighlightSpecialLights = (DisableBits & 16u) != 0;
	// Gathering is CPU only, so it runs while the GPU is still presenting the
	// last frame.
	const double collectStart = NowMs();
	Scene.CollectDynamic(level);
	Timings.Collect += NowMs() - collectStart;

	// Only when something new appeared, so this says what is being traced
	// without filling the log every frame.
	if (Scene.Geometries.size() != geometriesBefore)
	{
		debugf(TEXT("PathTracer: %d shapes, %d instances this frame"),
			(int)Scene.Geometries.size(), (int)Scene.Instances.size());
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

	Camera.Origin = vec4(c.Origin.X, c.Origin.Y, c.Origin.Z, 0.0f);
	Camera.Right = vec4(c.XAxis.X, c.XAxis.Y, c.XAxis.Z, 0.0f) * halfWidth;
	Camera.Up = vec4(c.YAxis.X, c.YAxis.Y, c.YAxis.Z, 0.0f) * (halfWidth * aspect);
	Camera.Forward = vec4(c.ZAxis.X, c.ZAxis.Y, c.ZAxis.Z, 0.0f);

	HaveCamera = true;

	unguardSlow;
}

void UPathTracerRenderDevice::Lock(FPlane InFlashScale, FPlane InFlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize)
{
	guard(UPathTracerRenderDevice::Lock);

	if (HitSize)
		*HitSize = 0;

	try
	{
		CreateSwapChainResources();
		HaveCamera = false;
		FlashScale = InFlashScale;
		FlashFog = InFlashFog;
		TileVertices.clear();
		TileBatches.clear();
	}
	catch (const std::exception& e)
	{
		debugf(TEXT("PathTracer could not begin a frame: %s"), appFromAnsi(e.what()));
	}

	unguard;
}

// Starts the helper that traces, from beside this DLL, on this device's GPU.
bool UPathTracerRenderDevice::StartTracer()
{
	HMODULE module = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&PathTracerEvent, &module);
	char path[MAX_PATH] = {};
	GetModuleFileNameA(module, path, MAX_PATH);
	std::string directory = path;
	directory = directory.substr(0, directory.find_last_of("\\/"));
	const std::string helper = directory + "\\PathTracerHelper.exe";

	Tracer.reset(new TraceClient());
	if (!Tracer->Start(Device.get(), helper, directory, VkDebug != 0))
	{
		const std::string why = Tracer->Error();
		PathTracerEvent("helper did not start: %s", why.c_str());
		debugf(TEXT("PathTracerDrv traces in PathTracerHelper.exe, which did not start: %s"), *Widen(why.c_str()));
		debugf(TEXT("It must be beside PathTracerDrv.dll (%s), and the GPU must offer a 64-bit program ray tracing:"), *Widen(helper.c_str()));
		debugf(TEXT("VK_KHR_ray_query and VK_KHR_acceleration_structure. Its own log, PathTracerHelper.log, is beside it."));
		Tracer.reset();
		return false;
	}

	const TraceProtocol::Header& status = Tracer->Status();
	debugf(TEXT("PathTracer: tracing in PathTracerHelper.exe on %s%s"), *Widen(status.DeviceName),
		status.CanSampleTextures ? TEXT("") : TEXT(", which cannot index textures: surfaces will use one averaged colour each"));
	PathTracerEvent("helper started on %s", status.DeviceName);
	SceneReset = true;
	TracerLost = false;
	return true;
}

// Whatever of the scene the helper does not have yet, and what changes every
// frame: new and rebuilt shapes, new textures with what their surfaces are
// made of, the next frame of any that animate, and every placement and light.
bool UPathTracerRenderDevice::SendScene()
{
	guard(UPathTracerRenderDevice::SendScene);

	if (SceneReset)
	{
		Tracer->ResetScene();
		SentVersions.clear();
		SentTextures.clear();
		SceneReset = false;
	}

	// Shapes are sent once, and those that are rebuilt - an animating
	// character, the decals - again whenever they change.
	for (size_t i = 0; i < Scene.Geometries.size(); i++)
	{
		const SceneGeometry& geometry = Scene.Geometries[i];
		if (i >= SentVersions.size())
			SentVersions.push_back(geometry.Version);
		else if (!geometry.Dynamic || SentVersions[i] == geometry.Version)
			continue;
		SentVersions[i] = geometry.Version;
		Tracer->Geometry((uint32_t)i, geometry);
	}

	const double texturesStart = NowMs();
	for (size_t i = SentTextures.size(); i < Scene.Textures.size(); i++)
	{
		SentTexture sent;
		sent.Source = Scene.Textures[i];
		sent.Masked = Scene.TextureMasked[i];
		int width = 0, height = 0;
		const bool converted = TextureCache::ScenePixels(sent.Source, sent.Masked, Pixels, width, height);
		// Named, so a texture that comes out wrong on screen can be identified
		// rather than guessed at.
		if (!converted && TextureFailuresLogged < 24)
		{
			TextureFailuresLogged++;
			debugf(TEXT("PathTracer texture %d '%s' (%s) %dx%d could not be converted"),
				(int)i, sent.Source->GetName(),
				sent.Source->GetClass() ? sent.Source->GetClass()->GetName() : TEXT("?"),
				(int)sent.Source->USize, (int)sent.Source->VSize);
		}
		sent.Width = converted ? width : 0;
		sent.Height = converted ? height : 0;
		sent.Animated = TextureCache::Animates(sent.Source);
		Tracer->Texture((uint32_t)i, (uint32_t)sent.Width, (uint32_t)sent.Height, converted ? Pixels.data() : nullptr,
			Scene.TextureMaterials[i], sent.Animated);
		SentTextures.push_back(sent);
	}

	// Asked afresh every frame rather than remembered from the first sending,
	// since a script can give a texture an animation chain after it was first
	// seen. One frame of an animation shown on its own - a sprite that plays
	// once chooses it - stays that frame: advancing it would loop it.
	if (Viewport && Viewport->Actor && Viewport->Actor->Level)
	{
		const double time = Viewport->Actor->Level->TimeSeconds;
		for (size_t i = 0; i < SentTextures.size(); i++)
		{
			SentTexture& sent = SentTextures[i];
			if (!sent.Width || Scene.FixedFrames.count(sent.Source) || !TextureCache::Animates(sent.Source))
				continue;
			if (TextureCache::AnimatedPixels(sent.Source, sent.Masked, time, sent.Width, sent.Height, sent.LastFrame, Pixels))
				Tracer->TexturePixels((uint32_t)i, (uint32_t)sent.Width, (uint32_t)sent.Height, Pixels.data());
		}
	}
	Timings.Textures += NowMs() - texturesStart;

	Tracer->Instances(Scene.Instances, Scene.StaticGeometries);
	Tracer->Lights(Scene.Lights, Scene.FogLights);
	return Tracer->Alive();

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
	WaitForPreviousFrame();

	if (!Blit || !OutputImage)
		return;

	const double frameStart = NowMs();

	// Set once the helper has traced a frame for this one. From then on this
	// frame owes it: its submission must wait on Ready and signal Released,
	// or the helper's next frame waits for ever.
	bool owed = false;

	try
	{
		// Accumulate only while the view is still. Any movement and the samples
		// behind it describe a different picture, so start again.
		auto sameXyz = [](const vec4& a, const vec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
		const bool cameraMoved =
			!sameXyz(Camera.Origin, LastCamera.Origin) ||
			!sameXyz(Camera.Right, LastCamera.Right) ||
			!sameXyz(Camera.Up, LastCamera.Up) ||
			!sameXyz(Camera.Forward, LastCamera.Forward);
		// A door swinging past is as much a change as the camera turning, and
		// the instance count is a cheap proxy for the scene having moved. It
		// misses an actor that moves while the count holds, which is why the
		// accumulation is capped rather than trusted indefinitely.
		const bool sceneChanged = Scene.Instances.size() != LastInstanceCount;
		LastInstanceCount = Scene.Instances.size();
		if (cameraMoved || sceneChanged)
			AccumulatedFrames = 0;
		// Where the camera was, for the motion vectors: last frame's, or this
		// one's on the first frame there is.
		const bool haveLastCamera = LastCamera.Forward.x != 0.0f || LastCamera.Forward.y != 0.0f || LastCamera.Forward.z != 0.0f;
		const TraceCamera previousCamera = haveLastCamera ? LastCamera : Camera;
		LastCamera = Camera;

		int windowWidth = 0, windowHeight = 0;
		RECT box = {};
		GetClientRect((HWND)Viewport->GetWindow(), &box);
		windowWidth = box.right;
		windowHeight = box.bottom;
		if (windowWidth <= 0 || windowHeight <= 0)
			return;

		if (SwapChain->Lost() || SwapChain->Width() != windowWidth || SwapChain->Height() != windowHeight || UsingVsync != UseVSync)
		{
			PathTracerEvent("swap chain %dx%d -> %dx%d%s", SwapChain->Width(), SwapChain->Height(), windowWidth, windowHeight,
				SwapChain->Lost() ? " (lost)" : "");
			UsingVsync = UseVSync;
			SwapChain->Create(windowWidth, windowHeight, UseVSync ? 2 : 3, UseVSync, false, false);
		}

		int imageIndex = SwapChain->AcquireImage(ImageAvailableSemaphore.get());
		if (imageIndex == -1)
		{
			PathTracerEvent("no swap chain image%s", SwapChain->Lost() ? " (lost)" : "");
			return;
		}

		// The frame is traced only once it is certain to be presented, since a
		// traced frame has to be taken. Without one the output image keeps the
		// last traced world, which is what the engine expects behind a menu or
		// a conversation.
		if (HaveCamera && Tracer && Tracer->Alive() && !Scene.IsEmpty())
		{
			const double sendStart = NowMs();
			if (SendScene())
			{
				TraceProtocol::TraceCommand frame = {};
				frame.Width = (uint32_t)TraceWidth;
				frame.Height = (uint32_t)TraceHeight;
				frame.Frame = FrameIndex++;
				frame.AccumulatedFrames = AccumulatedFrames;
				frame.MaxSamples = (uint32_t)Max(MaxAccumulatedFrames, 1);
				frame.Bounces = (uint32_t)Clamp(Bounces, 1, 255);
				frame.GlossBounces = (uint32_t)Clamp(GlossBounces, 0, 255);
				frame.DisableBits = DisableBits;
				frame.ViewMode = (uint32_t)ViewMode;
				frame.DebugMode = (uint32_t)DebugMode;
				frame.Denoise = DenoiseEnabled ? 1 : 0;
				frame.Materials = MaterialsEnabled ? 1 : 0;
				frame.RestartDenoiser = DenoiseRestart ? 1 : 0;
				frame.Timing = LogTimings ? 1 : 0;
				frame.Time = (Viewport && Viewport->Actor && Viewport->Actor->Level)
					? (float)fmod((double)Viewport->Actor->Level->TimeSeconds, 1000.0) : 0.0f;
				frame.Exposure = 0.2f + Exposure * (2.0f / 255.0f);
				frame.SkyIntensity = SkyIntensity * (2.0f / 255.0f);
				// The screen flash, as the other devices blend it - the picture
				// times min(2 * scale, 1), plus the flash colour - carried in
				// the camera vectors' spare w. Neutral is a scale of one half
				// and no colour.
				frame.Camera[0] = Camera.Origin;
				frame.Camera[1] = Camera.Right;
				frame.Camera[2] = Camera.Up;
				frame.Camera[3] = Camera.Forward;
				frame.Camera[0].w = Min(FlashScale.X * 2.0f, 1.0f);
				frame.Camera[1].w = FlashFog.X;
				frame.Camera[2].w = FlashFog.Y;
				frame.Camera[3].w = FlashFog.Z;
				frame.PreviousCamera[0] = previousCamera.Origin;
				frame.PreviousCamera[1] = previousCamera.Right;
				frame.PreviousCamera[2] = previousCamera.Up;
				frame.PreviousCamera[3] = previousCamera.Forward;
				frame.SkyOrigin = vec4(Scene.SkyOrigin.X, Scene.SkyOrigin.Y, Scene.SkyOrigin.Z, Scene.HasSky ? 1.0f : 0.0f);
				owed = Tracer->Trace(frame);
				if (owed)
				{
					DenoiseRestart = false;
					const TraceProtocol::Header& status = Tracer->Status();
					if (status.GpuTimed)
					{
						Timings.GpuBuild += status.GpuBuildMs;
						Timings.GpuTrace += status.GpuTraceMs;
						Timings.GpuDenoise += status.GpuDenoiseMs;
						Timings.GpuComposite += status.GpuCompositeMs;
						Timings.GpuFrames++;
					}
				}
			}
			Timings.Send += NowMs() - sendStart;

			if (!Tracer->Alive() && !TracerLost)
			{
				TracerLost = true;
				PathTracerEvent("helper lost: %s", Tracer->Error().c_str());
				debugf(TEXT("PathTracer: the helper has stopped (%s); PathTracerHelper.log says more. The world will not be traced again this session."),
					*Widen(Tracer->Error().c_str()));
			}
		}

		auto commands = CommandPool->createBuffer();
		commands->begin();

		// The helper's frame, taken over from its queue as a transfer from
		// VK_QUEUE_FAMILY_EXTERNAL, copied into the output image and handed
		// back the same way.
		if (owed)
		{
			const uint32_t family = (uint32_t)Device->GraphicsFamily;
			VkImageMemoryBarrier barriers[2] = {};
			for (auto& b : barriers)
			{
				b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			}
			barriers[0].image = Tracer->Output();
			barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			barriers[0].dstQueueFamilyIndex = family;
			barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[1].image = OutputImage->image;
			barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

			VkImageCopy copy = {};
			copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.dstSubresource = copy.srcSubresource;
			copy.extent = { (uint32_t)Min(TraceWidth, (int)Tracer->OutputWidth()), (uint32_t)Min(TraceHeight, (int)Tracer->OutputHeight()), 1 };
			vkCmdCopyImage(commands->buffer, Tracer->Output(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, OutputImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

			barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[0].srcQueueFamilyIndex = family;
			barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[0].dstAccessMask = 0;
			barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 2, barriers);
		}

		// HUD, menus and console on top of the traced world.
		RenderTiles(commands.get());

		// The copy and the tiles write the output image; the blit reads it.
		PipelineBarrier()
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

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
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0)
			.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		commands->end();

		// Waits for the swap chain image before the tiles, and for the helper's
		// frame before the copy; signals the present, and the helper's go-ahead.
		VkSemaphore waits[2] = { ImageAvailableSemaphore->semaphore, owed ? Tracer->Ready() : VK_NULL_HANDLE };
		VkPipelineStageFlags waitStages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
		VkSemaphore signals[2] = { RenderFinishedSemaphore->semaphore, owed ? Tracer->Released() : VK_NULL_HANDLE };
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount = owed ? 2 : 1;
		submit.pWaitSemaphores = waits;
		submit.pWaitDstStageMask = waitStages;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands->buffer;
		submit.signalSemaphoreCount = owed ? 2 : 1;
		submit.pSignalSemaphores = signals;
		if (vkQueueSubmit(Device->GraphicsQueue, 1, &submit, RenderFinishedFence->fence) != VK_SUCCESS)
			throw std::runtime_error("vkQueueSubmit failed");
		owed = false;

		// Not waited for here. The command buffer has to outlive the
		// submission, so it is kept until the wait - and kept before
		// presenting, not after. A present that failed threw past this, which
		// freed the command buffer the GPU was still running and left the
		// fence signalled with nothing to wait on it, so the next frame
		// submitted against a fence already set.
		PendingCommands = std::move(commands);
		FramePending = true;

		SwapChain->QueuePresent(imageIndex, RenderFinishedSemaphore.get());

		// Paced after the present rather than before the next frame's work,
		// so the game's own tick is what waits.
		const double limitStart = NowMs();
		LimitFrameRate();
		Timings.Limit += NowMs() - limitStart;

		// The scene gathering happens earlier, in SetSceneNode, so it is
		// added to the frame's total rather than measured inside it.
		Timings.Total += NowMs() - frameStart;
		if (++Timings.Frames >= 300)
		{
			if (LogTimings && Timings.Logged < 100 && HaveCamera)
			{
				Timings.Logged++;
				const double n = Timings.Frames;
				char line[512];
				snprintf(line, sizeof(line), "PathTracer ms/frame: collect %.2f send %.2f (textures %.2f) gpu-wait %.2f unlock %.2f limiter %.2f | %d instances, %d textures, %d poses rebuilt",
					Timings.Collect / n, Timings.Send / n, Timings.Textures / n,
					Timings.Wait / n, (Timings.Total - Timings.Limit) / n, Timings.Limit / n,
					(int)Scene.Instances.size(), (int)Scene.Textures.size(), Scene.MeshBuilds);
				WriteTimingLine(line);
				if (Timings.GpuFrames > 0 && Tracer)
				{
					const double g = Timings.GpuFrames;
					snprintf(line, sizeof(line), "PathTracer GPU ms/frame (helper): build %.2f trace %.2f denoise %.2f composite %.2f | total %.2f at %dx%d, bounces %d, glossy bounces %d, materials %s, denoiser %s",
						Timings.GpuBuild / g, Timings.GpuTrace / g, Timings.GpuDenoise / g, Timings.GpuComposite / g,
						(Timings.GpuBuild + Timings.GpuTrace + Timings.GpuDenoise + Timings.GpuComposite) / g,
						TraceWidth, TraceHeight, (int)Bounces, (int)GlossBounces,
						MaterialsEnabled ? "on" : "off", Tracer->Status().DenoiserActive ? "on" : "off");
					WriteTimingLine(line);
				}
			}
			const int logged = Timings.Logged;
			Timings = FrameTimings();
			Timings.Logged = logged;
		}

		if (AccumulatedFrames < (uint32_t)Max(MaxAccumulatedFrames, 1))
			AccumulatedFrames++;
	}
	catch (const std::exception& e)
	{
		PathTracerEvent("frame failed: %s", e.what());
		debugf(TEXT("PathTracer frame failed: %s"), *Widen(e.what()));
		// A frame the helper traced is still taken, with nothing done to it:
		// its Ready waited on and its Released signalled, so the next one can
		// be traced at all.
		if (owed && Tracer)
		{
			VkSemaphore ready = Tracer->Ready(), released = Tracer->Released();
			VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &ready;
			submit.pWaitDstStageMask = &stage;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &released;
			vkQueueSubmit(Device->GraphicsQueue, 1, &submit, VK_NULL_HANDLE);
			vkQueueWaitIdle(Device->GraphicsQueue);
		}
	}

	unguard;
}

// The same pacing as VulkanDrv's FPSLimit, less its wait for the frame to
// reach the screen: this device keeps the next frame's scene gathering running
// while the GPU traces the last, and waiting for the present would serialise
// the two again. What the game needs is its tick held down, which the deadline
// alone does.
void UPathTracerRenderDevice::LimitFrameRate()
{
	// Fullscreen and behind whatever the player alt-tabbed to, the game still
	// runs, as it would have minimised; there is no call to trace it at full
	// rate while nobody can see it.
	const bool background = FullscreenState.Enabled && Viewport && GetForegroundWindow() != (HWND)Viewport->GetWindow();
	const int limit = background ? (FPSLimit > 0 ? Min(FPSLimit, 20) : 20) : FPSLimit;
	if (limit <= 0)
	{
		NextFrameTime = {};
		return;
	}

	using namespace std::chrono;

	auto interval = duration_cast<steady_clock::duration>(duration<double>(1.0 / (double)limit));
	auto now = steady_clock::now();

	// Pace off when the last frame was due rather than when it finished, so a
	// frame that runs long is not paid for twice: the next one is due an
	// interval after the last was, which may already have passed. More than a
	// frame behind, the schedule starts again from now, with nothing to wait.
	//
	// Adding the interval after restarting, as VulkanDrv's limiter once did,
	// makes a game that cannot reach the limit wait a whole interval every
	// frame on top of its own time: a 13 ms frame under a 120 limit became
	// 21 ms.
	if (NextFrameTime == steady_clock::time_point())
		NextFrameTime = now;
	else
		NextFrameTime += interval;
	if (now > NextFrameTime + interval)
		NextFrameTime = now;

	// Sleeping is only accurate to a millisecond or so, so hand the last of the
	// wait to a spin.
	while (true)
	{
		auto remaining = NextFrameTime - steady_clock::now();
		if (remaining <= steady_clock::duration::zero())
			break;
		if (remaining > milliseconds(2))
			std::this_thread::sleep_for(remaining - milliseconds(1));
		else
			std::this_thread::yield();
	}
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
		// Only the 2D's textures: the scene's are the helper's, and a flush
		// does not change the level.
		Textures->Clear();
	}
	unguard;
}

UBOOL UPathTracerRenderDevice::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
{
	guard(UPathTracerRenderDevice::Exec);

	// Diagnostic switches for finding what a frame's time goes on, flipped
	// live from the console while watching the frame rate. Each turns one
	// part of the trace off; whichever gives the time back is the cost.
	if (ParseCommand(&Cmd, TEXT("PT")))
	{
		// The nearest lights that are not plain steady ones, relative to where
		// the player is looking, so they can be walked to without knowing
		// which way the level's axes run.
		// What the held weapon is drawn from: its mesh, style and skins, and
		// each material's flags and texture, for when one draws wrongly.
		if (ParseCommand(&Cmd, TEXT("WEAPON")))
		{
			APlayerPawn* player = Viewport ? Viewport->Actor : nullptr;
			AInventory* item = player ? player->Weapon : nullptr;
			if (item)
				DescribeActor(item, item->PlayerViewMesh ? item->PlayerViewMesh : item->Mesh);
			Ar.Logf(TEXT("PT: weapon details written to the log"));
			return 1;
		}

		// The same for whatever is under the crosshair.
		if (ParseCommand(&Cmd, TEXT("LOOK")))
		{
			APlayerPawn* player = Viewport ? Viewport->Actor : nullptr;
			if (!player || !player->XLevel)
				return 1;
			const FVector start = player->Location + FVector(0.0f, 0.0f, player->EyeHeight);
			const FVector end = start + player->ViewRotation.Vector() * 8000.0f;
			FCheckResult hit;
			player->XLevel->SingleLineCheck(hit, player, end, start, TRACE_AllColliding);
			if (hit.Actor && hit.Actor != player->Level)
				DescribeActor(hit.Actor, hit.Actor->Mesh);
			// The level's own surface: its texture and what it counts as being
			// made of, which is the name to use for an override in the ini.
			UModel* model = player->XLevel->Model;
			if (hit.Actor == player->Level && model && hit.Item >= 0 && hit.Item < model->Nodes.Num())
			{
				const FBspNode& node = model->Nodes(hit.Item);
				UTexture* texture = node.iSurf < model->Surfs.Num() ? model->Surfs(node.iSurf).Texture : nullptr;
				const TCHAR* kind = nullptr;
				const vec4 m = Materials::For(texture, nullptr, &kind);
				Ar.Logf(TEXT("PT: surface %s (group %s): %s, roughness %.2f metalness %.2f reflectance %.2f"),
					texture ? texture->GetName() : TEXT("none"),
					(texture && texture->GetOuter()) ? texture->GetOuter()->GetName() : TEXT("none"),
					kind, m.x, m.y, m.z);
			}
			Ar.Logf(TEXT("PT: %s written to the log"), (hit.Actor && hit.Actor != player->Level) ? hit.Actor->GetName() : TEXT("nothing but the level"));
			return 1;
		}

		if (ParseCommand(&Cmd, TEXT("LIGHTS")))
		{
			APlayerPawn* player = Viewport ? Viewport->Actor : nullptr;
			ULevel* level = player ? player->XLevel : nullptr;
			if (!level)
				return 1;
			static const TCHAR* types[] = { TEXT("none"), TEXT("steady"), TEXT("pulse"), TEXT("blink"), TEXT("flicker"),
				TEXT("strobe"), TEXT("backdrop"), TEXT("subtlepulse"), TEXT("paletteonce"), TEXT("paletteloop") };
			const FCoords view = GMath.UnitCoords / player->ViewRotation;
			std::vector<std::pair<float, AActor*>> found;
			for (INT i = 0; i < level->Actors.Num(); i++)
			{
				AActor* a = level->Actors(i);
				if (!a || a->LightType == LT_None || a->LightBrightness == 0)
					continue;
				if (a->LightType == LT_Steady && a->LightEffect == LE_None)
					continue;
				found.push_back({ (a->Location - player->Location).Size(), a });
			}
			std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			Ar.Logf(TEXT("PT: %d special lights, nearest:"), (int)found.size());
			for (size_t n = 0; n < found.size() && n < 8; n++)
			{
				AActor* a = found[n].second;
				const FVector d = a->Location - player->Location;
				const float ahead = d | view.XAxis, right = d | view.YAxis;
				static const TCHAR* effects[] = { TEXT(""), TEXT(" torchwaver"), TEXT(" firewaver"), TEXT(" wateryshimmer"),
					TEXT(" searchlight"), TEXT(" slowwave"), TEXT(" fastwave"), TEXT(" cloudcast"), TEXT(" staticspot"),
					TEXT(" shock"), TEXT(" disco"), TEXT(" warp"), TEXT(" spot"), TEXT(" nonincidence"), TEXT(" shell"),
					TEXT(" omnibumpmap"), TEXT(" interference"), TEXT(" cylinder"), TEXT(" rotor"), TEXT(" unused") };
				Ar.Logf(TEXT("  %s %s%s cone %d: %.0f %s, %.0f %s, %.0f %s"),
					a->GetName(), a->LightType < 10 ? types[a->LightType] : TEXT("?"),
					a->LightEffect < 20 ? effects[a->LightEffect] : TEXT(" ?"),
					(int)a->LightCone,
					std::fabs(ahead), ahead >= 0 ? TEXT("ahead") : TEXT("behind"),
					std::fabs(right), right >= 0 ? TEXT("right") : TEXT("left"),
					std::fabs(d.Z), d.Z >= 0 ? TEXT("up") : TEXT("down"));
			}
			return 1;
		}

		struct Switch { const TCHAR* Name; uint32_t Bit; };
		static const Switch switches[] = {
			{ TEXT("NOLIGHTS"), 1u }, { TEXT("NOSHADOWS"), 2u }, { TEXT("NOSKY"), 4u }, { TEXT("OPAQUE"), 8u }, { TEXT("HIGHLIGHT"), 16u }, { TEXT("NOFOG"), 32u }, { TEXT("GUIDES"), 64u },
		};
		bool handled = false;
		for (const Switch& s : switches)
		{
			if (ParseCommand(&Cmd, s.Name))
			{
				DisableBits ^= s.Bit;
				handled = true;
			}
		}
		if (ParseCommand(&Cmd, TEXT("BOUNCES")))
		{
			Bounces = Max(appAtoi(Cmd), 1);
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("NOMATERIALS")))
		{
			// The denoiser follows at the next frame, once nothing is
			// using the one it replaces.
			MaterialsEnabled = !MaterialsEnabled;
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("GLOSSBOUNCES")))
		{
			GlossBounces = Max(appAtoi(Cmd), 0);
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("RESET")))
		{
			DisableBits = 0;
			ViewMode = 0;
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("DENOISE")))
		{
			DenoiseEnabled = !DenoiseEnabled;
			DenoiseRestart = true;
			Ar.Logf(TEXT("PT: denoiser %s (%s)"), DenoiseEnabled ? TEXT("on") : TEXT("off"),
				(Tracer && Tracer->Alive()) ? *Widen(Tracer->Status().DenoiserStatus) : TEXT("no helper"));
			handled = true;
		}
		// One of the denoiser's inputs in place of the picture. Numbered as
		// the trace shader numbers them.
		if (ParseCommand(&Cmd, TEXT("VIEW")))
		{
			static const struct { const TCHAR* Name; int Mode; } views[] = {
				{ TEXT("NORMALS"), 3 }, { TEXT("DEPTH"), 4 }, { TEXT("MOTION"), 5 }, { TEXT("DIFFUSE"), 6 },
				{ TEXT("SPECULAR"), 7 }, { TEXT("EMISSION"), 8 }, { TEXT("ALBEDO"), 9 }, { TEXT("HITDIST"), 10 }, { TEXT("HISTORY"), 11 },
				{ TEXT("MATERIAL"), 12 },
			};
			ViewMode = 0;
			for (const auto& v : views)
				if (ParseCommand(&Cmd, v.Name))
					ViewMode = v.Mode;
			Ar.Logf(TEXT("PT: view %s  (PT VIEW NORMALS | DEPTH | MOTION | DIFFUSE | SPECULAR | EMISSION | ALBEDO | HITDIST | HISTORY | MATERIAL, or PT VIEW for the picture)"),
				ViewMode ? TEXT("set") : TEXT("off"));
			handled = true;
		}
		AccumulatedFrames = 0;
		Ar.Logf(TEXT("PT: lights %s, shadows %s, sky %s, per-triangle checks %s, materials %s, bounces %d, glossy bounces %d%s"),
			(DisableBits & 1u) ? TEXT("OFF") : TEXT("on"), (DisableBits & 2u) ? TEXT("OFF") : TEXT("on"),
			(DisableBits & 4u) ? TEXT("OFF") : TEXT("on"), (DisableBits & 8u) ? TEXT("OFF") : TEXT("on"),
			MaterialsEnabled ? TEXT("on") : TEXT("OFF"),
			(int)Bounces, (int)GlossBounces, handled ? TEXT("") : TEXT("  (PT LIGHTS | WEAPON | LOOK | HIGHLIGHT | NOLIGHTS | NOSHADOWS | NOSKY | NOFOG | NOMATERIALS | OPAQUE | DENOISE | VIEW name | GUIDES | BOUNCES n | GLOSSBOUNCES n | RESET)"));
		return 1;
	}

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

	PathTracerLogStack("Exit");
	if (PathTracerEngineWndProc && SubclassedWindow && IsWindow(SubclassedWindow))
		SetWindowLongPtr(SubclassedWindow, GWLP_WNDPROC, (LONG_PTR)PathTracerEngineWndProc);
	PathTracerEngineWndProc = nullptr;
	SubclassedWindow = nullptr;
	if (PathTracerWindowDevice == this)
		PathTracerWindowDevice = nullptr;
	// The exception logger stays: the engine destroys and recreates this
	// device mid session - restoring the window from alt-tab does - and a
	// crash on the way down is exactly what it is there to catch.

	if (Device) vkDeviceWaitIdle(Device->device);
	FramePending = false;
	PendingCommands.reset();

	// Tells the helper to go, and frees what was imported from it, while the
	// device it was imported into is still here.
	Tracer.reset();
	Scene.Clear();
	SceneReset = true;
	SentVersions.clear();
	SentTextures.clear();
	NextFrameTime = {};

	OutputView.reset();
	OutputImage.reset();

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
	// Released here with everything else. Left to the member destructors
	// they outlive the device and destroy themselves against a dead handle,
	// which crashed on exit - and when the engine replaces the device on
	// restoring from alt-tab.
	TileSampler.reset();

	RenderFinishedSemaphore.reset();
	ImageAvailableSemaphore.reset();
	RenderFinishedFence.reset();
	CommandPool.reset();
	SwapChain.reset();

	Device.reset();
	Surface.reset();
	Instance.reset();
	PathTracerEvent("Exit complete");

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
