#include "Precomp.h"
#include "UPathTracerRenderDevice.h"
#include "Shaders.h"
#include "Materials.h"
#include "TraceProtocol.h"
#ifdef PATHTRACER_LOCAL
#include "RayReconstruction.h"
#include <cstdarg>
#endif
#include <stdexcept>
#include <chrono>
#include <thread>
#include <unordered_set>

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

#ifdef PATHTRACER_LOCAL
// The tracer's log. In the helper it is a file of its own; traced in this
// process, it is the engine's. printf formatted, with the engine's %S - a
// narrow string inside a wide format - read as %s.
void HelperLog(const char* format, ...)
{
	std::string fixed = format;
	for (size_t i = 0; (i = fixed.find("%S", i)) != std::string::npos; i += 2)
		fixed[i + 1] = 's';
	char line[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), fixed.c_str(), args);
	va_end(args);
	debugf(TEXT("PathTracer: %s"), appFromAnsi(line));
}
#endif

UPathTracerRenderDevice::UPathTracerRenderDevice()
{
}

// An ANSI string as the engine's text. Not the SDK's ANSI_TO_TCHAR: it sizes
// its stack buffer in bytes for a string of wide characters, so anything longer
// than a few words wrote past the end of it - which is what a timing line in
// the log did, taking the game down with it.
// The other way, for the timings log: anything past Latin-1 becomes '?'.
static std::string Narrow(const TCHAR* text)
{
	std::string result;
	for (; text && *text; text++)
		result += (*text < 256) ? (char)*text : '?';
	return result;
}

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

// The game's window and every other one the process has on screen, at one
// step of a mode change. A compositor that refuses a window change ends the
// process without an exception, so the last of these is all that is left to
// say which step it was and what else was on screen.
static BOOL CALLBACK PathTracerDescribeOtherWindow(HWND window, LPARAM game)
{
	DWORD process = 0;
	GetWindowThreadProcessId(window, &process);
	if (process != GetCurrentProcessId() || window == (HWND)game || !IsWindowVisible(window))
		return TRUE;
	char name[64] = "";
	GetClassNameA(window, name, sizeof(name));
	RECT box = {};
	GetWindowRect(window, &box);
	PathTracerEvent("    also %p %s style %08lx ex %08lx owner %p %ld,%ld %ldx%ld", (void*)window, name,
		(unsigned long)GetWindowLong(window, GWL_STYLE), (unsigned long)GetWindowLong(window, GWL_EXSTYLE),
		(void*)GetWindow(window, GW_OWNER), box.left, box.top, box.right - box.left, box.bottom - box.top);
	return TRUE;
}

// The launcher's splash - an ownerless dialog without a caption - is still up
// when the device first sets a mode, and only goes once the engine has
// finished starting. Wine's Wayland driver, as Proton-GE and CachyOS patch it,
// makes a window like that an xdg_popup of whatever lies beneath its corner,
// which is the game's window. Entering a fullscreen mode narrower than the
// monitor then ended the process with the compositor's "destroyed popup not
// top most popup", the splash still being above. Hidden first, it has no
// surface to be in the way; the engine destroys it later as before.
static BOOL CALLBACK PathTracerHideSplash(HWND window, LPARAM game)
{
	DWORD process = 0;
	GetWindowThreadProcessId(window, &process);
	if (process != GetCurrentProcessId() || window == (HWND)game || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
		return TRUE;
	char name[16] = "";
	GetClassNameA(window, name, sizeof(name));
	const LONG style = GetWindowLong(window, GWL_STYLE);
	if (lstrcmpA(name, "#32770") != 0 || !(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION)
		return TRUE;
	PathTracerEvent("hiding the splash %p", (void*)window);
	ShowWindow(window, SW_HIDE);
	return TRUE;
}

static void PathTracerWindowEvent(const char* step, HWND window)
{
	RECT box = {}, client = {};
	GetWindowRect(window, &box);
	GetClientRect(window, &client);
	PathTracerEvent("%s: style %08lx ex %08lx owner %p window %ld,%ld %ldx%ld client %ldx%ld", step,
		(unsigned long)GetWindowLong(window, GWL_STYLE), (unsigned long)GetWindowLong(window, GWL_EXSTYLE),
		(void*)GetWindow(window, GW_OWNER), box.left, box.top, box.right - box.left, box.bottom - box.top,
		client.right, client.bottom);
	EnumWindows(PathTracerDescribeOtherWindow, (LPARAM)window);
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
	// a decorated name such as .?AVruntime_error@std@@, after the type
	// descriptor's two pointers. x86 keeps pointers there directly; x64 keeps
	// offsets from the image base of the module that threw, which is the
	// fourth parameter.
	if (code == 0xE06D7363 && record->NumberParameters >= 3)
	{
		const char* type = "?";
#ifdef _WIN64
		const uintptr_t base = record->NumberParameters >= 4 ? (uintptr_t)record->ExceptionInformation[3] : 0;
#else
		const uintptr_t base = 0;
#endif
		auto at = [base](DWORD offset) { return (const void*)(base + offset); };
		const DWORD* throwInfo = (const DWORD*)record->ExceptionInformation[2];
		if (throwInfo && throwInfo[3] && (base || sizeof(void*) == 4))
		{
			const DWORD* catchables = (const DWORD*)at(throwInfo[3]);
			if (catchables[0] >= 1 && catchables[1])
			{
				const DWORD* catchable = (const DWORD*)at(catchables[1]);
				if (catchable[1])
					type = (const char*)at(catchable[1]) + 2 * sizeof(void*);
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

// PinnedUI as an aspect ratio to lay the UI out in, or 0 for the whole width.
// Anything narrower than square is taken as a mistake rather than a request
// for a portrait UI.
static float UsablePinnedAspect(float aspect)
{
	return (aspect >= 1.0f && aspect <= 8.0f) ? aspect : 0.0f;
}

// "16:9", "4:3", "1.78" or "off", as PT PINNEDUI takes it.
static float ParsePinnedAspect(const TCHAR* text)
{
	while (*text == ' ')
		text++;
	const TCHAR* colon = appStrchr(text, ':');
	if (colon)
	{
		const float height = appAtof(colon + 1);
		return height > 0.0f ? UsablePinnedAspect(appAtof(text) / height) : 0.0f;
	}
	return UsablePinnedAspect(appAtof(text));
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
#if defined(OLDUNREAL469SDK)
	// 469's extensions, which the trace has no use for: the level is read
	// from UModel rather than drawn. Its text comes with PF_Highlighted, which
	// DrawTile blends premultiplied, as 469's own devices do.
	UseLightmapAtlas = 0;
	NeedsMaskedFonts = 0;
	SupportsUpdateTextureRect = 0;
	SupportsStaticBsp = 0;
	SupportsDrawTileList = 0;
#endif

	Bounces = 3;
	Exposure = 90;
	SkyIntensity = 128;
	MaxAccumulatedFrames = 256;
	VkDeviceIndex = 0;
	VkDebug = 0;
	DebugMode = 0;
	LightScale = 100;
	UseVSync = 1;
	LogTimings = 0;
	UseDenoiser = 1;
#if defined(OLDUNREAL469SDK)
	// Deus Ex needs a limit to keep its conversations whole; UT has its own.
	FPSLimit = 0;
#else
	FPSLimit = 120;
#endif
	GlossBounces = 1;
	UseMaterials = 0;
#if defined(OLDUNREAL469SDK)
	// 469 widens the view for a wide screen itself (Hor+); the trace follows
	// the engine's projection, so widening it again here would widen it
	// twice. The HUD is pinned to 4:3 as in Deus Ex.
	UseWidescreenFOV = 0;
	PinnedUI = 4.0f / 3.0f;
#else
	UseWidescreenFOV = 1;
	PinnedUI = 4.0f / 3.0f;
#endif
	LightSize = 4;
	UseDLSS = 1;
	DLSSQuality = 1;
	DetailTextures = 1;
	MaxAnisotropy = 16.0f;
	UseS3TC = 1;
	Lighting = 1;
	NeutralToneMap = 1;
	UseFlashlight = 1;
	FlashlightBrightness = 100;
	FlashlightHaze = 0;
	UseFogShadows = 1;
	GlowLighting = 1000;
	UseColouredGlass = 1;
	Wetness = 0;
	BumpMapping = 0;
	Hdr = 0;
	HdrPeakNits = 1000;
	HdrPaperWhite = 200;

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
	new(GetClass(), TEXT("WidescreenFOV"), RF_Public) UBoolProperty(CPP_PROPERTY(UseWidescreenFOV), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("PinnedUI"), RF_Public) UFloatProperty(CPP_PROPERTY(PinnedUI), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("LightSize"), RF_Public) UIntProperty(CPP_PROPERTY(LightSize), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("MaxAnisotropy"), RF_Public) UFloatProperty(CPP_PROPERTY(MaxAnisotropy), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("UseS3TC"), RF_Public) UBoolProperty(CPP_PROPERTY(UseS3TC), TEXT("Display"), CPF_Config);
	UEnum* lightings = new(GetClass(), TEXT("Lightings"))UEnum(nullptr);
	new(lightings->Names)FName(TEXT("Linear"));
	new(lightings->Names)FName(TEXT("Engine"));
	new(GetClass(), TEXT("Lighting"), RF_Public) UByteProperty(CPP_PROPERTY(Lighting), TEXT("Display"), CPF_Config, lightings);
	new(GetClass(), TEXT("NeutralToneMap"), RF_Public) UBoolProperty(CPP_PROPERTY(NeutralToneMap), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("GlossBounces"), RF_Public) UIntProperty(CPP_PROPERTY(GlossBounces), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("FPSLimit"), RF_Public) UIntProperty(CPP_PROPERTY(FPSLimit), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("DLSS"), RF_Public) UBoolProperty(CPP_PROPERTY(UseDLSS), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("DLSSQuality"), RF_Public) UIntProperty(CPP_PROPERTY(DLSSQuality), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("Flashlight"), RF_Public) UBoolProperty(CPP_PROPERTY(UseFlashlight), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("FlashlightBrightness"), RF_Public) UIntProperty(CPP_PROPERTY(FlashlightBrightness), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("FlashlightHaze"), RF_Public) UIntProperty(CPP_PROPERTY(FlashlightHaze), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("FogShadows"), RF_Public) UBoolProperty(CPP_PROPERTY(UseFogShadows), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("GlowLighting"), RF_Public) UIntProperty(CPP_PROPERTY(GlowLighting), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("ColouredGlass"), RF_Public) UBoolProperty(CPP_PROPERTY(UseColouredGlass), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("Wetness"), RF_Public) UIntProperty(CPP_PROPERTY(Wetness), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("BumpMapping"), RF_Public) UIntProperty(CPP_PROPERTY(BumpMapping), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("HDR"), RF_Public) UBoolProperty(CPP_PROPERTY(Hdr), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("HDRPeakNits"), RF_Public) UIntProperty(CPP_PROPERTY(HdrPeakNits), TEXT("Display"), CPF_Config);
	new(GetClass(), TEXT("HDRPaperWhite"), RF_Public) UIntProperty(CPP_PROPERTY(HdrPaperWhite), TEXT("Display"), CPF_Config);

	unguard;
}

UBOOL UPathTracerRenderDevice::Init(UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	guard(UPathTracerRenderDevice::Init);

	Viewport = InViewport;
	DenoiseEnabled = UseDenoiser != 0;
	DlssEnabled = UseDLSS != 0;
	DlssQualityNow = Clamp(DLSSQuality, 0, 4);
	MaterialsEnabled = UseMaterials != 0;
	WidescreenFovEnabled = UseWidescreenFOV != 0;
	PinnedAspect = UsablePinnedAspect(PinnedUI);
	LightSizeNow = Clamp(LightSize, 0, 255);
	EngineLightingNow = Lighting != 0;
	NeutralToneMapNow = NeutralToneMap != 0;
	DisableBits = (DisableBits & ~ConfiguredMask) | ConfiguredBits();

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
#ifdef PATHTRACER_LOCAL
		// This device traces as well as presents, as the helper's does
		// elsewhere: ray queries, acceleration structures and an indexed
		// texture array, and DLSS Ray Reconstruction's extensions, which have
		// to be asked for now, before there is a device, or not at all.
		std::vector<std::string> ngxInstance, ngxDevice;
		RayReconstruction::RequiredExtensions(ngxInstance, ngxDevice);
		VulkanInstanceBuilder instanceBuilder;
		instanceBuilder.RequireSurfaceExtensions();
		instanceBuilder.DebugLayer(VkDebug);
		for (const std::string& name : ngxInstance)
			instanceBuilder.OptionalExtension(name);
		// Without it no surface can say it takes an HDR colour space.
		instanceBuilder.OptionalExtension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
		Instance = instanceBuilder.Create();
#else
		// Without the colour space extension no surface can say it takes an
		// HDR colour space.
		Instance = VulkanInstanceBuilder()
			.RequireSurfaceExtensions()
			.OptionalExtension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)
			.DebugLayer(VkDebug)
			.Create();
#endif

		Surface = VulkanSurfaceBuilder()
			.Win32Window((HWND)Viewport->GetWindow())
			.Create(Instance);

		auto deviceBuilder = VulkanDeviceBuilder();
		deviceBuilder.Surface(Surface);
#ifdef PATHTRACER_LOCAL
		deviceBuilder.OptionalRayQuery();
		deviceBuilder.OptionalDescriptorIndexing();
		for (const std::string& name : ngxDevice)
			deviceBuilder.OptionalExtension(name);
#else
		// This device only presents: the tracing is the helper's. What it does
		// need is to take the helper's frame over on the GPU, which is two
		// extensions every driver tested offers a 32-bit client.
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		deviceBuilder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#endif
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
		CreateBrightnessPipeline();
		CreateEncodePipeline();

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
			debugf(TEXT("    material %s: roughness %.2f metalness %.2f reflectance %.2f relief %.2f (group %s)"),
				kind, m.x, m.y, Materials::Reflectance(m), Materials::Relief(m), texture->GetOuter() ? texture->GetOuter()->GetName() : TEXT("none"));
		}
		for (TFieldIterator<UObjectProperty> it(texture->GetClass()); it; ++it)
			if (!appStricmp(it->GetName(), TEXT("SourceTexture")))
				describe(*(UTexture**)((BYTE*)texture + it->Offset), TEXT("source"));
	}
}

// What lights an actor, for comparing the trace's lighting of a mesh with
// the engine's. Each light actor in reach with the values its author set,
// then the trace's own record of each light in reach and what the shader
// makes of a surface of the actor facing it square on: the engine's mesh
// response there is 2.5, scaled by 1.4 times the actor's ScaleGlow, summed
// in the colours as displayed with the mesh's ambient, clamped to one and
// then made linear. Measured at the actor's origin, and without shadows.
void UPathTracerRenderDevice::DescribeLightingOf(AActor* target)
{
	ULevel* level = target->XLevel;
	if (!level)
		return;
	debugf(TEXT("PT: lighting of %s at %.0f %.0f %.0f, ScaleGlow %.2f, AmbientGlow %d"), target->GetName(),
		target->Location.X, target->Location.Y, target->Location.Z, (float)target->ScaleGlow, (int)target->AmbientGlow);
	for (INT i = 0; i < level->Actors.Num(); i++)
	{
		AActor* light = level->Actors(i);
		if (!light || light->bDeleteMe || light->LightType == LT_None)
			continue;
		const float radius = light->LightRadius * 25.0f;
		const float distance = (light->Location - target->Location).Size();
		if (distance >= radius)
			continue;
		debugf(TEXT("  light %s (%s): type %d effect %d brightness %d hue %d saturation %d LightRadius %d (%.0f) distance %.0f falloff %.3f special %d"),
			light->GetName(), light->GetClass()->GetName(), (int)light->LightType, (int)light->LightEffect, (int)light->LightBrightness,
			(int)light->LightHue, (int)light->LightSaturation, (int)light->LightRadius, radius, distance, 1.0f - distance / radius,
			(int)light->bSpecialLit);
	}
	const float meshScale = 1.4f * (float)target->ScaleGlow;
	AZoneInfo* zone = target->Region.Zone;
	const vec3 ambient = Scene.MeshAmbient(target, zone);
	debugf(TEXT("  ambient %.3f %.3f %.3f as displayed: zone %s brightness %d hue %d saturation %d, and AmbientGlow"),
		ambient.x, ambient.y, ambient.z, zone ? zone->GetName() : TEXT("none"),
		zone ? (int)zone->AmbientBrightness : 0, zone ? (int)zone->AmbientHue : 0, zone ? (int)zone->AmbientSaturation : 0);
	float total[3] = { ambient.x, ambient.y, ambient.z };
	for (size_t i = 0; i < Scene.Lights.size(); i++)
	{
		const SceneLight& light = Scene.Lights[i];
		const FVector position(light.PositionRadius.x, light.PositionRadius.y, light.PositionRadius.z);
		const float radius = fabs(light.PositionRadius.w);
		const float distance = (position - target->Location).Size();
		if (distance >= radius)
			continue;
		const float falloff = 1.0f - distance / radius;
		const float scale = light.ColorBrightness.w * falloff * 2.5f * meshScale;
		const float facing[3] = { powf(light.ColorBrightness.x, 1.0f / 2.2f) * scale, powf(light.ColorBrightness.y, 1.0f / 2.2f) * scale,
			powf(light.ColorBrightness.z, 1.0f / 2.2f) * scale };
		for (int c = 0; c < 3; c++)
			total[c] += facing[c];
		debugf(TEXT("  traced light %d: colour %.3f %.3f %.3f brightness %.3f radius %.0f distance %.0f falloff %.3f pattern %.0f -> facing %.3f %.3f %.3f"),
			(int)i, light.ColorBrightness.x, light.ColorBrightness.y, light.ColorBrightness.z, light.ColorBrightness.w,
			radius, distance, falloff, light.Flags.w, facing[0], facing[1], facing[2]);
	}
	debugf(TEXT("  facing every light, with the ambient: %.3f %.3f %.3f as displayed, clamped and made linear %.3f %.3f %.3f"), total[0], total[1], total[2],
		powf(Min(total[0], 1.0f), 2.2f), powf(Min(total[1], 1.0f), 2.2f), powf(Min(total[2], 1.0f), 2.2f));
}

// What lights a point on the level, for comparing the trace with the
// engine's lightmaps. For each light in reach: what the engine's lightmap
// gets from it on the scale where 1 is full brightness - its colour at
// FGetHSV's full brightness, times its brightness and the level's, times
// 1 - 3x^2 + 2x^3 of the way x out to its radius, times the cosine at the
// surface, times twice its shadow mask there for a light baked into the
// surface's lightmap, as Render.dll builds a lightmap - whether the level's
// own geometry is in the way, and what the trace takes from it: the
// engine's own figure with its lighting, linear light with the linear.
// Written to the log, strongest first.
void UPathTracerRenderDevice::DescribeLightingAt(ULevel* level, const FVector& point, const FVector& normal, UTexture* texture, bool specialLit, INT iSurf, FOutputDevice& Ar)
{
	struct Entry { AActor* Light; float Engine[3]; float Traced[3]; float Distance, Radius, Cosine, Mask; bool Clear, Baked; };
	std::vector<Entry> entries;
	const FVector from = point + normal * 2.0f;
	const float levelBrightness = level->GetLevelInfo() ? (float)level->GetLevelInfo()->Brightness : 1.0f;
	std::unordered_set<AActor*> baked;
	for (INT i = 0; level->Model && i < level->Model->Lights.Num(); i++)
		baked.insert(level->Model->Lights(i));
	for (INT i = 0; i < level->Actors.Num(); i++)
	{
		AActor* light = level->Actors(i);
		if (!light || light->bDeleteMe || light->LightType == LT_None || !light->LightBrightness)
			continue;
		if ((light->bSpecialLit != 0) != specialLit)
			continue;
		const float radius = light->LightRadius * 25.0f;
		const FVector toLight = light->Location - point;
		const float distance = toLight.Size();
		if (distance >= radius || distance <= 0.0f)
			continue;
		const float cosine = (toLight | normal) / distance;
		if (cosine <= 0.0f && light->LightEffect != LE_NonIncidence)
			continue;
		const float incidence = light->LightEffect == LE_NonIncidence ? 1.0f : cosine;
		// A spotlight's cone, as the trace and Render.dll take it.
		float spot = 1.0f;
		if (light->LightEffect == LE_Spotlight || light->LightEffect == LE_StaticSpot)
		{
			APawn* pawn = Cast<APawn>(light);
			const float along = -((toLight / distance) | (pawn ? pawn->ViewRotation : light->Rotation).Vector());
			const float edge = 1.0f - light->LightCone / 256.0f;
			if (along <= edge)
				continue;
			const float f = (along - edge) / Max(1.0f - edge, 0.0001f);
			spot = f * f;
		}
		const float x = distance / radius;
		const float smooth = 1.0f - 3.0f * x * x + 2.0f * x * x * x;
		const float brightness = light->LightBrightness / 255.0f;
		// A baked light's shadow mask on this surface, which the trace holds
		// it to as well unless PT BAKEDSHADOWS has it left out; -1 where the
		// surface has no lightmap.
		const float bakedMask = baked.count(light) ? LevelScene::BakedMaskAt(level->Model, iSurf, light, point) : -1.0f;
		const float mask = baked.count(light) ? 2.0f * (bakedMask >= 0.0f ? bakedMask : 1.0f) : 1.0f;
		const float tracedMask = (baked.count(light) && bakedMask >= 0.0f && (DisableBits & 16384u)) ? 2.0f : mask;
		const FPlane c = FGetHSV(light->LightHue, light->LightSaturation, 255);
		Entry e;
		e.Light = light;
		e.Mask = bakedMask;
		const float colour[3] = { c.X, c.Y, c.Z };
		for (int k = 0; k < 3; k++)
		{
			e.Engine[k] = Min(mask * colour[k] * brightness * levelBrightness * smooth * spot * incidence, 1.0f);
			e.Traced[k] = EngineLightingNow
				? Min(tracedMask * colour[k] * brightness * smooth * spot * incidence, 1.0f)
				: powf(colour[k], 2.2f) * brightness * (1.0f - x) * spot * incidence;
		}
		e.Baked = baked.count(light) != 0;
		e.Distance = distance;
		e.Radius = radius;
		e.Cosine = cosine;
		e.Clear = level->Model->FastLineCheck(light->Location, from) != 0;
		entries.push_back(e);
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.Engine[1] > b.Engine[1]; });
	float engineAll[3] = {}, engineClear[3] = {}, tracedClear[3] = {};
	int clearCount = 0;
	for (const Entry& e : entries)
	{
		for (int k = 0; k < 3; k++)
		{
			engineAll[k] += e.Engine[k];
			if (e.Clear)
			{
				engineClear[k] += e.Engine[k];
				tracedClear[k] += e.Traced[k];
			}
		}
		clearCount += e.Clear ? 1 : 0;
	}
	debugf(TEXT("PT: lighting at %.0f %.0f %.0f facing %.2f %.2f %.2f, texture %s, level brightness %.2f: %d lights in reach, %d not blocked by the level"),
		point.X, point.Y, point.Z, normal.X, normal.Y, normal.Z, texture ? texture->GetName() : TEXT("none"), levelBrightness,
		(int)entries.size(), clearCount);
	for (size_t i = 0; i < entries.size() && i < 24; i++)
	{
		const Entry& e = entries[i];
		debugf(TEXT("  %s %s: brightness %d hue %d saturation %d, %.0f of %.0f away, cosine %.2f, %s, %s; engine %.3f %.3f %.3f, trace %.4f %.4f %.4f"),
			e.Light->GetName(), e.Light->GetClass()->GetName(), (int)e.Light->LightBrightness, (int)e.Light->LightHue,
			(int)e.Light->LightSaturation, e.Distance, e.Radius, e.Cosine, e.Clear ? TEXT("clear") : TEXT("BLOCKED"),
			!e.Baked ? TEXT("dynamic") : (e.Mask < 0.0f ? TEXT("baked, no lightmap here") : *FString::Printf(TEXT("baked, shadow mask %.2f"), e.Mask)),
			e.Engine[0], e.Engine[1], e.Engine[2], e.Traced[0], e.Traced[1], e.Traced[2]);
	}
	debugf(TEXT("  engine's lightmap, unclear lights too: %.3f %.3f %.3f; the clear ones: %.3f %.3f %.3f (1 is the lightmap's full, which the devices draw at twice the texture, before the ambient); trace, the clear ones: %.4f %.4f %.4f %s"),
		engineAll[0], engineAll[1], engineAll[2], engineClear[0], engineClear[1], engineClear[2], tracedClear[0], tracedClear[1], tracedClear[2],
		EngineLightingNow ? TEXT("as displayed, summed as the engine sums them") : TEXT("linear"));
	Ar.Logf(TEXT("PT: %d lights reach this point, %d unblocked; engine lightmap %.2f, trace %.3f (green); details in the log"),
		(int)entries.size(), clearCount, engineClear[1], tracedClear[1]);
}

VulkanDescriptorSet* UPathTracerRenderDevice::TileSet(CachedTexture* texture, int mode)
{
	std::unique_ptr<VulkanDescriptorSet>& set = texture->Sets[mode];
	if (!set)
	{
		set = TileDescriptorPool->allocate(TileSetLayout.get());
		WriteDescriptors()
			.AddCombinedImageSampler(set.get(), 0, texture->View.get(), TileSamplers[mode].get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			.Execute(Device.get());
	}
	return set.get();
}

void UPathTracerRenderDevice::CreateTilePipeline()
{
	for (int mode = 0; mode < 4; mode++)
	{
		const VkFilter filter = (mode & 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		const VkSamplerAddressMode address = (mode & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
		TileSamplers[mode] = SamplerBuilder()
			.MinFilter(filter)
			.MagFilter(filter)
			.AddressMode(address, address, address)
			.DebugName("PathTracerTileSampler")
			.Create(Device.get());
	}

	TileSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("PathTracerTileSetLayout")
		.Create(Device.get());

	// A set per cached texture for each way it is sampled, which in practice is
	// one or two. A generous ceiling: a Deus Ex menu touches a few hundred
	// distinct textures at most, and the pool is only reset when the cache is
	// flushed.
	TileDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192)
		.MaxSets(8192)
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

	for (int mode = 0; mode < 4; mode++)
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

		// The ways this engine composites 2D art: alpha blended (masked
		// art's holes being alpha), translucent, modulated, and 469's
		// PF_Highlighted, premultiplied.
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
		else if (mode == 3)
		{
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
		VulkanDescriptorSet* set = batch.Texture ? batch.Texture->Sets[batch.SamplerMode].get() : nullptr;
		if (!set || batch.VertexCount == 0)
			continue;

		if (batch.BlendMode != boundMode)
		{
			commands->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, TilePipelines[batch.BlendMode].get());
			boundMode = batch.BlendMode;
		}

		commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, TilePipelineLayout.get(), 0, set);
		commands->draw(batch.VertexCount, 1, batch.FirstVertex, 0);
	}

	commands->endRenderPass();

	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void UPathTracerRenderDevice::CreateBrightnessPipeline()
{
	BrightnessShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Brightness.comp", Shaders::Brightness())
		.DebugName("PathTracerBrightness")
		.Create("PathTracerBrightness", Device.get());

	BrightnessSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerBrightnessSetLayout")
		.Create(Device.get());
	BrightnessDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1)
		.MaxSets(1)
		.DebugName("PathTracerBrightnessDescriptorPool")
		.Create(Device.get());
	BrightnessSet = BrightnessDescriptorPool->allocate(BrightnessSetLayout.get());

	BrightnessPipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(BrightnessSetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(int32_t) * 4)
		.DebugName("PathTracerBrightnessPipelineLayout")
		.Create(Device.get());
	BrightnessPipeline = ComputePipelineBuilder()
		.Layout(BrightnessPipelineLayout.get())
		.ComputeShader(BrightnessShader.get())
		.DebugName("PathTracerBrightnessPipeline")
		.Create(Device.get());
}

// The game's Brightness setting - the client's, which its menu's slider sets
// - as a gamma curve over the finished picture, the 2D included, the way the
// original D3D driver's gamma ramp lifts the whole screen. The curve is
// VulkanDrv's, pow(c, 1 / (2 * Brightness)), which leaves the picture as it
// is at the default of 0.5; the original's ramp is 2.5 * Brightness, a lift
// even at the default. Under Proton's Wayland driver, which cannot set a
// gamma ramp, the original's does nothing at all.
void UPathTracerRenderDevice::ApplyBrightness(VulkanCommandBuffer* commands)
{
	if (!BrightnessPipeline || !OutputImage || !Viewport)
		return;
	const float brightness = Clamp(Viewport->GetOuterUClient()->Brightness * 2.0f, 0.05f, 2.99f);
	if (fabs(brightness - 1.0f) < 0.001f)
		return;

	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	struct { float InvGamma[4]; int32_t Size[4]; } constants = { { 1.0f / brightness, 0.0f, 0.0f, 0.0f }, { TraceWidth, TraceHeight, 0, 0 } };
	commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, BrightnessPipeline.get());
	commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, BrightnessPipelineLayout.get(), 0, BrightnessSet.get());
	commands->pushConstants(BrightnessPipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
	commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);

	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void UPathTracerRenderDevice::CreateEncodePipeline()
{
	EncodeShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/Encode.comp", Shaders::Encode())
		.DebugName("PathTracerEncode")
		.Create("PathTracerEncode", Device.get());

	EncodeSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerEncodeSetLayout")
		.Create(Device.get());
	EncodeDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4)
		.MaxSets(2)
		.DebugName("PathTracerEncodeDescriptorPool")
		.Create(Device.get());
	PresentSet = EncodeDescriptorPool->allocate(EncodeSetLayout.get());
	SdrSet = EncodeDescriptorPool->allocate(EncodeSetLayout.get());

	EncodePipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(EncodeSetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(int32_t) * 4)
		.DebugName("PathTracerEncodePipelineLayout")
		.Create(Device.get());
	EncodePipeline = ComputePipelineBuilder()
		.Layout(EncodePipelineLayout.get())
		.ComputeShader(EncodeShader.get())
		.DebugName("PathTracerEncodePipeline")
		.Create(Device.get());
}

