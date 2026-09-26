#pragma once

// The 64-bit helper's precompiled header. The tracer's GPU side was written
// inside the render device, and uses a handful of the engine's macros - debugf
// for the log, TEXT for its strings, guard and unguard around functions. Here
// they are plain C: the log is the helper's own file, strings are narrow, and
// guard is only a scope.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <functional>

// printf formatted, to PathTracerHelper.log beside the game's log. The engine's
// %S - a narrow string inside a wide format - is accepted and read as %s.
void HelperLog(const char* format, ...);

#define TEXT(s) s
#define debugf HelperLog
#define guard(name) {
#define unguard }
#define guardSlow(name) {
#define unguardSlow }
