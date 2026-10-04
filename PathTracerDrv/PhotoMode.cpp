#include "Precomp.h"
#include "UPathTracerRenderDevice.h"
#include "TraceProtocol.h"
#include "UnCon.h"
#include <wincodec.h>
#include <chrono>

// Photo mode (PT PHOTO): the world held still and a camera flown free through
// it, the picture refined sample by sample while the camera rests, and saved
// to a PNG beside the game's folders.
//
// The world is held by LevelInfo's bPlayersOnly, the engine's own switch for
// ticking only the players: nothing else moves, fires, talks or triggers.
// The player is put in the engine's CheatFlying state, which turns the view
// and asks for a velocity from the game's own movement keys and mouse, but
// with its physics off, so the body stays exactly where it stood - it never
// passes a trigger or enters a zone, and a game saved meanwhile would find it
// there - and the camera is flown by that velocity here instead. Pausing the
// game would hold the player too: the engine takes a paused player's input
// before a frame is drawn, and nothing would be left to steer by.
//
// Everything is put back as it ends: the player's state, physics, rotation
// and pose, the level's switch and the key bindings. It ends at PT PHOTO, and
// whenever a menu or a screen opens - every one pauses the game - so a save
// through the menus never finds it on. Only the keys that move, look, open
// the console or the menu, pause or take a screenshot are kept while it is
// on: firing, using, the belt and the augmentations would act on the frozen
// world from wherever the camera had flown, and QuickSave would save it
// frozen. Fire and QuickSave take the photo instead.

static double PhotoNowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What photo mode does with a key's binding: leaves it, blanks it, or makes
// it take the photo. Judged by the first word of each command the binding
// runs, which is either a command or one of the input's aliases.
enum PhotoKey { PhotoKeyKept, PhotoKeyBlanked, PhotoKeyTakes };

static PhotoKey JudgeBinding(const TCHAR* binding)
{
	static const TCHAR* const kept[] = {
		TEXT("Axis"), TEXT("Button"), TEXT("Toggle"),
		TEXT("MoveForward"), TEXT("MoveBackward"), TEXT("StrafeLeft"), TEXT("StrafeRight"), TEXT("Strafe"),
		TEXT("TurnLeft"), TEXT("TurnRight"), TEXT("LookUp"), TEXT("LookDown"), TEXT("Look"), TEXT("LookToggle"),
		TEXT("CenterView"), TEXT("Jump"), TEXT("Duck"), TEXT("Walking"), TEXT("ToggleWalk"),
		TEXT("LeanLeft"), TEXT("LeanRight"),
		TEXT("ShowMainMenu"), TEXT("ShowMenu"), TEXT("Pause"), TEXT("QuickLoad"),
		TEXT("Shot"), TEXT("Type"), TEXT("Talk"), TEXT("ConsoleToggle"), TEXT("PT"),
	};
	static const TCHAR* const takes[] = { TEXT("Fire"), TEXT("AltFire"), TEXT("ParseLeftClick"), TEXT("QuickSave") };

	PhotoKey verdict = PhotoKeyKept;
	const TCHAR* p = binding;
	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == '|')
			p++;
		TCHAR word[64];
		int length = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '|')
		{
			if (length < 63)
				word[length++] = *p;
			p++;
		}
		word[length] = 0;
		while (*p && *p != '|')
			p++;
		if (!length)
			continue;

		bool found = false;
		for (const TCHAR* name : takes)
			found = found || !appStricmp(word, name);
		if (found)
		{
			verdict = PhotoKeyTakes;
			continue;
		}
		for (const TCHAR* name : kept)
			found = found || !appStricmp(word, name);
		if (!found && verdict == PhotoKeyKept)
			verdict = PhotoKeyBlanked;
	}
	return verdict;
}

