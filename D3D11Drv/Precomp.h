#pragma once

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#define USE_SSE2
#endif

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#pragma pack(push, 8)
#include <Windows.h>
#include <D3D11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <comdef.h>
#undef min
#undef max
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <unordered_map>
#include <set>
#include <functional>
#ifdef USE_SSE2
#include <emmintrin.h>
//#include <immintrin.h>
#endif
#pragma pack(pop)

#define UTGLR_NO_APP_MALLOC
#include <stdlib.h>

#ifdef _MSC_VER
#pragma warning(disable: 4297) // warning C4297: 'UObject::operator delete': function assumed not to throw an exception but does
#endif

// The engine's ABI is 4 byte packed (StructMemberAlignment in the vcxproj),
// while the Windows and D3D headers above need their natural alignment. MSVC
// gets that from the pragma pack around them plus a global /Zp4; clang-cl will
// not honour a pragma that widens past a command line /Zp, so the cross build
// passes no /Zp at all and the engine's packing is stated here instead. Both
// compilers then see the same layout.
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

#if !defined(OLDUNREAL469SDK)

// The VR paths in this file are written against the 469 SDK. They cannot run on
// an older engine - CreateOpenXRBackend has no headers to build against, so it
// returns nullptr and UseVR stays off - but they still have to compile. These
// give them the three things 469 has and the older SDKs do not.

// Engine.h does not pull the console class in on these SDKs, and the VR cursor
// reflection needs the complete type.
#include "UnCon.h"

// GUglyHackFlags itself exists; these particular bits are 469's own, set by its
// renderer around overlays and the post-render HUD pass. Zero makes every test
// against them false, which is the right answer on an engine that never sets
// them and never reaches this code anyway.
enum { HACKFLAGS_PostRender = 0, HACKFLAGS_NoNearZ = 0 };

// 469 renamed the seconds clock when it made it monotonic.
inline DOUBLE appSecondsNew() { return appSeconds(); }

#endif

#pragma pack(pop)
