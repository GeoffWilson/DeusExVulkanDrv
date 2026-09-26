#pragma once

// The precompiled header for the files built into both halves of the path
// tracer: the render device in the game's 32-bit process, and the 64-bit
// helper that does the tracing. The helper has no engine to link against, so
// it gets its own, which stands in for the few engine macros those files use.
#ifdef PATHTRACER_HELPER
#include "HelperPrecomp.h"
#else
#include "Precomp.h"
#endif