void UPathTracerRenderDevice::PhotoCommand(const TCHAR* Cmd, FOutputDevice& Ar)
{
	guard(UPathTracerRenderDevice::PhotoCommand);

	if (ParseCommand(&Cmd, TEXT("SAVE")))
	{
		if (!Photo.Active)
			Ar.Logf(TEXT("PT: not in photo mode (PT PHOTO)"));
		else
			Photo.SavePending = true;
		return;
	}
	if (ParseCommand(&Cmd, TEXT("APERTURE")))
	{
		Photo.Aperture = Max(appAtof(Cmd), 0.0f);
		AccumulatedFrames = 0;
		if (Photo.Aperture > 0.0f)
			Ar.Logf(TEXT("PT: photo aperture %.1f units across its radius, focused %s"), Photo.Aperture,
				Photo.Focus > 0.0f ? *FString::Printf(TEXT("%.0f units ahead"), Photo.Focus) : TEXT("on the middle of the view"));
		else
			Ar.Logf(TEXT("PT: photo aperture 0, a pinhole: everything sharp"));
		return;
	}
	if (ParseCommand(&Cmd, TEXT("FOCUS")))
	{
		Photo.Focus = ParseCommand(&Cmd, TEXT("AUTO")) ? 0.0f : Max(appAtof(Cmd), 0.0f);
		AccumulatedFrames = 0;
		if (Photo.Focus > 0.0f)
			Ar.Logf(TEXT("PT: photos focused %.0f units ahead"), Photo.Focus);
		else
			Ar.Logf(TEXT("PT: photos focused on whatever is in the middle of the view"));
		return;
	}
	if (ParseCommand(&Cmd, TEXT("OFF")) || Photo.Active)
	{
		if (Photo.Active)
		{
			EndPhoto(TEXT("PT PHOTO"));
			Ar.Logf(TEXT("PT: photo mode off"));
		}
		return;
	}
	if (StartPhoto(Ar))
	{
		Ar.Logf(TEXT("PT: photo mode. The world is held still: fly with the movement keys and the mouse, jump and crouch to rise and sink, walk to go slowly."));
		Ar.Logf(TEXT("PT: hold still and the picture refines. Fire, or PT PHOTO SAVE, saves it; PT PHOTO or the menu ends it. PT PHOTO APERTURE n, PT PHOTO FOCUS n|AUTO for depth of field."));
	}

	unguard;
}

