#pragma once

#include <string>

namespace Shaders
{
	// The trace shader, as GLSL. Embedded rather than shipped as a file so the
	// driver is a single DLL, the same way VulkanDrv carries its shaders.
	std::string Trace();
}
