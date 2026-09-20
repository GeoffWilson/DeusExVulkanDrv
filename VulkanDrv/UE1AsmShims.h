#pragma once

// Replacements for the SDK's MSVC inline assembly.
//
// UnVcWin32.h implements appRound, appFloor, appCycles, appSeconds, appMemcpy,
// appMemzero and appDebugBreak as __asm blocks and nothing else defines them -
// they are not exported from Core.dll. Compilers that cannot parse MASM syntax
// (clang-cl, used by the CMake cross build) therefore have to build the SDK
// with ASM=0, which would leave all seven undefined. Defining the SDK's
// DEFINED_app* guards here, before any SDK header is parsed, substitutes
// portable equivalents instead.
//
// Only plain C types are used because Core.h has not been seen yet at this
// point, so INT, DWORD and friends do not exist.

#if defined(ASM) && ASM == 0

#include <cmath>
#include <cstring>
#include <intrin.h>

#define DEFINED_appRound 1
inline int appRound(float F)
{
	return (int)std::lrintf(F); // fistp, i.e. round to nearest, ties to even
}

#define DEFINED_appFloor 1
inline int appFloor(float F)
{
	return (int)std::floor(F);
}

#define DEFINED_appCycles 1
inline unsigned long appCycles()
{
	return (unsigned long)__rdtsc();
}

#define DEFINED_appSeconds 1
inline double appSeconds()
{
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
}

#define DEFINED_appMemcpy
inline void appMemcpy(void* Dest, const void* Src, int Count)
{
	std::memcpy(Dest, Src, (size_t)Count);
}

#define DEFINED_appMemzero
inline void appMemzero(void* Dest, int Count)
{
	std::memset(Dest, 0, (size_t)Count);
}

#define DEFINED_appDebugBreak
inline void appDebugBreak()
{
	__debugbreak();
}

#endif