bool UPathTracerRenderDevice::StartPhoto(FOutputDevice& Ar)
{
	APlayerPawn* pawn = Viewport ? Viewport->Actor : nullptr;
	FStateFrame* frame = pawn ? pawn->GetStateFrame() : nullptr;
	const FName state = (frame && frame->StateNode) ? frame->StateNode->GetFName() : FName(NAME_None);
	if (!pawn || !pawn->Level || !pawn->XLevel || pawn->XLevel != Scene.SourceLevel || Scene.IsEmpty() || Scene.ViewActor != pawn)
	{
		Ar.Logf(TEXT("PT: photo mode needs a level being traced"));
		return false;
	}
	// Holding the world and flying the player are the local game's to do: in
	// a network game the server has the world, and the player, its own way.
	if (pawn->Level->NetMode != NM_Standalone)
	{
		Ar.Logf(TEXT("PT: photo mode is for a game played alone, not a network one"));
		return false;
	}
	if (HeadsetNow)
	{
		Ar.Logf(TEXT("PT: photo mode is for the screen, not the headset (PT VR switches it off)"));
		return false;
	}
	if (pawn->Level->Pauser.Len() || pawn->bShowMenu)
	{
		Ar.Logf(TEXT("PT: photo mode cannot start while the game is paused"));
		return false;
	}
	// Walking or swimming, the states the player is put back into. Not in a
	// conversation, a cutscene, dying, or already flying as a cheat.
	if (state != FName(TEXT("PlayerWalking")) && state != FName(TEXT("PlayerSwimming")) && state != FName(TEXT("PlayerFlying")))
	{
		Ar.Logf(TEXT("PT: photo mode starts only while the player walks or swims, not in %s"), *state);
		return false;
	}

	Photo.Pawn = pawn;
	Photo.Level = pawn->XLevel;
	Photo.State = state;
	Photo.Physics = pawn->Physics;
	Photo.Velocity = pawn->Velocity;
	Photo.Acceleration = pawn->Acceleration;
	Photo.Rotation = pawn->Rotation;
	Photo.ViewRotation = pawn->ViewRotation;
	Photo.EyeHeight = pawn->EyeHeight;
	Photo.PlayersOnly = pawn->Level->bPlayersOnly != 0;
	Photo.AnimSequence = pawn->AnimSequence;
	Photo.AnimFrame = pawn->AnimFrame;
	Photo.AnimRate = pawn->AnimRate;
	Photo.TweenRate = pawn->TweenRate;
	Photo.AnimLast = pawn->AnimLast;
	Photo.AnimMinRate = pawn->AnimMinRate;
	Photo.OldAnimRate = pawn->OldAnimRate;
	Photo.SimAnim = pawn->SimAnim;

	// The camera starts where the view was, first person or from behind; the
	// body's eyes are where it is kept out of the view until the camera has
	// left them.
	Photo.Position = Scene.ViewOrigin;
	Photo.Eye = pawn->Location + FVector(0.0f, 0.0f, pawn->EyeHeight);
	Photo.LastMs = PhotoNowMs();
	Photo.Time = (float)fmod((double)pawn->Level->TimeSeconds, 1000.0);
	for (int i = 0; i < 3; i++)
		Photo.Flashlight[i] = Scene.Flashlight[i];
	Photo.StillFrames = 0;
	Photo.Accumulating = false;
	Photo.SavePending = false;

	pawn->Level->bPlayersOnly = 1;
	pawn->GotoState(FName(TEXT("CheatFlying")));
	pawn->setPhysics(PHYS_None);
	pawn->Velocity = FVector(0.0f, 0.0f, 0.0f);
	pawn->Acceleration = FVector(0.0f, 0.0f, 0.0f);

	Photo.Input = Viewport->Input;
	Photo.Bindings.clear();
	if (Photo.Input)
	{
		for (int key = 0; key < IK_MAX; key++)
		{
			FString& binding = Photo.Input->Bindings[key];
			if (!binding.Len())
				continue;
			const PhotoKey verdict = JudgeBinding(*binding);
			if (verdict == PhotoKeyKept)
				continue;
			Photo.Bindings.push_back(std::make_pair(key, FString(binding)));
			binding = verdict == PhotoKeyTakes ? TEXT("PT PHOTO SAVE") : TEXT("");
		}
	}

	Photo.Active = true;
	AccumulatedFrames = 0;
	DenoiseRestart = true;
	debugf(TEXT("PathTracer: photo mode on in %s, %d key bindings set aside"), Photo.Level->GetOuter()->GetName(), (int)Photo.Bindings.size());
	return true;
}