float UPathTracerRenderDevice::ToneCeiling() const
{
	if (!HdrMode)
		return 1.0f;
	return Max(Clamp(HdrPeakNits, 100, 10000) / (float)Clamp(HdrPaperWhite, 80, 1000), 1.0f);
}

// Made with the output image's size the first time they are wanted after it
// changes, which drops them.
void UPathTracerRenderDevice::EnsureEncodeImages()
{
	if (PresentImage || !OutputImage)
		return;
	auto makeImage = [&](std::unique_ptr<VulkanImage>& image, std::unique_ptr<VulkanImageView>& view, const char* name)
	{
		image = ImageBuilder()
			.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
			.Size(TraceWidth, TraceHeight)
			.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			.DebugName(name)
			.Create(Device.get());
		view = ImageViewBuilder().Image(image.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName(name).Create(Device.get());
	};
	makeImage(PresentImage, PresentView, "PathTracerPresent");
	makeImage(SdrImage, SdrView, "PathTracerSdr");
	WriteDescriptors()
		.AddStorageImage(PresentSet.get(), 0, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(PresentSet.get(), 1, PresentView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(SdrSet.get(), 0, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(SdrSet.get(), 1, SdrView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.Execute(Device.get());
}

// The finished picture, the 2D over it and the Brightness applied, encoded
// for the HDR swap chain - or, for a picture saved while HDR is on, brought
// back to what SDR would have shown. Left ready to be blitted from. The
// output image itself is left as it is: a frame the helper has not replaced
// is drawn again from it, and would otherwise be encoded twice.
void UPathTracerRenderDevice::EncodeFrame(VulkanCommandBuffer* commands, bool forSaving)
{
	EnsureEncodeImages();
	if (!PresentImage || !EncodePipeline)
		return;
	VulkanImage* target = forSaving ? SdrImage.get() : PresentImage.get();
	PipelineBarrier()
		.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		.AddImage(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	struct { float Params[4]; int32_t Size[4]; } constants = {
		{ forSaving ? 3.0f : (float)HdrMode, (float)Clamp(HdrPaperWhite, 80, 1000), ToneCeiling(), 0.0f },
		{ TraceWidth, TraceHeight, 0, 0 } };
	commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, EncodePipeline.get());
	commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, EncodePipelineLayout.get(), 0, forSaving ? SdrSet.get() : PresentSet.get());
	commands->pushConstants(EncodePipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
	commands->dispatch((TraceWidth + 7) / 8, (TraceHeight + 7) / 8, 1);

	PipelineBarrier()
		.AddImage(target, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
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
	PathTracerWindowEvent("SetRes start", window);

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

	EnumWindows(PathTracerHideSplash, (LPARAM)window);

	// The UI pinned to a narrower box: the engine is given a mode of that
	// shape at the height asked for, and lays its HUD, menus and conversations
	// out in it, while the trace fills the screen around it (see
	// CreateSwapChainResources). Only in fullscreen, where the window is the
	// screen whatever mode the engine has; in a window it would only shrink it.
	INT engineX = NewX;
	if (Fullscreen && PinnedAspect > 0.0f && NewX > NewY * PinnedAspect + 1.0f)
	{
		engineX = (INT)(NewY * PinnedAspect + 0.5f) & ~1;
		PathTracerEvent("UI pinned: the engine gets %dx%d", (int)engineX, (int)NewY);
	}

	// BLIT_Fullscreen even though the window below is only borderless. The engine
	// keys a good deal off believing it is fullscreen - input capture, the
	// pointer clip, and whether the system cursor is shown - so telling it
	// otherwise leaves the desktop cursor on screen underneath the one the game
	// draws itself. The actual display mode change it asks for is undone by the
	// restyle that follows; UseDirectDraw=False in DeusEx.ini removes it
	// entirely, which a device presenting through Vulkan has no use for.
	if (!Viewport->ResizeViewport(Fullscreen ? (BLIT_Fullscreen | BLIT_Direct3D) : (BLIT_HardwarePaint | BLIT_Direct3D), engineX, NewY, NewColorBytes))
		return 0;
	PathTracerWindowEvent("engine resized", window);

	// The trace fills out to the mode chosen, not to the window: a mode chosen
	// narrower than the screen is traced as it is and letterboxed.
	PinnedModeWidth = engineX != NewX ? NewX : 0;
	PinnedModeHeight = NewY;

	// The engine keeps the mode it was given as the one to start in next time.
	// Keep the one the player chose instead, so that with the pin taken off
	// the game comes back at the whole width rather than the narrower mode.
	if (engineX != NewX)
	{
		UClient* client = Viewport->GetOuterUClient();
		client->FullscreenViewportX = NewX;
		client->FullscreenViewportY = NewY;
		client->SaveConfig();
	}

	if (enteringFullscreen)
	{
		HDC screenDC = GetDC(0);
		int screenWidth = GetDeviceCaps(screenDC, HORZRES);
		int screenHeight = GetDeviceCaps(screenDC, VERTRES);
		ReleaseDC(0, screenDC);

		PathTracerEvent("screen %dx%d", screenWidth, screenHeight);
		SetWindowLong(window, GWL_STYLE, WS_OVERLAPPED | WS_VISIBLE);
		SetWindowLong(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
		PathTracerWindowEvent("restyled", window);
		SetWindowPos(window, HWND_TOP, 0, 0, screenWidth, screenHeight, SWP_FRAMECHANGED | SWP_NOSENDCHANGING | SWP_NOZORDER);
		PathTracerWindowEvent("placed", window);

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
	}

	// The clip is not gated on wasActive, and must not be: the foreground can
	// arrive during SetRes rather than before it, since hiding the splash hands
	// it to the game window a moment after wasActive was read. Gated, the first
	// SetRes of all skipped this and left the clip describing the 640x480
	// window the engine captured while the client had become the whole screen,
	// so every recentre came back as movement and mouse look span on startup
	// until an alt-tab re-clipped it. The other three devices have always done
	// this unconditionally. It is harmless when the game is not in front: the
	// clip is the desktop then, which ReclipCursorToWindow leaves alone.
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
	// The helper follows whatever size the frames are asked for at.
	const int uiWidth = Max((int)Viewport->SizeX, 1);
	int width = uiWidth;
	int height = Max((int)Viewport->SizeY, 1);

	// Except with the UI pinned, where the engine's view is narrower than the
	// mode the player chose on purpose: the trace is the mode chosen, and the
	// engine's view - and everything it draws in 2D - sits in the middle of
	// it. Filling out to the window instead traced a 1920x1440 mode at
	// 3440x1440 on a 21:9 screen, and a 640x480 one at 1147x480.
	if (PinnedModeWidth > uiWidth && PinnedModeHeight == height && FullscreenState.Enabled)
	{
		width = Min(PinnedModeWidth, 16384);
		width += (width - uiWidth) & 1;
	}
	UiOffsetX = (width - uiWidth) / 2;

	if (OutputImage && width == TraceWidth && height == TraceHeight)
		return;
	PathTracerEvent("trace buffers %dx%d -> %dx%d", TraceWidth, TraceHeight, width, height);

	vkDeviceWaitIdle(Device->device);

	TileFramebuffer.reset();
	PresentView.reset();
	PresentImage.reset();
	SdrView.reset();
	SdrImage.reset();
	OutputView.reset();
	OutputImage.reset();

	OutputImage = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)
		.DebugName("PathTracerOutput")
		.Create(Device.get());
	OutputView = ImageViewBuilder().Image(OutputImage.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerOutputView").Create(Device.get());

	InsetSource = std::make_unique<CachedTexture>();
	InsetSource->Width = width;
	InsetSource->Height = height;
	InsetSource->Image = ImageBuilder()
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("PathTracerInsetSource")
		.Create(Device.get());
	InsetSource->View = ImageViewBuilder().Image(InsetSource->Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerInsetSourceView").Create(Device.get());
	InsetSourceFresh = true;

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

	if (BrightnessSet)
	{
		WriteDescriptors()
			.AddStorageImage(BrightnessSet.get(), 0, OutputView.get(), VK_IMAGE_LAYOUT_GENERAL)
			.Execute(Device.get());
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

// PT BENCH's steps: the settings as they are, then each costlier feature
// switched off on its own, then the settings again to show how far the
// measurement drifted.
static const TCHAR* const BenchSteps[] = {
	TEXT("as set"),
	TEXT("linear lighting, not the engine's"),
	TEXT("no baked shadow masks"),
	TEXT("no mipmaps"),
	TEXT("trilinear, not anisotropic"),
	TEXT("no detail textures"),
	TEXT("meshes lit as flat surfaces"),
	TEXT("hard shadows, from points"),
	TEXT("glowing surfaces light nothing"),
	TEXT("glowing surfaces not sampled"),
	TEXT("fog unshadowed"),
	TEXT("the game's light augmentation"),
	TEXT("light untinted by glass"),
	TEXT("dry streets"),
	TEXT("no bump mapping"),
	TEXT("one bounce"),
	TEXT("no shadows"),
	// Bounds rather than features: what judging the see-through surfaces
	// a triangle at a time costs every ray, and what the lights cost
	// altogether - the most any work on either could give back.
	TEXT("see-through surfaces solid"),
	TEXT("no lights"),
	// The other way: what drawing from a crowded cell's lights saves, as
	// against weighing every one.
	TEXT("every light weighed"),
	TEXT("as set, again"),
};
static const int BenchStepCount = (int)(sizeof(BenchSteps) / sizeof(BenchSteps[0]));

void UPathTracerRenderDevice::StartBench()
{
	Bench.DisableBits = DisableBits;
	Bench.EngineLighting = EngineLightingNow;
	Bench.Detail = DetailTextures != 0;
	Bench.Anisotropy = MaxAnisotropy;
	Bench.LightSize = LightSizeNow;
	Bench.Bounces = Bounces;
	Bench.Wetness = Wetness;
	Bench.BumpMapping = BumpMapping;
	// Off meanwhile, as the frame limit is, so the frame times are the
	// frame's own; the swap chain is made again for it.
	Bench.Vsync = UseVSync != 0;
	UseVSync = 0;
	Bench.Results.clear();
	Bench.Step = 0;
	Bench.Frame = 0;
	ApplyBenchStep(0);
}

// Everything back as it was when the benchmark started, then this step's one
// change. False when the step would change nothing - the feature already off.
bool UPathTracerRenderDevice::ApplyBenchStep(int step)
{
	DisableBits = Bench.DisableBits;
	EngineLightingNow = Bench.EngineLighting;
	DetailTextures = Bench.Detail ? 1 : 0;
	MaxAnisotropy = Bench.Anisotropy;
	LightSizeNow = Bench.LightSize;
	Bounces = Bench.Bounces;
	Wetness = Bench.Wetness;
	BumpMapping = Bench.BumpMapping;
	AccumulatedFrames = 0;
	DenoiseRestart = true;

	auto setBit = [&](uint32_t bit)
	{
		if (DisableBits & bit)
			return false;
		DisableBits |= bit;
		return true;
	};
	switch (step)
	{
	case 1:  if (!EngineLightingNow) return false; EngineLightingNow = false; return true;
	case 2:  return EngineLightingNow && setBit(16384u);
	case 3:  return setBit(4096u);
	case 4:  if (MaxAnisotropy <= 1.0f) return false; MaxAnisotropy = 1.0f; return true;
	case 5:  if (!DetailTextures) return false; DetailTextures = 0; return true;
	case 6:  return setBit(1024u);
	case 7:  if (LightSizeNow <= 0) return false; LightSizeNow = 0; return true;
	case 8:  return setBit(512u);
	case 9:  return setBit(262144u);
	// Any glow in the last couple of seconds: a strobe or a flickering fog
	// light is out of the list on the frames it is dark.
	case 10: return FrameIndex - LastFogFrame < 120u && !(DisableBits & 32u) && setBit(65536u);
	case 11: return Scene.Flashlight[0].w > 0.0f && setBit(131072u);
	case 12: return setBit(1048576u);
	case 13: if (Wetness <= 0) return false; Wetness = 0; return true;
	case 14: if (BumpMapping <= 0) return false; BumpMapping = 0; return true;
	case 15: if (Bounces <= 1) return false; Bounces = 1; return true;
	case 16: return setBit(2u);
	case 17: return setBit(8u);
	case 18: return setBit(1u);
	case 19: return setBit(2097152u);
	default: return true;
	}
}

// Once a traced frame: a step settles, is measured, and gives way to the next
// that changes anything; after the last, everything goes back and the table
// is logged.
void UPathTracerRenderDevice::AdvanceBench()
{
	if (Bench.Step < 0)
		return;
	if (++Bench.Frame == Bench.SettleFrames())
	{
		Bench.Gpu = Bench.Trace = Bench.Collect = Bench.FrameSum = 0.0;
		Bench.GpuFrames = Bench.Frames = 0;
	}
	if (Bench.Frame < Bench.SettleFrames() + BenchState::Measure)
		return;

	const double g = Max(Bench.GpuFrames, 1), f = Max(Bench.Frames, 1);
	Bench.Results.push_back({ BenchSteps[Bench.Step], Bench.Gpu / g, Bench.Trace / g, Bench.Collect / f, Bench.FrameSum / f });
	do
		Bench.Step++;
	while (Bench.Step < BenchStepCount && !ApplyBenchStep(Bench.Step));
	Bench.Frame = 0;
	if (Bench.Step >= BenchStepCount)
	{
		ApplyBenchStep(0);
		Bench.Step = -1;
		LogBench();
		UseVSync = Bench.Vsync ? 1 : 0;
	}
}

void UPathTracerRenderDevice::LogBench()
{
	if (Bench.Results.empty())
		return;
	const TraceProtocol::Header* status = (Tracer && Tracer->Alive()) ? &Tracer->Status() : nullptr;
	static const char* denoisers[] = { "off", "NRD", "DLSS-RR" };
	const TCHAR* map = (Viewport && Viewport->Actor && Viewport->Actor->XLevel && Viewport->Actor->XLevel->GetOuter())
		? Viewport->Actor->XLevel->GetOuter()->GetName() : TEXT("?");
	char line[512];
	snprintf(line, sizeof(line), "PT BENCH: %s, %dx%d traced at %ux%u, denoiser %s, materials %s, bounces %d, %d instances, %d lights, vsync %s",
		Narrow(map).c_str(), TraceWidth, TraceHeight, status ? status->RenderWidth : 0u, status ? status->RenderHeight : 0u,
		denoisers[Min<uint32_t>(status ? status->DenoisedWith : 0u, 2)], MaterialsEnabled ? "on" : "off", (int)Bounces,
		(int)Scene.Instances.size(), (int)Scene.Lights.size(), UsingVsync ? "on" : "off");
	WriteTimingLine(line);
	WriteTimingLine("PT BENCH:                                     GPU ms (trace)   collect ms   frame ms   fps    change: GPU, frame");
	const BenchState::Result& base = Bench.Results[0];
	for (const BenchState::Result& r : Bench.Results)
	{
		snprintf(line, sizeof(line), "PT BENCH:   %-34s %6.2f (%5.2f)   %6.2f     %7.2f  %6.1f   %+6.2f, %+6.2f",
			Narrow(r.Name).c_str(), r.Gpu, r.Trace, r.Collect, r.Frame, r.Frame > 0.0 ? 1000.0 / r.Frame : 0.0,
			r.Gpu - base.Gpu, r.Frame - base.Frame);
		WriteTimingLine(line);
	}
	if (Viewport && Viewport->Actor)
		Viewport->Actor->eventClientMessage(TEXT("PT BENCH: done, the table is in PathTracerTimings.log"), NAME_None, 0);
}

// The scene holds the engine's own objects - the level's textures, meshes and
// actors - by pointer from one frame to the next, and a garbage collection can
// free any of them. EnsureSceneBuilt rebuilds when the level changes, but it
// can only tell by the level's address and size: loading a save of the map
// already being played frees the old level and loads the same map again,
// which can land at the same address with the same number of nodes. The
// scene then went on animating textures that had been freed, calling into
// whatever had taken their place - a HUD graphics context, the engine said,
// as it gave up. So a collection of any kind drops everything held, and the
// scene is built again from the level as it now is.
//
// Looked up through the object table rather than dereferenced, since once
// collected it is freed memory; the name makes sure an object that has taken
// both its slot and its address is not taken for it.
bool UPathTracerRenderDevice::GarbageCollected()
{
	const bool alive = GcSentinel && UObject::GetIndexedObject(GcSentinelIndex) == GcSentinel &&
		GcSentinel->GetFName() == GcSentinelName;
	if (alive)
		return false;

	const bool collected = GcSentinel != nullptr;
	static int serial = 0;
	GcSentinelName = FName(*FString::Printf(TEXT("PathTracerGcSentinel%d"), ++serial));
	GcSentinel = UObject::StaticConstructObject(UTextBuffer::StaticClass(), UObject::GetTransientPackage(), GcSentinelName, 0);
	GcSentinelIndex = GcSentinel ? GcSentinel->GetIndex() : INDEX_NONE;
	return collected;
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
		RepairPhotoSave();
	}

	// Every frame: where the movers and the mesh actors are now. New shapes are
	// sent to the helper when the frame is.
	const size_t geometriesBefore = Scene.Geometries.size();
	Scene.LightScale = Max(LightScale, 1) / 100.0f;
	Scene.ViewportTime = EngineSeconds(Viewport->CurrentTime);
	Scene.HighlightSpecialLights = (DisableBits & 16u) != 0;
	Scene.UseFlashlight = (DisableBits & 131072u) == 0;
	Scene.FlashlightBrightness = Max(FlashlightBrightness, 0) / 100.0f;
	Scene.FlashlightHaze = Max(FlashlightHaze, 0) / 100.0f;
	// Gathering is CPU only, so it runs while the GPU is still presenting the
	// last frame.
	const double collectStart = NowMs();
	// In photo mode the world is held by the engine, but not the player,
	// whose flying plays a pose and turns the body with the view: gathered
	// as it stood, with the flashlight where it shone from.
	Scene.PhotoMode = Photo.Active;
	Scene.PhotoEye = Photo.Eye;
	if (Photo.Active)
		SwapPhotoPose();
	Scene.CollectDynamic(level);
	if (Photo.Active)
	{
		SwapPhotoPose();
		for (int i = 0; i < 3; i++)
			Scene.Flashlight[i] = Photo.Flashlight[i];
	}
	if (!Scene.FogLights.empty())
		LastFogFrame = FrameIndex;
	Timings.Collect += NowMs() - collectStart;
	if (Bench.Measuring())
		Bench.Collect += NowMs() - collectStart;
	CollectedThisFrame = true;

	// Only when something new appeared, so this says what is being traced
	// without filling the log every frame.
	if (Scene.Geometries.size() != geometriesBefore)
	{
		debugf(TEXT("PathTracer: %d shapes, %d instances this frame"),
			(int)Scene.Geometries.size(), (int)Scene.Instances.size());
	}
}

// A view the HUD draws in a window: traced by the helper after the player's
// from its own eye (TraceRenderer::RecordInsets) and written into the picture
// at the window, then drawn back from a copy of the picture at this point
// among the 2D, so what the HUD drew before it - a computer's screen behind
// its camera views - lies under it, and what it draws after, over it.
void UPathTracerRenderDevice::AddInsetView(FSceneNode* Frame)
{
	if (InsetViews.size() >= TraceProtocol::MaxInsets || !InsetSource || TraceWidth <= 0 || TraceHeight <= 0 || Frame->Proj.Z <= 0.0f)
		return;

	// A frame that draws only these - a security computer's screen hides the
	// player's view - still wants the scene where things are now. Either
	// way, the camera the window is seen from is hidden now, while it draws.
	if (!CollectedThisFrame)
	{
		Scene.ViewFrame = Frame;
		EnsureSceneBuilt(Frame->Level);
		Scene.ViewFrame = nullptr;
	}
	Scene.HideFromWindows();

	InsetView view;
	view.X = Frame->XB + UiOffsetX;
	view.Y = Frame->YB;
	view.Width = Min(Frame->X, TraceWidth - view.X);
	view.Height = Min(Frame->Y, TraceHeight - view.Y);
	if (view.X < 0 || view.Y < 0 || view.Width <= 0 || view.Height <= 0)
		return;
	// As the player's view is built (see below): the axes of the node's
	// coordinates, scaled to the window's edges by its focal length.
	const FCoords& c = Frame->Coords;
	const float halfWidth = Frame->FX * 0.5f / Frame->Proj.Z;
	const float halfHeight = Frame->FY * 0.5f / Frame->Proj.Z;
	view.Camera.Origin = vec4(c.Origin.X, c.Origin.Y, c.Origin.Z, 0.0f);
	view.Camera.Right = vec4(c.XAxis.X, c.XAxis.Y, c.XAxis.Z, 0.0f) * halfWidth;
	view.Camera.Up = vec4(c.YAxis.X, c.YAxis.Y, c.YAxis.Z, 0.0f) * halfHeight;
	view.Camera.Forward = vec4(c.ZAxis.X, c.ZAxis.Y, c.ZAxis.Z, 0.0f);
	InsetViews.push_back(view);

	// Drawn back here among the tiles, texel for texel.
	const int samplerMode = 3;
	if (!TileSet(InsetSource.get(), samplerMode))
		return;
	const float x0 = (float)view.X, y0 = (float)view.Y, x1 = x0 + view.Width, y1 = y0 + view.Height;
	const float sx = 2.0f / (float)TraceWidth, sy = 2.0f / (float)TraceHeight;
	const vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
	TileVertex corners[4];
	corners[0] = { vec2(x0 * sx - 1.0f, y0 * sy - 1.0f), vec2(x0 / TraceWidth, y0 / TraceHeight), white };
	corners[1] = { vec2(x1 * sx - 1.0f, y0 * sy - 1.0f), vec2(x1 / TraceWidth, y0 / TraceHeight), white };
	corners[2] = { vec2(x1 * sx - 1.0f, y1 * sy - 1.0f), vec2(x1 / TraceWidth, y1 / TraceHeight), white };
	corners[3] = { vec2(x0 * sx - 1.0f, y1 * sy - 1.0f), vec2(x0 / TraceWidth, y1 / TraceHeight), white };
	TileBatch batch;
	batch.Texture = InsetSource.get();
	batch.BlendMode = 0;
	batch.SamplerMode = samplerMode;
	batch.FirstVertex = (int)TileVertices.size();
	batch.VertexCount = 6;
	TileBatches.push_back(batch);
	const int order[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++)
		TileVertices.push_back(corners[order[i]]);
}

void UPathTracerRenderDevice::SetSceneNode(FSceneNode* Frame)
{
	guardSlow(UPathTracerRenderDevice::SetSceneNode);

	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: scene node %dx%d at %d %d, from %.0f %.0f %.0f, viewport actor %s"),
			Frame->X, Frame->Y, Frame->XB, Frame->YB, Frame->Coords.Origin.X, Frame->Coords.Origin.Y, Frame->Coords.Origin.Z,
			(Frame->Viewport && Frame->Viewport->Actor) ? Frame->Viewport->Actor->GetName() : TEXT("none"));

	// A view the HUD draws in a window of its own: Extension's XViewportWindow
	// has the engine render one through a scene node of its own, the size of
	// its window - narrower than the screen, where the player's view, a
	// mirror's and the sky's are its whole width.
	// A child of one - the sky zone or a mirror seen in it, the same window
	// again - is its own business, as the player's view's children are.
	//
	// UT has no such windows: its narrower nodes are the menus' previews of
	// a model, drawn by DrawClippedActor from an actor in the entry level,
	// which the trace of this level has nothing to show for.
	if (Viewport && Frame->X > 0 && Frame->Y > 0 && Frame->X < Viewport->SizeX)
	{
#if defined(DEUSEX)
		// Photo mode leaves the HUD, and the views in its windows, out.
		if (!Frame->Parent && !Photo.Active)
			AddInsetView(Frame);
#endif
		return;
	}

	// The first scene node of a frame is the player's view. Later ones are
	// mirrors, skyboxes and the weapon, which this device does not yet treat
	// separately - taking the first keeps the camera stable.
	if (HaveCamera)
		return;

	// Whose view this is, so the actor collection can skip the player's own body
	// and honour the owner-only visibility flags.
	Scene.ViewActor = Frame->Viewport ? Frame->Viewport->Actor : nullptr;
	APlayerPawn* viewer = Cast<APlayerPawn>(Scene.ViewActor);
	Scene.ViewFromBehind = viewer && viewer->bBehindView;
#if defined(OLDUNREAL469SDK)
	Scene.ViewTarget = (viewer && viewer->ViewTarget != viewer) ? viewer->ViewTarget : nullptr;
#endif

	// The view's basis, kept for placing the first person weapon.
	Scene.ViewOrigin = Frame->Coords.Origin;
	Scene.ViewRight = Frame->Coords.XAxis;
	Scene.ViewDown = Frame->Coords.YAxis;
	Scene.ViewForward = Frame->Coords.ZAxis;
	// In photo mode the view looks the player's way from the free camera.
	if (Photo.Active && Scene.ViewActor == Photo.Pawn)
	{
		MovePhotoCamera();
		Scene.ViewOrigin = Photo.Position;
	}

	Scene.ViewFrame = Frame;
	EnsureSceneBuilt(Frame->Level);
	Scene.ViewFrame = nullptr;

	// The engine projects a point as X * Proj.Z / Z: Proj.Z is the focal
	// length in pixels, and carries the field of view this node is actually
	// rendered with - including the cinematic cameras', which do not use the
	// player's FovAngle. That field of view spans the frame's width.
	//
	// The camera is built from it over the whole trace rather than from the
	// frame's size. The frame can be a strip of the screen - a conversation
	// narrows it to the space between its black bars - and the trace can be
	// wider than the engine's view, when the UI is pinned. Either way the view
	// stays centred, and one focal length describes all of it.
	float halfWidth = 1.0f;
	float halfHeight = (float)TraceHeight / (float)TraceWidth;
	if (Frame->Proj.Z > 0.0f && Frame->FX > 0.0f && Viewport && Viewport->SizeY > 0)
	{
		float focal = Frame->Proj.Z;
		const float tanHalfFov = Frame->FX / (2.0f * focal);
		const float screenX = (float)Viewport->SizeX, screenY = (float)Viewport->SizeY;

		// That field of view is horizontal: FovAngle across the screen, whatever
		// the screen's shape. On one wider than 4:3 the engine keeps the width and
		// crops the top and bottom - at 21:9 its 75 degrees shows under 60% of the
		// height it does at 4:3, and the conversations and cinematics, framed for
		// 4:3, lose heads and feet. The trace builds the view from the level
		// itself, so it can keep the height the same FovAngle gives at 4:3 and
		// widen the view to fill the screen instead. (A raster device has to have
		// the engine compute the wider view, since it draws only what the engine
		// culled for its own - see VulkanDrv's WidescreenComputeRenderSize.) A
		// screen 4:3 or narrower is left as the engine has it.
		if (WidescreenFovEnabled && screenX * 3.0f > screenY * 4.0f)
			focal = Min(screenY * 0.5f / (0.75f * tanHalfFov), focal);

		halfWidth = (float)TraceWidth * 0.5f / focal;
		halfHeight = (float)TraceHeight * 0.5f / focal;

		// The HUD projects the world through this frame too. The brackets
		// around what the player can use and the augmentations' target boxes
		// are placed by XRootWindow::ConvertVectorToCoordinates, from a copy
		// of this node taken as the HUD is drawn, at Proj.Z. Given the trace's
		// focal length they land on what the trace shows rather than where
		// the engine's narrower view would have had it. The engine's own world
		// pass runs on with it too, but everything that draws is thrown away,
		// and the focal length only ever shortens here, drawing its points
		// nearer the middle rather than off the edge.
		Frame->Proj.Z = focal;
		Frame->RProj.Z = 1.0f / focal;
	}

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

	Camera.Origin = vec4(Scene.ViewOrigin.X, Scene.ViewOrigin.Y, Scene.ViewOrigin.Z, 0.0f);
	Camera.Right = vec4(c.XAxis.X, c.XAxis.Y, c.XAxis.Z, 0.0f) * halfWidth;
	Camera.Up = vec4(c.YAxis.X, c.YAxis.Y, c.YAxis.Z, 0.0f) * halfHeight;
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
		// Before anything this frame reaches for what the scene holds.
		if (GarbageCollected())
		{
			PathTracerEvent("garbage collected: the scene is built again");
			WaitForPreviousFrame();
			if (Device)
				vkDeviceWaitIdle(Device->device);
			Scene.Clear();
			SceneReset = true;
			DenoiseRestart = true;
			if (Textures)
				Textures->Clear();
		}

		CreateSwapChainResources();
		CheckPhoto();
		HaveCamera = false;
		FlashScale = InFlashScale;
		FlashFog = InFlashFog;
		TileVertices.clear();
		TileBatches.clear();
		InsetViews.clear();
		CollectedThisFrame = false;
		if (Textures)
			Textures->BeginFrame();
		LogDraws = LogDrawsArmed;
		LogDrawsArmed = false;
		LoggedDraws = 0;
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
	// A new client counts its refusals from none.
	RefusedSeen = 0;
#ifdef PATHTRACER_LOCAL
	// A 64-bit game traces in its own process, on this device.
	Tracer.reset(new TraceClient());
	if (!Tracer->StartLocal(Device.get()))
	{
		const std::string why = Tracer->Error();
		PathTracerEvent("tracer did not start: %s", why.c_str());
		debugf(TEXT("PathTracerDrv could not start tracing: %s"), *Widen(why.c_str()));
		debugf(TEXT("The GPU must offer ray tracing to Vulkan: VK_KHR_ray_query and VK_KHR_acceleration_structure."));
		Tracer.reset();
		return false;
	}
	{
		const TraceProtocol::Header& status = Tracer->Status();
		debugf(TEXT("PathTracer: tracing on %s%s"), *Widen(status.DeviceName),
			status.CanSampleTextures ? TEXT("") : TEXT(", which cannot index textures: surfaces will use one averaged colour each"));
		PathTracerEvent("tracing on %s", status.DeviceName);
		if (LogTimings)
		{
			char line[256];
			snprintf(line, sizeof(line), "PathTracer session: device built %s %s, tracing on %s", __DATE__, __TIME__, status.DeviceName);
			WriteTimingLine(line);
		}
	}
	SceneReset = true;
	TracerLost = false;
	return true;
#else
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
	if (LogTimings)
	{
		char line[256];
		snprintf(line, sizeof(line), "PathTracer session: device built %s %s, helper on %s", __DATE__, __TIME__, status.DeviceName);
		WriteTimingLine(line);
	}
	SceneReset = true;
	TracerLost = false;
	return true;
#endif
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
		LightmapsSent = false;
		EmittersSent = false;
		SceneReset = false;
	}

	// The engine's shadow masks, once a level. More than the command area
	// could carry would stop the helper, so a level that big goes without,
	// its baked lights shadowed by the trace alone.
	if (!LightmapsSent)
	{
		LightmapsSent = true;
		const size_t bytes = Scene.Lightmaps.size() * sizeof(uint32_t);
		if (bytes < (48u << 20))
			Tracer->Lightmaps(Scene.Lightmaps);
		else
		{
			debugf(TEXT("PathTracer lightmaps: %.1f MB is too much to send; the engine's shadows are left out"), bytes / (1024.0f * 1024.0f));
			Tracer->Lightmaps(std::vector<uint32_t>());
		}
	}

	// The glowing surfaces sampled as lights, once a level, likewise.
	if (!EmittersSent)
	{
		EmittersSent = true;
		const size_t bytes = Scene.Emitters.size() * sizeof(uint32_t);
		if (bytes < (16u << 20))
			Tracer->Emitters(Scene.Emitters);
		else
		{
			debugf(TEXT("PathTracer glowing surfaces: %.1f MB is too much to send; they light only by the bounces that find them"), bytes / (1024.0f * 1024.0f));
			Tracer->Emitters(std::vector<uint32_t>());
		}
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
	const size_t firstNew = SentTextures.size();
	int sentWithMips = 0, sentLevels = 0, sentS3tc = 0;
	for (size_t i = SentTextures.size(); i < Scene.Textures.size(); i++)
	{
		SentTexture sent;
		sent.Source = Scene.Textures[i];
		sent.Masked = Scene.TextureMasked[i];
		int width = 0, height = 0, levels = 0;
		uint32_t format = 0;
		// A texture that changes sends its frames at its own size, so it
		// keeps to its originals.
		const bool s3tc = UseS3TC && !TextureCache::Animates(sent.Source);
		const bool converted = TextureCache::SceneMips(sent.Source, sent.Masked, s3tc, Pixels, width, height, levels, format);
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
		// One that changes sends its top level alone from then on, so it has
		// no mips to fall behind it.
		const uint32_t sentMips = sent.Animated ? 1u : (uint32_t)levels;
		const int dropped = Tracer->Texture((uint32_t)i, (uint32_t)sent.Width, (uint32_t)sent.Height, converted ? Pixels.data() : nullptr,
			Scene.TextureMaterials[i], sent.Animated, sentMips, format);
		if (dropped)
		{
			if (dropped > 0)
				debugf(TEXT("PathTracer texture %d '%s' %dx%d is too big to send whole: sent from %dx%d"), (int)i, sent.Source->GetName(),
					sent.Width, sent.Height, Max(sent.Width >> dropped, 1), Max(sent.Height >> dropped, 1));
			else
				debugf(TEXT("PathTracer texture %d '%s' %dx%d is too big to send; it is left white"), (int)i, sent.Source->GetName(), sent.Width, sent.Height);
			// Its frames would not fit either.
			if (dropped < 0)
				sent.Width = sent.Height = 0;
		}
		SentTextures.push_back(sent);
		if (converted && sentMips > 1)
		{
			sentWithMips++;
			sentLevels += (int)sentMips;
		}
		if (converted && (format == TraceProtocol::TextureBc1 || (s3tc && (sent.Source->bHasComp || sent.Source->Format == TEXF_DXT1))))
			sentS3tc++;
	}
	if (SentTextures.size() > firstNew)
		debugf(TEXT("PathTracer textures: %d sent, %d of them with mips (%.1f levels on average), %d in S3TC"),
			(int)(SentTextures.size() - firstNew), sentWithMips, sentWithMips ? sentLevels / (float)sentWithMips : 0.0f, sentS3tc);

	// Asked afresh every frame rather than remembered from the first sending,
	// since a script can give a texture an animation chain after it was first
	// seen. One frame of an animation shown on its own - a sprite that plays
	// once chooses it - stays that frame: advancing it would loop it.
	//
	// On the viewport's clock, the one the engine locks its textures with, not
	// the level's. The engine's own world pass still runs under this device
	// and advances every texture it draws with that clock; given the level's
	// as well, a texture was advanced twice a frame between two different
	// times - a chain stepping at double speed, a paced one lurching - and on
	// the level's clock alone the animations stopped behind the pause menu,
	// where the other devices keep them going.
	if (Viewport && !Photo.Active)
	{
		const double time = EngineSeconds(Viewport->CurrentTime);
		for (size_t i = 0; i < SentTextures.size(); i++)
		{
			SentTexture& sent = SentTextures[i];
			if (!sent.Width || Scene.FixedFrames.count(sent.Source) || !TextureCache::Animates(sent.Source))
				continue;
			if (TextureCache::AnimatedPixels(sent.Source, sent.Masked, time, sent.Width, sent.Height, sent.LastFrame, Pixels))
			{
				Tracer->TexturePixels((uint32_t)i, (uint32_t)sent.Width, (uint32_t)sent.Height, Pixels.data());
				sent.Advances++;
			}
		}
	}
	Timings.Textures += NowMs() - texturesStart;

	Tracer->Instances(Scene.Instances, Scene.StaticGeometries);
	Tracer->Lights(Scene.Lights, Scene.FogLights);

	// Anything else too big for the channel - a level's world past about
	// half a million triangles - is dropped by it, so said here.
	if (Tracer->Refused() != RefusedSeen)
	{
		RefusedSeen = Tracer->Refused();
		if (RefusalsLogged < 8)
		{
			RefusalsLogged++;
			debugf(TEXT("PathTracer: %s, and was left out"), *Widen(Tracer->Error().c_str()));
		}
	}
	return Tracer->Alive();

	unguard;
}

void UPathTracerRenderDevice::Unlock(UBOOL Blit)
{
	guard(UPathTracerRenderDevice::Unlock);

	// The frame as the player sees it, whatever it was spent on. A pause of a
	// second or more - a level loading - is not a frame.
	{
		const double now = NowMs();
		if (LastUnlockMs > 0.0 && now - LastUnlockMs < 1000.0)
		{
			FrameIntervals.push_back((float)(now - LastUnlockMs));
			if (Bench.Measuring())
			{
				Bench.FrameSum += now - LastUnlockMs;
				Bench.Frames++;
			}
		}
		LastUnlockMs = now;
	}

	// Deliberately not conditional on HaveCamera. A frame that draws no world -
	// a menu, or a conversation - never calls SetSceneNode, and returning here
	// left an acquired swap chain image unpresented, which the compositor shows
	// as black. Whether the world can be traced and whether the frame must be
	// presented are different questions.
	if (!Blit || !OutputImage)
	{
		WaitForPreviousFrame();
		return;
	}

	const double frameStart = NowMs();

	// Set once the helper has traced a frame for this one. From then on this
	// frame owes it: its submission must wait on Ready and signal Released,
	// or the helper's next frame waits for ever.
	bool owed = false;

	// A frame the helper traced that cannot be presented after all is still
	// taken, with nothing done to it: its Ready waited on and its Released
	// signalled, so the next one can be traced at all.
	auto settle = [&]()
	{
		if (!owed || !Tracer)
			return;
		owed = false;
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
	};

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
		// The flashlight lights what it shines on differently once it has
		// moved or gone on or off, though the view may not have moved.
		bool flashlightChanged = false;
		for (int i = 0; i < 3; i++)
		{
			const vec4& a = Scene.Flashlight[i];
			const vec4& b = LastFlashlight[i];
			flashlightChanged = flashlightChanged || a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w;
			LastFlashlight[i] = a;
		}
		// Photo mode shows the denoised picture while the camera moves, and
		// once it has held still a few frames refines the picture sample by
		// sample instead, the world being held: nothing else starts it over,
		// and a light flickering is averaged as a long exposure would.
		if (Photo.Active)
		{
			Photo.StillFrames = cameraMoved ? 0 : Photo.StillFrames + 1;
			const bool accumulate = Photo.StillFrames >= PhotoState::SettleFrames;
			if (accumulate != Photo.Accumulating)
			{
				Photo.Accumulating = accumulate;
				AccumulatedFrames = 0;
				DenoiseRestart = true;
			}
		}
		if (cameraMoved || (!Photo.Accumulating && (sceneChanged || flashlightChanged)))
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

		// Traced first, and the last frame waited for only afterwards: the
		// helper records this one while the GPU is still on the last, and
		// queues it behind it, so the GPU goes from one straight to the next
		// rather than idling while the frame is described and recorded.
		// Without a world to trace the output image keeps the last one, which
		// is what the engine expects behind a menu or a conversation.
		if ((HaveCamera || !InsetViews.empty()) && Tracer && Tracer->Alive() && !Scene.IsEmpty())
		{
			// UT's weapon, now that RenderOverlays has placed it.
			Scene.FinishViewModel();
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
				// A photo being refined: the samples averaged as they come,
				// no denoiser, and paths given more bounces than a frame can
				// afford (Disable bit 524288 in the trace shader).
				if (Photo.Accumulating)
				{
					frame.MaxSamples = PhotoState::MaxSamples;
					frame.Bounces = (uint32_t)Clamp(Max(Bounces, PhotoState::PathBounces), 1, 255);
					frame.GlossBounces = (uint32_t)Clamp(Max(GlossBounces, PhotoState::GlossyBounces), 0, 255);
				}
				// DetailTextures is the engine's own switch, the one the
				// display settings set, and is honoured as it changes.
				frame.DisableBits = DisableBits | (DetailTextures ? 0u : 2048u) | (NeutralToneMapNow ? 8192u : 0u) | (Photo.Accumulating ? 524288u : 0u);
				frame.ViewMode = (uint32_t)ViewMode;
				frame.DebugMode = (uint32_t)DebugMode;
				frame.Denoise = (!DenoiseEnabled || Photo.Accumulating) ? TraceProtocol::DenoiseOff : (DlssEnabled ? TraceProtocol::DenoiseDlss : TraceProtocol::DenoiseNrd);
				// Photo mode's preview is NRD's rather than Ray Reconstruction's:
				// NRD traces at the full size, as the refined picture does, so
				// the helper goes from one to the other and back without
				// making its images again every time the camera stops.
				if (Photo.Active && !Photo.Accumulating && DenoiseEnabled)
					frame.Denoise = TraceProtocol::DenoiseNrd;
				frame.DlssQuality = (uint32_t)DlssQualityNow;
				frame.LightSize = (uint32_t)LightSizeNow;
				frame.MaxAnisotropy = (uint32_t)Clamp(appRound(MaxAnisotropy), 0, 16);
				frame.Lighting = EngineLightingNow ? 1 : 0;
				frame.Materials = MaterialsEnabled ? 1 : 0;
				frame.RestartDenoiser = DenoiseRestart ? 1 : 0;
				frame.Timing = (LogTimings || Bench.Step >= 0) ? 1 : 0;
				frame.Time = (Viewport && Viewport->Actor && Viewport->Actor->Level)
					? (float)fmod((double)Viewport->Actor->Level->TimeSeconds, 1000.0) : 0.0f;
				// Photo mode holds what pans and sways with the clock.
				if (Photo.Active)
					frame.Time = Photo.Time;
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
				// No flash over a photo: the player is not where it is taken.
				if (Photo.Active)
				{
					frame.Camera[0].w = 1.0f;
					frame.Camera[1].w = frame.Camera[2].w = frame.Camera[3].w = 0.0f;
				}
				frame.PreviousCamera[0] = previousCamera.Origin;
				frame.PreviousCamera[1] = previousCamera.Right;
				frame.PreviousCamera[2] = previousCamera.Up;
				frame.PreviousCamera[3] = previousCamera.Forward;
				frame.SkyOrigin = vec4(Scene.SkyOrigin.X, Scene.SkyOrigin.Y, Scene.SkyOrigin.Z, Scene.HasSky ? 1.0f : 0.0f);
				// As Render.dll sets up its skybox camera: the view's coordinate
				// system divided by the sky zone's rotation, which turns each
				// axis by it - roll, then pitch, then yaw.
				const FCoords sky = GMath.UnitCoords / Scene.SkyRotation;
				frame.SkyAxes[0] = vec4(sky.XAxis.X, sky.XAxis.Y, sky.XAxis.Z, 0.0f);
				frame.SkyAxes[1] = vec4(sky.YAxis.X, sky.YAxis.Y, sky.YAxis.Z, 0.0f);
				frame.SkyAxes[2] = vec4(sky.ZAxis.X, sky.ZAxis.Y, sky.ZAxis.Z, 0.0f);
				for (int i = 0; i < 3; i++)
					frame.Flashlight[i] = Scene.Flashlight[i];
				frame.GlowLighting = Max(GlowLighting, 0) / 100.0f;
				frame.Wetness = Clamp(Wetness, 0, 100) / 100.0f;
				frame.BumpMapping = Clamp(BumpMapping, 0, 1000) / 100.0f;
				frame.ToneCeiling = ToneCeiling();
				frame.PhotoLens = vec4(Photo.Aperture, Photo.Focus, 0.0f, 0.0f);
				// The windows' views, each averaging its samples while it and
				// the scene hold still, as the player's does.
				frame.InsetCount = (uint32_t)InsetViews.size();
				for (size_t i = 0; i < InsetViews.size(); i++)
				{
					const InsetView& view = InsetViews[i];
					InsetView& last = LastInsetViews[i];
					const bool moved = !sameXyz(view.Camera.Origin, last.Camera.Origin) || !sameXyz(view.Camera.Right, last.Camera.Right) ||
						!sameXyz(view.Camera.Up, last.Camera.Up) || !sameXyz(view.Camera.Forward, last.Camera.Forward) ||
						view.X != last.X || view.Y != last.Y || view.Width != last.Width || view.Height != last.Height;
					if (moved || sceneChanged)
						InsetAccumulated[i] = 0;
					last = view;
					TraceProtocol::TraceInset& inset = frame.Insets[i];
					inset.Camera[0] = view.Camera.Origin;
					inset.Camera[1] = view.Camera.Right;
					inset.Camera[2] = view.Camera.Up;
					inset.Camera[3] = view.Camera.Forward;
					inset.X = (uint32_t)view.X;
					inset.Y = (uint32_t)view.Y;
					inset.Width = (uint32_t)view.Width;
					inset.Height = (uint32_t)view.Height;
					inset.AccumulatedFrames = InsetAccumulated[i];
					if (InsetAccumulated[i] < (uint32_t)Max(MaxAccumulatedFrames, 1))
						InsetAccumulated[i]++;
				}
				owed = Tracer->Trace(frame);
				if (Tracer->Alive())
				{
					const TraceProtocol::Header& status = Tracer->Status();
					Timings.HelperWait += status.HelperWaitMs;
					Timings.HelperApply += status.HelperApplyMs;
					Timings.HelperRecord += status.HelperRecordMs;
					Timings.HelperStalls += (int)status.HelperStalls;
				}
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
						if (Bench.Measuring())
						{
							Bench.Gpu += status.GpuBuildMs + status.GpuTraceMs + status.GpuDenoiseMs + status.GpuCompositeMs;
							Bench.Trace += status.GpuTraceMs;
							Bench.GpuFrames++;
						}
					}
				}
			}
			Timings.Send += NowMs() - sendStart;

			if (!Tracer->Alive() && !TracerLost)
			{
				TracerLost = true;
				PathTracerEvent("helper lost: %s", Tracer->Error().c_str());
				if (Tracer->IsLocal())
					debugf(TEXT("PathTracer: tracing has stopped (%s). The world will not be traced again this session."),
						*Widen(Tracer->Error().c_str()));
				else
					debugf(TEXT("PathTracer: the helper has stopped (%s); PathTracerHelper.log says more. The world will not be traced again this session."),
						*Widen(Tracer->Error().c_str()));
			}
		}

		// The swap chain image is acquired only once the last frame is done
		// with its semaphore.
		WaitForPreviousFrame();

		if (SwapChain->Lost() || SwapChain->Width() != windowWidth || SwapChain->Height() != windowHeight || UsingVsync != UseVSync || UsingHdr != Hdr)
		{
			PathTracerEvent("swap chain %dx%d -> %dx%d%s", SwapChain->Width(), SwapChain->Height(), windowWidth, windowHeight,
				SwapChain->Lost() ? " (lost)" : "");
			UsingVsync = UseVSync;
			const bool hdrAsked = Hdr != 0, hdrChanged = UsingHdr != Hdr;
			UsingHdr = Hdr;
			SwapChain->Create(windowWidth, windowHeight, UseVSync ? 2 : 3, UseVSync, hdrAsked, false);

			// What it took: scRGB where the compositor offers it (Windows),
			// HDR10 where it does not (a Wayland compositor). The picture is
			// blitted to it from a float image, which the format has to allow;
			// one that does not is made again as SDR.
			const VkSurfaceFormatKHR format = SwapChain->Format();
			int mode = !hdrAsked ? 0 : format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ? 1 : format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ? 2 : 0;
			if (mode)
			{
				VkFormatProperties properties = {};
				vkGetPhysicalDeviceFormatProperties(Device->PhysicalDevice.Device, format.format, &properties);
				if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
				{
					debugf(TEXT("PathTracer HDR: the swap chain's format %d cannot be blitted to; staying SDR"), (int)format.format);
					mode = 0;
					SwapChain->Create(windowWidth, windowHeight, UseVSync ? 2 : 3, UseVSync, false, false);
				}
			}
			if (mode != HdrMode || (hdrChanged && hdrAsked))
			{
				if (mode == 1)
					debugf(TEXT("PathTracer HDR: scRGB, the SDR white at %d nits and the peak at %d"), (int)Clamp(HdrPaperWhite, 80, 1000), (int)Clamp(HdrPeakNits, 100, 10000));
				else if (mode == 2)
					debugf(TEXT("PathTracer HDR: HDR10 (Rec.2020, PQ), the SDR white at %d nits and the peak at %d"), (int)Clamp(HdrPaperWhite, 80, 1000), (int)Clamp(HdrPeakNits, 100, 10000));
				else if (hdrAsked)
				{
					// Said with what was on offer: a compositor that does not
					// do HDR looks just like a pairing this does not look for.
					const std::set<std::string>& enabled = Instance->EnabledExtensions;
					debugf(TEXT("PathTracer HDR: the surface offers neither scRGB nor HDR10; staying SDR (%s)"),
						enabled.count(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) ? TEXT("the colour space extension is on")
							: TEXT("without the colour space extension no HDR colour space can be offered"));
					for (const VkSurfaceFormatKHR& f : SwapChain->AvailableFormats())
						debugf(TEXT("  surface offers format %d, colour space %d"), (int)f.format, (int)f.colorSpace);
				}
				else
					debugf(TEXT("PathTracer HDR: off"));
				PathTracerEvent("HDR mode %d", mode);
			}
			HdrMode = mode;
		}

		const double acquireStart = NowMs();
		int imageIndex = SwapChain->AcquireImage(ImageAvailableSemaphore.get());
		Timings.Acquire += NowMs() - acquireStart;
		if (imageIndex == -1)
		{
			PathTracerEvent("no swap chain image%s", SwapChain->Lost() ? " (lost)" : "");
			settle();
			return;
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
			// Traced in this process, on this queue, it has no hands to change.
			const bool handOver = !Tracer->IsLocal();
			barriers[0].image = Tracer->Output();
			barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[0].srcQueueFamilyIndex = handOver ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
			barriers[0].dstQueueFamilyIndex = handOver ? family : VK_QUEUE_FAMILY_IGNORED;
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
			barriers[0].srcQueueFamilyIndex = handOver ? family : VK_QUEUE_FAMILY_IGNORED;
			barriers[0].dstQueueFamilyIndex = handOver ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
			barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[0].dstAccessMask = 0;
			barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 2, barriers);
		}

		// The windows' views, copied out of the picture for the tiles to draw
		// back at their place among them.
		if (!InsetViews.empty() && InsetSource)
		{
			VulkanImage* source = InsetSource->Image.get();
			PipelineBarrier()
				.AddImage(source, InsetSourceFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
				.Execute(commands.get(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			std::vector<VkImageCopy> regions;
			for (const InsetView& view : InsetViews)
			{
				VkImageCopy region = {};
				region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.dstSubresource = region.srcSubresource;
				region.srcOffset = { view.X, view.Y, 0 };
				region.dstOffset = region.srcOffset;
				region.extent = { (uint32_t)view.Width, (uint32_t)view.Height, 1 };
				regions.push_back(region);
			}
			vkCmdCopyImage(commands->buffer, OutputImage->image, VK_IMAGE_LAYOUT_GENERAL, source->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				(uint32_t)regions.size(), regions.data());
			PipelineBarrier()
				.AddImage(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
				.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			InsetSourceFresh = false;
		}

		// HUD, menus and console on top of the traced world, and the game's
		// Brightness over the lot - with the new pictures of the 2D textures
		// that draw themselves, now the last frame is done with them.
		if (Textures)
			Textures->RecordChanges(commands.get());
		RenderTiles(commands.get());
		ApplyBrightness(commands.get());

		// In HDR, the picture encoded for the swap chain, and a photo being
		// taken brought back to SDR for its PNG.
		if (HdrMode)
		{
			EncodeFrame(commands.get(), false);
			if (Photo.SavePending)
				EncodeFrame(commands.get(), true);
		}

		// The copy and the tiles write the output image; the blit reads it.
		PipelineBarrier()
			.AddImage(OutputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		// The photo asked for, copied out as it is about to be shown.
		if (Photo.SavePending)
			RecordPhotoSave(commands.get(), HdrMode && SdrImage ? SdrImage.get() : OutputImage.get());

		// Letterbox: keep the traced image's aspect inside the window rather
		// than stretching it, the same as the other devices here.
		float scale = std::min(windowWidth / (float)TraceWidth, windowHeight / (float)TraceHeight);
		int dstWidth = (int)std::round(TraceWidth * scale);
		int dstHeight = (int)std::round(TraceHeight * scale);
		int dstX = (windowWidth - dstWidth) / 2;
		int dstY = (windowHeight - dstHeight) / 2;

		// The bars are the swap chain image's own, taken in an undefined
		// layout, and in practice they hold whatever was last presented from
		// it - the edges of a wider mode, after the player picks a narrower one.
		if (dstWidth != windowWidth || dstHeight != windowHeight)
		{
			VkClearColorValue black = {};
			black.float32[3] = 1.0f;
			VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			commands->clearColorImage(SwapChain->GetImage(imageIndex)->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
			PipelineBarrier()
				.AddImage(SwapChain->GetImage(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
				.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		}

		VkImageBlit blit = {};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1] = { TraceWidth, TraceHeight, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[0] = { dstX, dstY, 0 };
		blit.dstOffsets[1] = { dstX + dstWidth, dstY + dstHeight, 1 };

		commands->blitImage(
			(HdrMode && PresentImage ? PresentImage : OutputImage)->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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

		const double presentStart = NowMs();
		SwapChain->QueuePresent(imageIndex, RenderFinishedSemaphore.get());
		Timings.Present += NowMs() - presentStart;

		// A photo copied out in this frame, written once it is done.
		if (Photo.SaveRecorded)
		{
			WaitForPreviousFrame();
			WritePhoto();
		}

		// Paced after the present rather than before the next frame's work,
		// so the game's own tick is what waits.
		// Not while benchmarking, whose frame times would be the limit's.
		// Nor while a photo refines, whose samples come a frame at a time.
		const double limitStart = NowMs();
		if (Bench.Step < 0 && !Photo.Accumulating)
			LimitFrameRate();
		Timings.Limit += NowMs() - limitStart;
		if (HaveCamera)
			AdvanceBench();

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
				snprintf(line, sizeof(line), "PathTracer ms/frame: collect %.2f send %.2f (textures %.2f; helper wait %.2f apply %.2f record %.2f, %d stalls) gpu-wait %.2f acquire %.2f present %.2f unlock %.2f limiter %.2f | %d instances, %d textures, %d poses rebuilt",
					Timings.Collect / n, Timings.Send / n, Timings.Textures / n,
					Timings.HelperWait / n, Timings.HelperApply / n, Timings.HelperRecord / n, Timings.HelperStalls,
					Timings.Wait / n, Timings.Acquire / n, Timings.Present / n, (Timings.Total - Timings.Limit) / n, Timings.Limit / n,
					(int)Scene.Instances.size(), (int)Scene.Textures.size(), Scene.MeshBuilds);
				WriteTimingLine(line);
				const double* c = Scene.CollectStageMs;
				snprintf(line, sizeof(line), "PathTracer collect ms/frame: lights %.2f animated %.2f (the engine posing them %.2f) other actors %.2f held weapons %.2f particles %.2f fittings %.2f decals %.2f view model %.2f",
					c[0] / n, c[1] / n, c[8] / n, c[2] / n, c[3] / n, c[4] / n, c[5] / n, c[6] / n, c[7] / n);
				WriteTimingLine(line);
				if (!FrameIntervals.empty())
				{
					// The 1% low is the frame rate the slowest one frame in a
					// hundred ran at: the 99th percentile frame.
					std::vector<float> sorted = FrameIntervals;
					std::sort(sorted.begin(), sorted.end());
					double sum = 0.0;
					for (float f : sorted)
						sum += f;
					const double average = sum / sorted.size();
					const float slow = sorted[std::min(sorted.size() - 1, (size_t)(sorted.size() * 0.99))];
					snprintf(line, sizeof(line), "PathTracer frames: %.2f ms average, %.1f fps, 1%% low %.1f fps (%.2f ms), over %d frames",
						average, 1000.0 / average, 1000.0 / slow, slow, (int)sorted.size());
					WriteTimingLine(line);
				}
				if (Timings.GpuFrames > 0 && Tracer)
				{
					const double g = Timings.GpuFrames;
					const TraceProtocol::Header& status = Tracer->Status();
					static const char* denoisers[] = { "off", "NRD", "DLSS-RR" };
					snprintf(line, sizeof(line), "PathTracer GPU ms/frame (helper): build %.2f trace %.2f denoise %.2f composite %.2f | total %.2f at %dx%d traced at %ux%u, bounces %d, glossy bounces %d, materials %s, denoiser %s",
						Timings.GpuBuild / g, Timings.GpuTrace / g, Timings.GpuDenoise / g, Timings.GpuComposite / g,
						(Timings.GpuBuild + Timings.GpuTrace + Timings.GpuDenoise + Timings.GpuComposite) / g,
						TraceWidth, TraceHeight, status.RenderWidth, status.RenderHeight, (int)Bounces, (int)GlossBounces,
						MaterialsEnabled ? "on" : "off", denoisers[Min<uint32_t>(status.DenoisedWith, 2)]);
					WriteTimingLine(line);
				}
			}
			const int logged = Timings.Logged;
			Timings = FrameTimings();
			for (double& ms : Scene.CollectStageMs)
				ms = 0.0;
			FrameIntervals.clear();
			Timings.Logged = logged;
		}

		if (AccumulatedFrames < (Photo.Accumulating ? PhotoState::MaxSamples : (uint32_t)Max(MaxAccumulatedFrames, 1)))
			AccumulatedFrames++;
	}
	catch (const std::exception& e)
	{
		PathTracerEvent("frame failed: %s", e.what());
		debugf(TEXT("PathTracer frame failed: %s"), *Widen(e.what()));
		settle();
	}

	unguard;
}

// What PT DENOISE and PT DLSS have asked for, as the next frame will be
// denoised: NRD, Ray Reconstruction at a quality, or nothing - and NRD in Ray
// Reconstruction's place, with the helper's reason, where it cannot run.
FString UPathTracerRenderDevice::DescribeDenoiser() const
{
	static const TCHAR* qualityNames[] = { TEXT("DLAA"), TEXT("quality"), TEXT("balanced"), TEXT("performance"), TEXT("ultra performance") };
	const bool helper = Tracer && Tracer->Alive();
	if (!DenoiseEnabled)
		return TEXT("denoising off");
	if (!DlssEnabled)
		return FString::Printf(TEXT("denoising with NRD (%s)"), helper ? *Widen(Tracer->Status().DenoiserStatus) : TEXT("no helper"));
	const FString quality = qualityNames[Clamp(DlssQualityNow, 0, 4)];
	if (!helper)
		return FString::Printf(TEXT("denoising with DLSS Ray Reconstruction, %s (no helper)"), *quality);
	const char* status = Tracer->Status().DlssStatus;
	// Started the first time a frame asks for it, so on the frame after this
	// one; until then there is nothing to report either way.
	if (!strcmp(status, "ready") || !strcmp(status, "not asked for yet"))
		return FString::Printf(TEXT("denoising with DLSS Ray Reconstruction, %s"), *quality);
	return FString::Printf(TEXT("DLSS Ray Reconstruction asked for, %s, but NRD stands in: %s"), *quality, *Widen(status));
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
			// PT LOOK TIME: the engine's own lightmap at the spot as well,
			// every frame for four seconds, with the level's clock.
			const bool series = ParseCommand(&Cmd, TEXT("TIME"));
			APlayerPawn* player = Viewport ? Viewport->Actor : nullptr;
			if (!player || !player->XLevel)
				return 1;
			const FVector start = player->Location + FVector(0.0f, 0.0f, player->EyeHeight);
			const FVector end = start + player->ViewRotation.Vector() * 8000.0f;
			FCheckResult hit;
			player->XLevel->SingleLineCheck(hit, player, end, start, TRACE_AllColliding);
			if (hit.Actor && hit.Actor != player->Level)
			{
				DescribeActor(hit.Actor, hit.Actor->Mesh);
				DescribeLightingOf(hit.Actor);
			}
			// The level's own surface: its texture and what it counts as being
			// made of, which is the name to use for an override in the ini.
			UModel* model = player->XLevel->Model;
			if (hit.Actor == player->Level && model && hit.Item >= 0 && hit.Item < model->Nodes.Num())
			{
				const FBspNode& node = model->Nodes(hit.Item);
				UTexture* texture = node.iSurf < model->Surfs.Num() ? model->Surfs(node.iSurf).Texture : nullptr;
				const TCHAR* kind = nullptr;
				const vec4 m = Materials::For(texture, nullptr, &kind);
				Ar.Logf(TEXT("PT: surface %s (group %s): %s, roughness %.2f metalness %.2f reflectance %.2f relief %.2f, detail texture %s"),
					texture ? texture->GetName() : TEXT("none"),
					(texture && texture->GetOuter()) ? texture->GetOuter()->GetName() : TEXT("none"),
					kind, m.x, m.y, Materials::Reflectance(m), Materials::Relief(m),
					(texture && texture->DetailTexture) ? texture->DetailTexture->GetName() : TEXT("none"));
				// Which file its package came from - the first of Paths to
				// have it, as the engine looks - and whether it carries an
				// S3TC set, New Vision's.
				if (texture)
				{
					TCHAR file[256] = TEXT("");
					UObject* package = texture->GetOuter();
					while (package && package->GetOuter())
						package = package->GetOuter();
					if (package)
						appFindPackageFile(package->GetName(), NULL, file);
					// How it animates, as the engine paces it, and how often
					// the trace has actually taken a new frame of it since the
					// last look.
					if (texture->AnimNext || texture->bRealtime || texture->bParametric)
					{
						int chain = 1;
						for (UTexture* t = texture->AnimNext; t && t != texture && chain < 1000; t = t->AnimNext)
							chain++;
						const double now = EngineSeconds(appSeconds());
						for (SentTexture& sent : SentTextures)
							if (sent.Source == texture)
								debugf(TEXT("PT: animation: %d frames, MaxFrameRate %.2f MinFrameRate %.2f, realtime %d; the trace took %d new frames in the last %.2f seconds"),
									chain, texture->MaxFrameRate, texture->MinFrameRate, (int)texture->bRealtime, (int)sent.Advances, now - AdvancesSince);
						for (SentTexture& sent : SentTextures)
							sent.Advances = 0;
						AdvancesSince = now;
					}
					Ar.Logf(TEXT("PT: texture %dx%d from %s%s"), (int)texture->USize, (int)texture->VSize, file[0] ? file : TEXT("?"),
						texture->bHasComp && texture->CompMips.Num() > 0
							? *FString::Printf(TEXT(", S3TC %dx%d%s"), (int)texture->CompMips(0).USize, (int)texture->CompMips(0).VSize, UseS3TC ? TEXT("") : TEXT(" (UseS3TC off)"))
							: TEXT(", no S3TC"));
				}
				// Its zone's ambient: FGetHSV at the zone's brightness, of
				// which a lightmap starts at half.
				AZoneInfo* zone = node.iZone[1] < FBspNode::MAX_ZONES ? model->Zones[node.iZone[1]].ZoneActor : nullptr;
				if (zone && zone->AmbientBrightness)
				{
					const FPlane a = FGetHSV(zone->AmbientHue, zone->AmbientSaturation, zone->AmbientBrightness);
					Ar.Logf(TEXT("PT: zone %s ambient brightness %d hue %d saturation %d: %.3f %.3f %.3f on a mesh, half that on a lightmap"),
						zone->GetName(), (int)zone->AmbientBrightness, (int)zone->AmbientHue, (int)zone->AmbientSaturation, a.X, a.Y, a.Z);
				}
				else
					Ar.Logf(TEXT("PT: zone %s has no ambient"), zone ? zone->GetName() : TEXT("none"));
				DescribeLightingAt(player->XLevel, hit.Location, hit.Normal, texture,
					node.iSurf < model->Surfs.Num() && (model->Surfs(node.iSurf).PolyFlags & PF_SpecialLit) != 0, node.iSurf, Ar);
				LightmapProbeSurf = node.iSurf;
				LightmapProbePoint = hit.Location;
				LightmapProbeUntil = FrameIndex + 30;
				if (series)
				{
					LightmapSeriesSurf = node.iSurf;
					LightmapSeriesPoint = hit.Location;
					LightmapSeriesFrames = 480;
				}
				// The lights the level's build baked into this surface's
				// lightmap, and how much of each one's shadow mask it left lit:
				// what the engine's own shadows are made of.
				FLightMapIndex* index = node.iSurf < model->Surfs.Num() ? model->GetLightMapIndex(node.iSurf) : nullptr;
				if (index)
				{
					const INT rowBytes = (index->UClamp + 7) >> 3;
					const INT perLight = rowBytes * index->VClamp;
					debugf(TEXT("PT: lightmap %dx%d texels of %.0f units, lights baked in:"), index->UClamp, index->VClamp, index->UScale);
					for (INT k = 0; index->iLightActors >= 0 && index->iLightActors + k < model->Lights.Num(); k++)
					{
						AActor* light = model->Lights(index->iLightActors + k);
						if (!light)
							break;
						INT lit = 0, total = 0;
						const INT start = index->DataOffset + k * perLight;
						for (INT b = 0; b < perLight && start + b < model->LightBits.Num(); b++)
						{
							const BYTE bits = model->LightBits(start + b);
							for (INT bit = 0; bit < 8; bit++)
								if ((b % rowBytes) * 8 + bit < index->UClamp)
								{
									total++;
									lit += (bits >> bit) & 1;
								}
						}
						debugf(TEXT("  %s: %d of %d texels lit"), light->GetName(), lit, total);
					}
				}
				else
					debugf(TEXT("PT: this surface has no lightmap"));
			}
			Ar.Logf(TEXT("PT: %s written to the log"), (hit.Actor && hit.Actor != player->Level) ? hit.Actor->GetName() : TEXT("nothing but the level"));
			return 1;
		}

		// The lights that glow in fog, nearest first: where each is, how big
		// its glow is, whether the eye can see it, and how much of its glow
		// it can see itself - the level's own line checks out from it in 26
		// directions to the edge of the glow - which is what the fog's
		// shadows hold it to. A light buried in a wall or a ceiling sees
		// none, and with FogShadows its glow is gone.
		if (ParseCommand(&Cmd, TEXT("FOG")))
		{
			APlayerPawn* player = Viewport ? Viewport->Actor : nullptr;
			ULevel* level = player ? player->XLevel : nullptr;
			if (!level)
				return 1;
			auto say = [&](const FString& line)
			{
				Ar.Logf(TEXT("%s"), *line);
				debugf(TEXT("%s"), *line);
			};
			const FCoords view = GMath.UnitCoords / player->ViewRotation;
			const FVector eye = player->Location + FVector(0.0f, 0.0f, player->EyeHeight);
			std::vector<std::pair<float, AActor*>> found;
			for (INT i = 0; i < level->Actors.Num(); i++)
			{
				AActor* a = level->Actors(i);
				if (a && a->LightType != LT_None && a->VolumeRadius && a->VolumeBrightness)
					found.push_back({ (a->Location - eye).Size(), a });
			}
			std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			say(FString::Printf(TEXT("PT FOG: %d lights with a glow, nearest first; fog shadows %s"), (int)found.size(),
				(DisableBits & 65536u) ? TEXT("off") : TEXT("on")));
			// The engine's own conditions: the player in a fog zone, or no
			// fog anywhere, and fog only over what is in one.
			AZoneInfo* here = player->Region.Zone;
			say(FString::Printf(TEXT("  you are in %s%s"), here ? here->GetName() : TEXT("no zone"),
				(here && here->bFogZone) ? TEXT(", a fog zone") : TEXT(", not a fog zone: the engine draws no volumetric fog anywhere from here")));
			{
				FCheckResult look;
				const FVector ahead = player->ViewRotation.Vector();
				level->SingleLineCheck(look, player, eye + ahead * 8000.0f, eye, TRACE_VisBlocking);
				if (look.Actor && level->Model)
				{
					AZoneInfo* there = level->Model->PointRegion(level->GetLevelInfo(), look.Location - ahead * 4.0f).Zone;
					say(FString::Printf(TEXT("  under the crosshair, %.0f units off: %s%s"), (look.Location - eye).Size(),
						there ? there->GetName() : TEXT("no zone"), (there && there->bFogZone) ? TEXT(", a fog zone") : TEXT(", not a fog zone: no fog over it")));
				}
			}
			for (size_t n = 0; n < found.size() && n < 8; n++)
			{
				AActor* a = found[n].second;
				const float radius = (a->VolumeRadius + 1) * 25.0f;
				const FVector d = a->Location - eye;
				FCheckResult hit;
				level->SingleLineCheck(hit, player, a->Location, eye, TRACE_VisBlocking);
				const bool seen = hit.Actor == nullptr;
				int open = 0;
				float reached = 0.0f, nearest = 1.0f;
				for (int x = -1; x <= 1; x++)
					for (int y = -1; y <= 1; y++)
						for (int z = -1; z <= 1; z++)
						{
							if (!x && !y && !z)
								continue;
							const FVector dir = FVector((FLOAT)x, (FLOAT)y, (FLOAT)z).SafeNormal();
							FCheckResult out;
							level->SingleLineCheck(out, nullptr, a->Location + dir * radius, a->Location, TRACE_VisBlocking);
							const float t = out.Actor ? out.Time : 1.0f;
							open += t > 0.9f ? 1 : 0;
							reached += t;
							nearest = Min(nearest, t);
						}
				AZoneInfo* zone = a->Region.Zone;
				say(FString::Printf(TEXT("  %s: %.0f %s, %.0f %s, %.0f %s; glow %.0f across (VolumeRadius %d, brightness %d, fog %d), light %d, type %d, effect %d; zone %s%s; %s from here; sees out %d of 26 ways, on average %.0f%% of the way, the nearest wall %.0f units off"),
					a->GetName(), Abs(d | view.XAxis), (d | view.XAxis) >= 0 ? TEXT("ahead") : TEXT("behind"),
					Abs(d | view.YAxis), (d | view.YAxis) >= 0 ? TEXT("right") : TEXT("left"),
					Abs(d.Z), d.Z >= 0 ? TEXT("up") : TEXT("down"),
					radius * 2.0f, (int)a->VolumeRadius, (int)a->VolumeBrightness, (int)a->VolumeFog, (int)a->LightBrightness,
					(int)a->LightType, (int)a->LightEffect,
					zone ? zone->GetName() : TEXT("none"), (zone && zone->bFogZone) ? TEXT(" (fog)") : TEXT(" (not fog, so no glow)"),
					seen ? TEXT("seen") : TEXT("hidden"), open, reached / 26.0f * 100.0f, nearest * radius));
			}
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
			{ TEXT("NOLIGHTS"), 1u }, { TEXT("NOSHADOWS"), 2u }, { TEXT("NOSKY"), 4u }, { TEXT("OPAQUE"), 8u }, { TEXT("HIGHLIGHT"), 16u }, { TEXT("NOFOG"), 32u }, { TEXT("GUIDES"), 64u }, { TEXT("NOGLOW"), 512u },
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
		if (ParseCommand(&Cmd, TEXT("MATERIALS")) || ParseCommand(&Cmd, TEXT("NOMATERIALS")))
		{
			// The denoiser follows at the next frame, once nothing is
			// using the one it replaces.
			MaterialsEnabled = !MaterialsEnabled;
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("HDR")))
		{
			Hdr = !Hdr;
			Ar.Logf(TEXT("PT: HDR %s"), Hdr
				? TEXT("asked for: the log says what the display took (HDRPEAK n, HDRWHITE n set its levels)")
				: TEXT("off"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("HDRPEAK")))
		{
			HdrPeakNits = Clamp(appAtoi(Cmd), 100, 10000);
			Ar.Logf(TEXT("PT: HDR peak %d nits%s"), (int)HdrPeakNits, HdrMode ? TEXT("") : TEXT(" (HDR is not on)"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("HDRWHITE")))
		{
			HdrPaperWhite = Clamp(appAtoi(Cmd), 80, 1000);
			Ar.Logf(TEXT("PT: HDR's SDR white at %d nits%s"), (int)HdrPaperWhite, HdrMode ? TEXT("") : TEXT(" (HDR is not on)"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("ALLLIGHTS")))
		{
			DisableBits ^= 2097152u;
			AccumulatedFrames = 0;
			Ar.Logf(TEXT("PT: %s"), (DisableBits & 2097152u)
				? TEXT("every light in a point's cell weighed, however many")
				: TEXT("a crowded cell's heaviest lights weighed, and a few drawn from the rest to stand for them"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("MESHLIGHT")))
		{
			DisableBits ^= 1024u;
			AccumulatedFrames = 0;
			Ar.Logf(TEXT("PT: meshes lit %s"), (DisableBits & 1024u)
				? TEXT("as flat surfaces are, by N.L")
				: TEXT("as the engine lights them, brighter and flatter, with its rim"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("ANISOTROPY")))
		{
			MaxAnisotropy = (FLOAT)Clamp(appAtoi(Cmd), 0, 16);
			Ar.Logf(TEXT("PT: texture filter %s"), MaxAnisotropy > 1.0f ? *FString::Printf(TEXT("%dx anisotropic"), appRound(MaxAnisotropy)) : TEXT("trilinear"));
			handled = true;
		}
		// The engine's own shadow masks on its baked lights, with the
		// engine's lighting (Disable bit 16384 leaves them out).
		// Every 2D draw of the next frame, to the log: what the HUD and the
		// menus hand this device, to find what it leaves out.
		if (ParseCommand(&Cmd, TEXT("BENCH")))
		{
			if (Bench.Step >= 0)
			{
				ApplyBenchStep(0);
				Bench.Step = -1;
				UseVSync = Bench.Vsync ? 1 : 0;
				Ar.Logf(TEXT("PT BENCH: stopped, everything as it was"));
			}
			else
			{
				StartBench();
				Ar.Logf(TEXT("PT BENCH: hold still for about half a minute; the frame limit and VSync are off meanwhile. PT BENCH again stops it."));
			}
			return 1;
		}
		if (ParseCommand(&Cmd, TEXT("TILES")))
		{
			LogDrawsArmed = true;
			Ar.Logf(TEXT("PT: the next frame's 2D draws go to the log"));
			return 1;
		}
		if (ParseCommand(&Cmd, TEXT("BAKEDSHADOWS")))
		{
			DisableBits ^= 16384u;
			AccumulatedFrames = 0;
			Ar.Logf(TEXT("PT: the engine's baked shadows %s"), (DisableBits & 16384u)
				? TEXT("off, baked lights shadowed by the trace alone")
				: TEXT("on, each baked light held to the lightmap's shadow mask as well as traced"));
			handled = true;
		}
		// Glass's colour on the light through it, and wet streets.
		if (ParseCommand(&Cmd, TEXT("GLASS")))
		{
			DisableBits ^= 1048576u;
			AccumulatedFrames = 0;
			Ar.Logf(TEXT("PT: light through glass %s"), (DisableBits & 1048576u)
				? TEXT("as it is, as the engine's lightmaps pass it")
				: TEXT("tinted by the glass"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("BUMP")))
		{
			BumpMapping = Clamp(appAtoi(Cmd), 0, 1000);
			AccumulatedFrames = 0;
			if (BumpMapping > 0)
				Ar.Logf(TEXT("PT: bump mapping at %d%% of the materials' relief"), (int)BumpMapping);
			else
				Ar.Logf(TEXT("PT: bump mapping off, every surface flat"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("WET")))
		{
			Wetness = Clamp(appAtoi(Cmd), 0, 100);
			AccumulatedFrames = 0;
			if (Wetness > 0)
				Ar.Logf(TEXT("PT: the streets %d%% wet"), (int)Wetness);
			else
				Ar.Logf(TEXT("PT: the streets dry"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("FOGSHADOWS")))
		{
			DisableBits ^= 65536u;
			Ar.Logf(TEXT("PT: fog %s"), (DisableBits & 65536u)
				? TEXT("glowing through everything, as the engine draws it")
				: TEXT("held to its lights' shadows"));
			handled = true;
		}
		// Photo mode: see PhotoMode.cpp.
		if (ParseCommand(&Cmd, TEXT("PHOTO")))
		{
			PhotoCommand(Cmd, Ar);
			return 1;
		}
		// The flashlight on or off, or with a number its brightness, in
		// percent.
		if (ParseCommand(&Cmd, TEXT("FLASHLIGHT")))
		{
			if (appIsDigit(*Cmd))
			{
				FlashlightBrightness = Max(appAtoi(Cmd), 0);
				DisableBits &= ~131072u;
			}
			else
				DisableBits ^= 131072u;
			if (DisableBits & 131072u)
				Ar.Logf(TEXT("PT: the light augmentation as the game lights it"));
			else
				Ar.Logf(TEXT("PT: the light augmentation as a flashlight, brightness %d%%, beam %d%%  (PT FLASHLIGHT n, PT BEAM n)"),
					(int)FlashlightBrightness, (int)FlashlightHaze);
			handled = true;
		}
		// How much glowing surfaces light, in percent; and whether they are
		// sampled as lights or found only by the bounces that reach them,
		// to compare (Disable bit 262144).
		if (ParseCommand(&Cmd, TEXT("GLOW")))
		{
			GlowLighting = Max(appAtoi(Cmd), 0);
			Ar.Logf(TEXT("PT: glowing surfaces light at %d%%"), (int)GlowLighting);
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("GLOWSAMPLING")))
		{
			DisableBits ^= 262144u;
			Ar.Logf(TEXT("PT: glowing surfaces %s"), (DisableBits & 262144u)
				? TEXT("found only by the bounces that reach them")
				: TEXT("sampled as lights as well"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("BEAM")))
		{
			FlashlightHaze = Max(appAtoi(Cmd), 0);
			Ar.Logf(TEXT("PT: the flashlight's beam in the air at %d%%"), (int)FlashlightHaze);
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("MIPS")))
		{
			DisableBits ^= 4096u;
			AccumulatedFrames = 0;
			Ar.Logf(TEXT("PT: mipmaps %s"), (DisableBits & 4096u) ? TEXT("off, every texture at its top level") : TEXT("on"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("DETAIL")))
		{
			DetailTextures = !DetailTextures;
			Ar.Logf(TEXT("PT: detail textures %s"), DetailTextures ? TEXT("on") : TEXT("off"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("WIDESCREEN")))
		{
			WidescreenFovEnabled = !WidescreenFovEnabled;
			Ar.Logf(TEXT("PT: widescreen field of view %s"), WidescreenFovEnabled ? TEXT("on (Hor+)") : TEXT("off (the engine's own, cropped top and bottom)"));
			handled = true;
		}
		// The exposure for the session, as the ini's Exposure: 0 to 255 for
		// 0.2 to 2.2.
		if (ParseCommand(&Cmd, TEXT("EXPOSURE")))
		{
			while (*Cmd == ' ')
				Cmd++;
			if (*Cmd)
				Exposure = (BYTE)Clamp(appAtoi(Cmd), 0, 255);
			Ar.Logf(TEXT("PT: exposure %d, %.2f  (PT EXPOSURE 0-255; 102 is 1.0)"), (int)Exposure, 0.2f + Exposure * (2.0f / 255.0f));
			handled = true;
		}
		// Reinhard's tone curve or the neutral one; see the shaders' toneMap.
		if (ParseCommand(&Cmd, TEXT("TONEMAP")))
		{
			NeutralToneMapNow = !NeutralToneMapNow;
			Ar.Logf(TEXT("PT: %s tone curve"), NeutralToneMapNow ? TEXT("the neutral") : TEXT("Reinhard's"));
			handled = true;
		}
		// The engine's lighting or the linear; with no word, the other one.
		if (ParseCommand(&Cmd, TEXT("LIGHTING")))
		{
			if (ParseCommand(&Cmd, TEXT("ENGINE")))
				EngineLightingNow = true;
			else if (ParseCommand(&Cmd, TEXT("LINEAR")))
				EngineLightingNow = false;
			else
				EngineLightingNow = !EngineLightingNow;
			AccumulatedFrames = 0;
			DenoiseRestart = true;
			Ar.Logf(TEXT("PT: %s  (PT LIGHTING [ENGINE | LINEAR])"), EngineLightingNow
				? TEXT("the engine's lighting: its lightmaps' falloff and sum, with traced shadows")
				: TEXT("linear lighting: each light straight down to nothing at its radius"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("LIGHTSIZE")))
		{
			LightSizeNow = Clamp(appAtoi(Cmd), 0, 255);
			AccumulatedFrames = 0;
			DenoiseRestart = true;
			Ar.Logf(TEXT("PT: shadows cast from %s"), LightSizeNow > 0 ? *FString::Printf(TEXT("a light %d units across"), LightSizeNow * 2) : TEXT("a point, hard to their ends"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("PINNEDUI")))
		{
			while (*Cmd == ' ')
				Cmd++;
			if (*Cmd)
			{
				PinnedAspect = ParsePinnedAspect(Cmd);
				// Only a new mode gives the engine the new shape, so set the
				// one the player has again.
				UClient* client = Viewport ? Viewport->GetOuterUClient() : nullptr;
				if (client && FullscreenState.Enabled)
					SetRes(client->FullscreenViewportX, client->FullscreenViewportY, Max(client->FullscreenColorBits / 8, 2), 1);
			}
			if (PinnedAspect > 0.0f)
				Ar.Logf(TEXT("PT: UI pinned to %.2f:1 in fullscreen, the world filling the screen around it"), PinnedAspect);
			else
				Ar.Logf(TEXT("PT: UI across the whole screen"));
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("GLOSSBOUNCES")))
		{
			GlossBounces = Max(appAtoi(Cmd), 0);
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("RESET")))
		{
			DisableBits = ConfiguredBits();
			ViewMode = 0;
			handled = true;
		}
		// DLSS Ray Reconstruction in place of NRD, or back; with a quality,
		// that quality and DLSS on.
		if (ParseCommand(&Cmd, TEXT("DLSS")))
		{
			static const struct { const TCHAR* Name; int Quality; } qualities[] = {
				{ TEXT("DLAA"), 0 }, { TEXT("QUALITY"), 1 }, { TEXT("BALANCED"), 2 }, { TEXT("PERFORMANCE"), 3 }, { TEXT("ULTRAPERFORMANCE"), 4 },
			};
			bool named = false;
			for (const auto& q : qualities)
				if (ParseCommand(&Cmd, q.Name))
				{
					DlssQualityNow = q.Quality;
					DlssEnabled = true;
					named = true;
				}
			if (!named)
				DlssEnabled = !DlssEnabled;
			// Asking for DLSS is asking for denoising: PT DENOISE is the
			// switch for both, and left off it would make this do nothing.
			if (DlssEnabled)
				DenoiseEnabled = true;
			DenoiseRestart = true;
			Ar.Logf(TEXT("PT: %s  (PT DLSS [DLAA | QUALITY | BALANCED | PERFORMANCE | ULTRAPERFORMANCE])"), *DescribeDenoiser());
			handled = true;
		}
		if (ParseCommand(&Cmd, TEXT("DENOISE")))
		{
			DenoiseEnabled = !DenoiseEnabled;
			DenoiseRestart = true;
			Ar.Logf(TEXT("PT: %s"), *DescribeDenoiser());
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
			MaterialsEnabled ? TEXT("on") : TEXT("off"),
			(int)Bounces, (int)GlossBounces, handled ? TEXT("") : TEXT("  (PT BENCH | LIGHTS | FOG | WEAPON | LOOK | HIGHLIGHT | NOLIGHTS | ALLLIGHTS | HDR | HDRPEAK n | HDRWHITE n | NOSHADOWS | NOSKY | NOFOG | FOGSHADOWS | FLASHLIGHT [n] | BEAM n | GLOW n | GLOWSAMPLING | GLASS | WET n | BUMP n | PHOTO | NOGLOW | MATERIALS | MESHLIGHT | DETAIL | MIPS | BAKEDSHADOWS | ANISOTROPY n | WIDESCREEN | PINNEDUI 16:9|4:3|OFF | LIGHTSIZE n | OPAQUE | DENOISE | DLSS [quality] | VIEW name | GUIDES | BOUNCES n | GLOSSBOUNCES n | RESET)"));
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

	// The key bindings above all: the game can write them out as it closes.
	EndPhoto(TEXT("the device is closing"));
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

	PresentView.reset();
	PresentImage.reset();
	SdrView.reset();
	SdrImage.reset();
	OutputView.reset();
	OutputImage.reset();
	InsetSource.reset();

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
	BrightnessPipeline.reset();
	BrightnessPipelineLayout.reset();
	BrightnessSet.reset();
	BrightnessDescriptorPool.reset();
	BrightnessSetLayout.reset();
	BrightnessShader.reset();
	EncodePipeline.reset();
	EncodePipelineLayout.reset();
	PresentSet.reset();
	SdrSet.reset();
	EncodeDescriptorPool.reset();
	EncodeSetLayout.reset();
	EncodeShader.reset();
	// Released here with everything else. Left to the member destructors
	// they outlive the device and destroy themselves against a dead handle,
	// which crashed on exit - and when the engine replaces the device on
	// restoring from alt-tab.
	for (auto& sampler : TileSamplers) sampler.reset();

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

// The engine's surfaces are traced from the level itself, so nothing is drawn
// here. What arrives is still worth one look: the lightmap the engine built
// for the surface, which is the ground truth PT LOOK compares the trace with.
void UPathTracerRenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet)
{
	// PT LOOK TIME: the texel at the spot, once a frame, with the level's
	// clock and the real one, and whether the engine rebuilt the lightmap -
	// how fast the engine's own lighting effects really run.
	if (LightmapSeriesFrames > 0 && FrameIndex != LightmapSeriesLastFrame && Surface.LightMap && Surface.Level && Facet.Polys)
	{
		UModel* m = Surface.Level->Model;
		bool mine = false;
		for (FSavedPoly* p = Facet.Polys; p && !mine; p = p->Next)
			mine = p->iNode >= 0 && p->iNode < m->Nodes.Num() && m->Nodes(p->iNode).iSurf == LightmapSeriesSurf;
		FLightMapIndex* index = mine ? m->GetLightMapIndex(LightmapSeriesSurf) : nullptr;
		FTextureInfo* lm = Surface.LightMap;
		if (index && lm->Mips[0] && lm->Mips[0]->DataPtr && lm->UClamp > 0 && lm->VClamp > 0)
		{
			const FBspSurf& surf = m->Surfs(LightmapSeriesSurf);
			const FVector d = LightmapSeriesPoint - m->Points(surf.pBase);
			const float u = ((d | m->Vectors(surf.vTextureU)) - index->Pan.X) / index->UScale;
			const float v = ((d | m->Vectors(surf.vTextureV)) - index->Pan.Y) / index->VScale;
			const INT x = Clamp(appRound(u), 0, lm->UClamp - 1), y = Clamp(appRound(v), 0, lm->VClamp - 1);
			const FColor& t = ((const FColor*)lm->Mips[0]->DataPtr)[x + y * lm->Mips[0]->USize];
			ALevelInfo* info = Surface.Level->GetLevelInfo();
			debugf(TEXT("PT LOOK TIME: level %.4f real %.4f lightmap %d %d %d%s"), info ? info->TimeSeconds : 0.0f, appSeconds(),
				(int)t.B, (int)t.G, (int)t.R, lm->bRealtimeChanged ? TEXT(" rebuilt") : TEXT(""));
			LightmapSeriesLastFrame = FrameIndex;
			LightmapSeriesFrames--;
		}
	}

	if (LightmapProbeSurf < 0)
		return;
	if (FrameIndex > LightmapProbeUntil)
	{
		debugf(TEXT("PT: the engine did not draw that surface's lightmap in the frames after PT LOOK"));
		LightmapProbeSurf = -1;
		return;
	}
	UModel* model = Surface.Level ? Surface.Level->Model : nullptr;
	if (!model || !Facet.Polys)
		return;
	FSavedPoly* poly = nullptr;
	for (FSavedPoly* p = Facet.Polys; p && !poly; p = p->Next)
		if (p->iNode >= 0 && p->iNode < model->Nodes.Num() && model->Nodes(p->iNode).iSurf == LightmapProbeSurf && p->NumPts >= 3)
			poly = p;
	if (!poly)
		return;
	const INT probedSurf = LightmapProbeSurf;
	LightmapProbeSurf = -1;

	FTextureInfo* lm = Surface.LightMap;
	if (!lm || !lm->Mips[0] || !lm->Mips[0]->DataPtr)
	{
		debugf(TEXT("PT: the engine drew that surface with no lightmap"));
		return;
	}
	// The facet's mapping is in the same space as its points, which may be
	// the camera's: find the probe point in whichever space lies on the
	// polygon's plane.
	const FVector a = poly->Pts[0]->Point, b = poly->Pts[1]->Point, c = poly->Pts[2]->Point;
	const FVector normal = ((b - a) ^ (c - a)).SafeNormal();
	const FVector world = LightmapProbePoint;
	const FVector camera = LightmapProbePoint.TransformPointBy(Frame->Coords);
	const float offWorld = Abs((world - a) | normal), offCamera = Abs((camera - a) | normal);
	const FVector point = offCamera < offWorld ? camera : world;
	const FCoords& map = Facet.MapCoords;
	const float u = ((map.XAxis | point) - (map.XAxis | map.Origin) - lm->Pan.X) / lm->UScale;
	const float v = ((map.YAxis | point) - (map.YAxis | map.Origin) - lm->Pan.Y) / lm->VScale;

	// RGBA7 texels, their B field red as the devices read it, each seven
	// bits; a device uploads them doubled and blends them doubled again, so
	// 127 draws a texture at twice its brightness.
	const INT pitch = lm->Mips[0]->USize;
	const FColor* texels = (const FColor*)lm->Mips[0]->DataPtr;
	auto texel = [&](INT x, INT y, int k) -> float
	{
		x = Clamp(x, 0, Max(lm->UClamp, 1) - 1);
		y = Clamp(y, 0, Max(lm->VClamp, 1) - 1);
		const FColor& t = texels[x + y * pitch];
		return (float)(k == 0 ? t.B : (k == 1 ? t.G : t.R));
	};
	const INT x0 = appFloor(u), y0 = appFloor(v);
	const float fu = u - x0, fv = v - y0;
	float value[3], peak[3] = {}, mean[3] = {};
	for (int k = 0; k < 3; k++)
	{
		value[k] = (texel(x0, y0, k) * (1 - fu) + texel(x0 + 1, y0, k) * fu) * (1 - fv)
			+ (texel(x0, y0 + 1, k) * (1 - fu) + texel(x0 + 1, y0 + 1, k) * fu) * fv;
		for (INT y = 0; y < lm->VClamp; y++)
			for (INT x = 0; x < lm->UClamp; x++)
			{
				const float t = texel(x, y, k);
				peak[k] = Max(peak[k], t);
				mean[k] += t;
			}
		mean[k] /= Max(lm->UClamp * lm->VClamp, 1);
	}
	debugf(TEXT("PT: the engine's own lightmap there (%s space, %.2f off the plane): texel %.1f %.1f of %dx%d; %.1f %.1f %.1f of 127, so drawn at %.3f %.3f %.3f of the texture; the whole lightmap peaks at %.0f %.0f %.0f and averages %.1f %.1f %.1f"),
		offCamera < offWorld ? TEXT("camera") : TEXT("world"), Min(offWorld, offCamera), u, v, lm->UClamp, lm->VClamp,
		value[0], value[1], value[2], value[0] * 4.0f / 255.0f, value[1] * 4.0f / 255.0f, value[2] * 4.0f / 255.0f,
		peak[0], peak[1], peak[2], mean[0], mean[1], mean[2]);
	// Where the trace puts the same point on it, from the level's own record
	// of the lightmap, which is what the shadow masks are laid on with.
	if (FLightMapIndex* index = model->GetLightMapIndex(probedSurf))
	{
		const FBspSurf& surf = model->Surfs(probedSurf);
		const FVector d = world - model->Points(surf.pBase);
		debugf(TEXT("PT: the level's record of it: texel %.2f %.2f (pan %.1f %.1f, scale %.2f %.2f; drawn with pan %.1f %.1f, scale %.2f %.2f)"),
			((d | model->Vectors(surf.vTextureU)) - index->Pan.X) / index->UScale, ((d | model->Vectors(surf.vTextureV)) - index->Pan.Y) / index->VScale,
			index->Pan.X, index->Pan.Y, index->UScale, index->VScale, lm->Pan.X, lm->Pan.Y, lm->UScale, lm->VScale);
	}
}

// A mesh the engine draws. The level's own come with the span buffer they
// were found visible through, and the trace has them already; so does the
// first person weapon, which the canvas draws over the view without one.
//
// What the trace does not have is the vision augmentation's heat sources: the
// HUD (AugmentationDisplayWindow, through Extension's XGC::DrawActor) draws
// every person and body in reach over the view, through walls, unlit, at
// twice its glow and in the style it set - translucent - each skin swapped
// for static by GetGridTexture: WhiteStatic, or in multiplayer Virus_SFX or
// Wepn_Prifle_SFX for an enemy or an ally (and BlackMaskTex, black, which
// adds nothing). Those are drawn here, over the traced picture with the rest
// of the HUD, in the order it draws them.
void UPathTracerRenderDevice::DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span)
{
	guardSlow(UPathTracerRenderDevice::DrawGouraudPolygon);

	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: mesh polygon %s, %d corners, first at %.0f %.0f, flags 0x%x%s"),
			Info.Texture ? Info.Texture->GetFullName() : TEXT("none"), NumPts, NumPts > 0 ? Pts[0]->ScreenX : 0.0f, NumPts > 0 ? Pts[0]->ScreenY : 0.0f,
			(int)PolyFlags, Span ? TEXT(", in the level") : TEXT(""));

	if (Span || !Textures || TraceWidth <= 0 || TraceHeight <= 0 || NumPts < 3 || !Info.Texture)
		return;
	static const FName whiteStatic(TEXT("WhiteStatic")), virus(TEXT("Virus_SFX")), rifle(TEXT("Wepn_Prifle_SFX"));
	const FName name = Info.Texture->GetFName();
	if (name != whiteStatic && name != virus && name != rifle)
		return;
	if (!LoggedVisionMesh)
	{
		debugf(TEXT("PathTracer: drawing the vision augmentation's heat sources over the view (%s, flags 0x%x)"), Info.Texture->GetFullName(), (int)PolyFlags);
		LoggedVisionMesh = true;
	}

	const DWORD flags = PolyFlags | Info.Texture->PolyFlags;
	const bool masked = (flags & PF_Masked) != 0 && (flags & PF_Modulated) == 0;
	CachedTexture* texture = Textures->Get(Info, masked);
	if (!texture)
		return;
	const int blendMode = (flags & PF_Translucent) ? 1 : ((flags & PF_Modulated) ? 2 : 0);
	const int samplerMode = (PolyFlags & PF_NoSmooth) ? 1 : 0;
	if (!TileSet(texture, samplerMode))
		return;

	// Placed as a tile is: in the frame's pixels, offset to where the frame
	// sits, and to where the UI does when it is pinned.
	const float sx = 2.0f / (float)TraceWidth;
	const float sy = 2.0f / (float)TraceHeight;
	const float uScale = Info.USize > 0 ? 1.0f / (Info.UScale * Info.USize) : 0.0f;
	const float vScale = Info.VSize > 0 ? 1.0f / (Info.VScale * Info.VSize) : 0.0f;
	auto corner = [&](const FTransTexture* p) -> TileVertex
	{
		const float x = p->ScreenX + Frame->XB + (float)UiOffsetX;
		const float y = p->ScreenY + Frame->YB;
		const vec4 colour = (flags & PF_Modulated) ? vec4(1.0f, 1.0f, 1.0f, 1.0f) : vec4(p->Light.X, p->Light.Y, p->Light.Z, 1.0f);
		return { vec2(x * sx - 1.0f, y * sy - 1.0f), vec2(p->U * uScale, p->V * vScale), colour };
	};

	if (TileBatches.empty() || TileBatches.back().Texture != texture || TileBatches.back().BlendMode != blendMode ||
		TileBatches.back().SamplerMode != samplerMode)
	{
		TileBatch batch;
		batch.Texture = texture;
		batch.BlendMode = blendMode;
		batch.SamplerMode = samplerMode;
		batch.FirstVertex = (int)TileVertices.size();
		batch.VertexCount = 0;
		TileBatches.push_back(batch);
	}
	// A fan, as the engine gives a polygon.
	const TileVertex first = corner(Pts[0]);
	for (int i = 1; i + 1 < NumPts; i++)
	{
		TileVertices.push_back(first);
		TileVertices.push_back(corner(Pts[i]));
		TileVertices.push_back(corner(Pts[i + 1]));
		TileBatches.back().VertexCount += 3;
	}

	unguardSlow;
}
// The engine's 2D drawing: HUD, menus, console, subtitles, the mouse cursor.
//
// Collected here rather than drawn, because the traced image does not exist yet
// when these arrive - the whole frame is traced in Unlock. Batches are merged
// while the texture and blend mode hold, which for a menu is most of it.
void UPathTracerRenderDevice::DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags)
{
	guardSlow(UPathTracerRenderDevice::DrawTile);

	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: tile %s at %.0f %.0f size %.0f %.0f flags 0x%x colour %.2f %.2f %.2f%s"),
			Info.Texture ? Info.Texture->GetFullName() : TEXT("none"), X, Y, XL, YL, (int)PolyFlags, Color.X, Color.Y, Color.Z,
			Span ? TEXT(", in the level") : TEXT(""));

	if (!Textures || TraceWidth <= 0 || TraceHeight <= 0 || PhotoHidesTiles())
		return;

	// A sprite in the level - a sprite actor, or one of a particle system's
	// particles - which the engine draws here with the span buffer it found
	// it visible through. The trace has these already, placed where walls hide
	// them; drawn here too they were pasted over the picture, the steam from a
	// vent showing through the side of a building. What the canvas draws - the
	// HUD, the menus, the effects over the weapon - comes without a span.
	if (Span)
	{
		if (!LoggedWorldSprite)
		{
			debugf(TEXT("PathTracer: the engine's sprites in the level are left to the trace (first: %s at depth %.1f)"),
				Info.Texture ? Info.Texture->GetFullName() : TEXT("no texture"), Z);
			LoggedWorldSprite = true;
		}
		return;
	}

	// A texture can carry PF_Masked itself rather than the caller passing it.
	// Modulated art is excluded: its transparency is carried by the grey level
	// rather than by a palette hole, and punching alpha into it would leave
	// gaps where the texture happens to use index zero as a real colour.
	const DWORD flags = PolyFlags | (Info.Texture ? Info.Texture->PolyFlags : 0);
	const bool masked = (flags & PF_Masked) != 0 && (flags & PF_Modulated) == 0;
#if defined(OLDUNREAL469SDK)
	// 469's own 2D, its text above all, asks for PF_Highlighted: premultiplied
	// alpha, the palette's alpha taken as it is.
	const bool highlighted = (flags & PF_Highlighted) != 0 && (flags & (PF_Translucent | PF_Modulated)) == 0;
#else
	const bool highlighted = false;
#endif
	CachedTexture* texture = Textures->Get(Info, masked, highlighted);
	if (!texture)
		return;

	int blendMode = 0;
	if (flags & PF_Translucent)
		blendMode = 1;
	else if (flags & PF_Modulated)
		blendMode = 2;
	else if (highlighted)
		blendMode = 3;

	// What each piece of 2D art actually asks for. The crosshair of a scope
	// arrives as a black square, which means it is not asking for the additive
	// blend its artwork assumes.
	// Only once a level is up: the menu has enough art to use the whole budget
	// before anything in the game is drawn, which it has done twice now.
	// The engine gives tile positions in viewport pixels including the frame's
	// own offset, and texture coordinates in texels. With the UI pinned the
	// viewport sits in the middle of the trace.
	float x0 = (X + Frame->XB) + (float)UiOffsetX;
	const float y0 = (Y + Frame->YB);
	float x1 = x0 + XL;
	const float y1 = y0 + YL;

	const float sx = 2.0f / (float)TraceWidth;
	const float sy = 2.0f / (float)TraceHeight;

	const float uScale = Info.USize > 0 ? 1.0f / (Info.UScale * Info.USize) : 0.0f;
	const float vScale = Info.VSize > 0 ? 1.0f / (Info.VScale * Info.VSize) : 0.0f;

	float u0 = U * uScale;
	const float v0 = V * vScale;
	float u1 = (U + UL) * uScale;
	const float v1 = (V + VL) * vScale;

	// Sampled the way the other devices sample it: see TileSamplers. By the
	// flags the tile was drawn with alone, as they and the original D3D
	// driver take them, not with the texture's own added.
	int samplerMode = (PolyFlags & PF_NoSmooth) ? 1 : 0;
	if (Min(u0, u1) >= 0.0f && Max(u0, u1) <= 1.00001f && Min(v0, v1) >= 0.0f && Max(v0, v1) <= 1.00001f)
		samplerMode |= 2;

	// What spans the engine's whole width - a conversation's black bars, a
	// fade, the darkening behind a menu - is meant to span the screen, and
	// stopping at the edges of a pinned UI left the world showing past its
	// ends. Carried on to the trace's edges at the same texel density, so a
	// pattern continues rather than stretches.
	if (UiOffsetX > 0 && XL > 0.0f && x0 - (float)UiOffsetX <= 0.5f && x1 - (float)UiOffsetX >= (float)Viewport->SizeX - 0.5f)
	{
		const float uPerPixel = (u1 - u0) / (x1 - x0);
		u0 -= x0 * uPerPixel;
		u1 += ((float)TraceWidth - x1) * uPerPixel;
		x0 = 0.0f;
		x1 = (float)TraceWidth;
	}

	vec4 colour = vec4(Color.X, Color.Y, Color.Z, 1.0f);
	if (flags & PF_Modulated)
		colour = vec4(1.0f, 1.0f, 1.0f, 1.0f);

	TileVertex corners[4];
	corners[0] = { vec2(x0 * sx - 1.0f, y0 * sy - 1.0f), vec2(u0, v0), colour };
	corners[1] = { vec2(x1 * sx - 1.0f, y0 * sy - 1.0f), vec2(u1, v0), colour };
	corners[2] = { vec2(x1 * sx - 1.0f, y1 * sy - 1.0f), vec2(u1, v1), colour };
	corners[3] = { vec2(x0 * sx - 1.0f, y1 * sy - 1.0f), vec2(u0, v1), colour };

	if (!TileSet(texture, samplerMode))
		return;

	if (TileBatches.empty() || TileBatches.back().Texture != texture || TileBatches.back().BlendMode != blendMode ||
		TileBatches.back().SamplerMode != samplerMode)
	{
		TileBatch batch;
		batch.Texture = texture;
		batch.BlendMode = blendMode;
		batch.SamplerMode = samplerMode;
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
void UPathTracerRenderDevice::Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2)
{
	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: line from %.0f %.0f to %.0f %.0f (not drawn)"), P1.X, P1.Y, P2.X, P2.Y);
}
void UPathTracerRenderDevice::Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z)
{
	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: point %.0f %.0f to %.0f %.0f (not drawn)"), X1, Y1, X2, Y2);
}
void UPathTracerRenderDevice::ClearZ(FSceneNode* Frame)
{
	if (LogDraws && LoggedDraws++ < 400)
		debugf(TEXT("PT TILES: clear depth"));
}
#if defined(OLDUNREAL469SDK)
// What the 2D's textures can come as: what TextureCache decodes. The engine
// converts anything else before handing it over.
UBOOL UPathTracerRenderDevice::SupportsTextureFormat(ETextureFormat Format)
{
	return Format == TEXF_P8 || Format == TEXF_BGRA8 || Format == TEXF_RGB8 || Format == TEXF_BC1 || Format == TEXF_BC1_PA;
}
#endif

void UPathTracerRenderDevice::PushHit(const BYTE* Data, INT Count) {}
void UPathTracerRenderDevice::PopHit(INT Count, UBOOL bForce) {}
void UPathTracerRenderDevice::GetStats(TCHAR* Result) { Result[0] = 0; }

// The last frame shown, for the game's own copies of the screen: the save
// menu's snapshot, which Extension's XRootWindow::GenerateSnapshot takes by
// calling this from script between frames, and the menus' Snapshot
// background. The output image keeps that frame - traced picture, 2D and
// Brightness - until the next one is drawn over it. Returned as the other
// devices here return it: the viewport's size, the top row first, and each
// pixel's bytes blue, green, red, alpha. Left empty, every save went without
// its picture.
void UPathTracerRenderDevice::ReadPixels(FColor* Pixels)
{
	guard(UPathTracerRenderDevice::ReadPixels);

	const int width = Viewport ? (int)Viewport->SizeX : 0;
	const int height = Viewport ? (int)Viewport->SizeY : 0;
	if (!Pixels || width <= 0 || height <= 0)
		return;
	const size_t bytes = (size_t)width * height * 4;
	if (!Device || !OutputImage)
	{
		memset(Pixels, 0, bytes);
		return;
	}

	// The engine's view, which with the UI pinned sits in the middle of a
	// wider trace.
	const int sourceX = Clamp(UiOffsetX, 0, TraceWidth - 1);
	const int sourceWidth = Max(Min(width, TraceWidth - sourceX), 1);
	const int sourceHeight = Max(Min(height, TraceHeight), 1);

	auto target = ImageBuilder()
		.Format(VK_FORMAT_B8G8R8A8_UNORM)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerReadPixels")
		.Create(Device.get());
	auto staging = BufferBuilder()
		.Size(bytes)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("PathTracerReadPixelsStaging")
		.Create(Device.get());

	VulkanImage* source = OutputImage.get();
	VulkanImage* dest = target.get();
	VulkanBuffer* download = staging.get();
	ExecuteImmediate([&](VulkanCommandBuffer* cmd)
	{
		// In HDR, brought back to what SDR would have shown, the save game's
		// picture and the screenshot being SDR.
		VulkanImage* from = source;
		if (HdrMode)
		{
			EncodeFrame(cmd, true);
			if (SdrImage)
				from = SdrImage.get();
		}

		// After the last frame's blit to the window, which is on the same
		// queue ahead of this.
		PipelineBarrier()
			.AddImage(source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(dest, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkImageBlit blit = {};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.srcOffsets[0] = { sourceX, 0, 0 };
		blit.srcOffsets[1] = { sourceX + sourceWidth, sourceHeight, 1 };
		blit.dstSubresource = blit.srcSubresource;
		blit.dstOffsets[1] = { width, height, 1 };
		const bool sameSize = sourceWidth == width && sourceHeight == height;
		cmd->blitImage(from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

		PipelineBarrier()
			.AddImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
			.AddImage(dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy region = {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
		cmd->copyImageToBuffer(dest->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download->buffer, 1, &region);
	});

	// Opaque whatever the tiles left in the alpha.
	const uint8_t* mapped = (const uint8_t*)staging->Map(0, bytes);
	uint8_t* out = (uint8_t*)Pixels;
	memcpy(out, mapped, bytes);
	staging->Unmap();
	for (size_t i = 3; i < bytes; i += 4)
		out[i] = 255;

	unguard;
}
