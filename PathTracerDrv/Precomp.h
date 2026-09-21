#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifdef _MSC_VER
#pragma warning(disable: 4297) // 'UObject::operator delete': assumed not to throw but does
#endif

#pragma pack(push, 8)
#ifdef WIN32
#include <Windows.h>
#endif
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <algorithm>
#pragma pack(pop)

// Keep the package's allocations on the CRT heap rather than the engine's; see
// the note in Thirdparty/DeusEx/Core/Inc/UnFile.h. C++14 sized deallocation
// bypasses the engine's replacement of operator delete, so memory taken from
// GMalloc would otherwise be handed to the CRT's free().
#define UTGLR_NO_APP_MALLOC
#include <stdlib.h>

// The engine's ABI is 4 byte packed. Stated here rather than with a global /Zp,
// which clang-cl treats as a cap a pragma cannot widen past.
#pragma pack(push, 4)

// Portable stand-ins for the SDK's MASM blocks. Must precede any SDK header.
#include "UE1AsmShims.h"

#include "Engine.h"
#include "UnRender.h"

#if !defined(UNREALGOLD) && !defined(DEUSEX)
#define OLDUNREAL469SDK
#endif

#if defined(OLDUNREAL469SDK)
#include "Render.h"
#endif

#pragma pack(pop)