// Puts back what photo mode changed. The player only if it is still the one
// it began with, in the level it began in, and still flying for it: after a
// level change the old player is gone, and a script that moved the player on
// has a state of its own for it.
void UPathTracerRenderDevice::EndPhoto(const TCHAR* why)
{
	if (!Photo.Active)
		return;
	Photo.Active = false;

	if (Photo.Input && Viewport && Viewport->Input == Photo.Input)
		for (const auto& binding : Photo.Bindings)
			Photo.Input->Bindings[binding.first] = binding.second;
	Photo.Bindings.clear();
	Photo.Input = nullptr;

	APlayerPawn* pawn = Viewport ? Viewport->Actor : nullptr;
	if (pawn && pawn == Photo.Pawn && pawn->XLevel == Photo.Level && !pawn->bDeleteMe)
	{
		if (pawn->Level)
			pawn->Level->bPlayersOnly = Photo.PlayersOnly;
		FStateFrame* frame = pawn->GetStateFrame();
		if (frame && frame->StateNode && frame->StateNode->GetFName() == FName(TEXT("CheatFlying")))
		{
			pawn->Rotation = Photo.Rotation;
			pawn->ViewRotation = Photo.ViewRotation;
			pawn->EyeHeight = Photo.EyeHeight;
			pawn->AnimSequence = Photo.AnimSequence;
			pawn->AnimFrame = Photo.AnimFrame;
			pawn->AnimRate = Photo.AnimRate;
			pawn->TweenRate = Photo.TweenRate;
			pawn->AnimLast = Photo.AnimLast;
			pawn->AnimMinRate = Photo.AnimMinRate;
			pawn->OldAnimRate = Photo.OldAnimRate;
			pawn->SimAnim = Photo.SimAnim;
			pawn->bPressedJump = 0;
			pawn->GotoState(Photo.State);
			// After the state, whose beginning may choose physics of its own.
			pawn->setPhysics(Photo.Physics);
			pawn->Velocity = Photo.Velocity;
			pawn->Acceleration = Photo.Acceleration;
		}
	}

	Photo.Pawn = nullptr;
	Photo.Level = nullptr;
	Photo.Accumulating = false;
	Photo.SavePending = false;
	AccumulatedFrames = 0;
	DenoiseRestart = true;
	debugf(TEXT("PathTracer: photo mode off (%s)"), why);
}

// A game saved while photo mode was on - only possible by typing QuickSave
// into the console, the keys and the menus being kept from it - comes back
// with the world held and the player flying for it, which only photo mode
// does. Put right as the level is taken up: the body never moved, so it
// walks again from where it stood.
void UPathTracerRenderDevice::RepairPhotoSave()
{
	APlayerPawn* pawn = Viewport ? Viewport->Actor : nullptr;
	FStateFrame* frame = pawn ? pawn->GetStateFrame() : nullptr;
	if (Photo.Active || !frame || !frame->StateNode || frame->StateNode->GetFName() != FName(TEXT("CheatFlying")) ||
		pawn->Physics != PHYS_None || !pawn->Level || !pawn->Level->bPlayersOnly)
		return;
	pawn->Level->bPlayersOnly = 0;
	pawn->GotoState(FName(TEXT("PlayerWalking")));
	pawn->setPhysics(PHYS_Falling);
	debugf(TEXT("PathTracer: this game was saved in photo mode; the world and the player are put back to walking"));
}

// Once a frame, before anything is drawn: whether photo mode can go on.
void UPathTracerRenderDevice::CheckPhoto()
{
	if (!Photo.Active)
		return;
	APlayerPawn* pawn = Viewport ? Viewport->Actor : nullptr;
	if (!pawn || pawn != Photo.Pawn || pawn->XLevel != Photo.Level)
		EndPhoto(TEXT("the level changed"));
	else if ((pawn->Level && pawn->Level->Pauser.Len()) || pawn->bShowMenu)
		EndPhoto(TEXT("a menu opened"));
}

// The camera flown by the velocity the flying asks for - 300 units a second
// the way the keys point, along the view - and a quarter of that while the
// walk key is held. Called with the player's view each frame.
void UPathTracerRenderDevice::MovePhotoCamera()
{
	const double now = PhotoNowMs();
	const float seconds = Clamp((float)((now - Photo.LastMs) / 1000.0), 0.0f, 0.1f);
	Photo.LastMs = now;
	if (!Photo.Pawn)
		return;
	const float speed = Photo.Pawn->bRun ? 0.25f : 1.0f;
	Photo.Position += Photo.Pawn->Velocity * (seconds * speed);
}

// The body's pose as it stood, in the player while the scene is gathered from
// it, and the flying's back afterwards: called in pairs.
void UPathTracerRenderDevice::SwapPhotoPose()
{
	APlayerPawn* pawn = Photo.Pawn;
	if (!pawn)
		return;
	std::swap(pawn->Rotation, Photo.Rotation);
	std::swap(pawn->AnimSequence, Photo.AnimSequence);
	std::swap(pawn->AnimFrame, Photo.AnimFrame);
	std::swap(pawn->AnimRate, Photo.AnimRate);
	std::swap(pawn->TweenRate, Photo.TweenRate);
	std::swap(pawn->AnimLast, Photo.AnimLast);
	std::swap(pawn->AnimMinRate, Photo.AnimMinRate);
	std::swap(pawn->OldAnimRate, Photo.OldAnimRate);
	std::swap(pawn->SimAnim, Photo.SimAnim);
}

// Whether the console is open to type into, when the 2D is left in so what
// is typed can be seen. Engine.Console's own properties, found by name.
bool UPathTracerRenderDevice::PhotoConsoleOpen()
{
	UConsole* console = Viewport ? Viewport->Console : nullptr;
	if (!console)
		return false;
	static UClass* lookedUp = nullptr;
	static UBoolProperty* typing = nullptr;
	static UFloatProperty* position = nullptr;
	if (console->GetClass() != lookedUp)
	{
		lookedUp = console->GetClass();
		typing = FindField<UBoolProperty>(lookedUp, TEXT("bTyping"));
		position = FindField<UFloatProperty>(lookedUp, TEXT("ConsolePos"));
	}
	if (typing && (*(BITFIELD*)((BYTE*)console + typing->Offset) & typing->BitMask))
		return true;
	return position && *(FLOAT*)((BYTE*)console + position->Offset) > 0.0f;
}

// The finished picture, Brightness and all, copied out of the frame being
// recorded - its output image already waiting to be blitted to the window -
// for WritePhoto once the frame is done.
void UPathTracerRenderDevice::RecordPhotoSave(VulkanCommandBuffer* commands, VulkanImage* source)
{
	Photo.SavePending = false;
	const int width = TraceWidth, height = TraceHeight;
	if (width <= 0 || height <= 0 || !source)
		return;

	Photo.SaveImage = ImageBuilder()
		.Format(VK_FORMAT_B8G8R8A8_UNORM)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("PathTracerPhoto")
		.Create(Device.get());
	Photo.SaveBuffer = BufferBuilder()
		.Size((size_t)width * height * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("PathTracerPhotoStaging")
		.Create(Device.get());

	PipelineBarrier()
		.AddImage(Photo.SaveImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageBlit blit = {};
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.srcOffsets[1] = { width, height, 1 };
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1] = { width, height, 1 };
	commands->blitImage(source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Photo.SaveImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);
	PipelineBarrier()
		.AddImage(Photo.SaveImage.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
	commands->copyImageToBuffer(Photo.SaveImage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Photo.SaveBuffer->buffer, 1, &region);

	Photo.SaveWidth = width;
	Photo.SaveHeight = height;
	Photo.SaveSamples = Photo.Accumulating ? Min<uint32_t>(AccumulatedFrames + 1, PhotoState::MaxSamples) : 0u;
	Photo.SaveRecorded = true;
}

// A PNG through Windows' own imaging component, which wine has as well.
static bool EncodePng(const TCHAR* path, int width, int height, const BYTE* bgr)
{
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	IWICImagingFactory* factory = nullptr;
	IWICStream* stream = nullptr;
	IWICBitmapEncoder* encoder = nullptr;
	IWICBitmapFrameEncode* frame = nullptr;
	IPropertyBag2* properties = nullptr;
	bool written = false;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&factory)) &&
		SUCCEEDED(factory->CreateStream(&stream)) &&
		SUCCEEDED(stream->InitializeFromFilename((LPCWSTR)path, GENERIC_WRITE)) &&
		SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
		SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) &&
		SUCCEEDED(frame->Initialize(properties)) &&
		SUCCEEDED(frame->SetSize((UINT)width, (UINT)height)))
	{
		WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
		written = SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
			SUCCEEDED(frame->WritePixels((UINT)height, (UINT)width * 3, (UINT)width * height * 3, (BYTE*)bgr)) &&
			SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
	}
	if (properties) properties->Release();
	if (frame) frame->Release();
	if (encoder) encoder->Release();
	if (stream) stream->Release();
	if (factory) factory->Release();
	if (SUCCEEDED(init))
		CoUninitialize();
	return written;
}

// A BMP, should there be no PNG encoder to be had.
static bool WriteBmp(const TCHAR* path, int width, int height, const BYTE* bgr)
{
	const int stride = (width * 3 + 3) & ~3;
	BITMAPFILEHEADER file = {};
	BITMAPINFOHEADER info = {};
	file.bfType = 0x4d42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + stride * height;
	info.biSize = sizeof(info);
	info.biWidth = width;
	info.biHeight = height;
	info.biPlanes = 1;
	info.biBitCount = 24;
	HANDLE handle = CreateFileW((LPCWSTR)path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	DWORD done = 0;
	bool written = WriteFile(handle, &file, sizeof(file), &done, nullptr) && WriteFile(handle, &info, sizeof(info), &done, nullptr);
	std::vector<BYTE> row(stride, 0);
	for (int y = height - 1; y >= 0 && written; y--)
	{
		memcpy(row.data(), bgr + (size_t)y * width * 3, (size_t)width * 3);
		written = WriteFile(handle, row.data(), stride, &done, nullptr) != 0;
	}
	CloseHandle(handle);
	return written;
}

// The copied picture to Photos, beside System: named for the map and the
// time, as a PNG.
void UPathTracerRenderDevice::WritePhoto()
{
	guard(UPathTracerRenderDevice::WritePhoto);
	Photo.SaveRecorded = false;
	if (!Photo.SaveBuffer)
		return;

	const int width = Photo.SaveWidth, height = Photo.SaveHeight;
	std::vector<BYTE> bgr((size_t)width * height * 3);
	const BYTE* mapped = (const BYTE*)Photo.SaveBuffer->Map(0, (size_t)width * height * 4);
	for (size_t i = 0, n = (size_t)width * height; i < n; i++)
	{
		bgr[i * 3 + 0] = mapped[i * 4 + 0];
		bgr[i * 3 + 1] = mapped[i * 4 + 1];
		bgr[i * 3 + 2] = mapped[i * 4 + 2];
	}
	Photo.SaveBuffer->Unmap();
	Photo.SaveBuffer.reset();
	Photo.SaveImage.reset();

	SYSTEMTIME now;
	GetLocalTime(&now);
	const TCHAR* map = (Scene.SourceLevel && Scene.SourceLevel->GetOuter()) ? Scene.SourceLevel->GetOuter()->GetName() : TEXT("Photo");
	CreateDirectoryW(L"..\\Photos", nullptr);
	FString name = FString::Printf(TEXT("..\\Photos\\%s %04d-%02d-%02d %02d-%02d-%02d"),
		map, (int)now.wYear, (int)now.wMonth, (int)now.wDay, (int)now.wHour, (int)now.wMinute, (int)now.wSecond);
	FString path = name + TEXT(".png");
	bool written = EncodePng(*path, width, height, bgr.data());
	if (!written)
	{
		DeleteFileW((LPCWSTR)*path);
		path = name + TEXT(".bmp");
		written = WriteBmp(*path, width, height, bgr.data());
	}

	TCHAR full[MAX_PATH] = {};
	if (!GetFullPathNameW((LPCWSTR)*path, MAX_PATH, (LPWSTR)full, nullptr))
		appStrncpy(full, *path, MAX_PATH);
	const FString samples = Photo.SaveSamples
		? FString::Printf(TEXT("%u samples a pixel"), Photo.SaveSamples)
		: FString(TEXT("the denoised preview: hold still for the refined picture"));
	const FString message = written
		? FString::Printf(TEXT("PT: photo saved, %dx%d, %s: %s"), width, height, *samples, full)
		: FString::Printf(TEXT("PT: the photo could not be written to %s"), full);
	debugf(TEXT("%s"), *message);
	if (Viewport && Viewport->Console)
		Viewport->Console->Logf(TEXT("%s"), *message);

	unguard;
}
