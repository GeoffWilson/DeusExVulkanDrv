#pragma once

#include <string>

namespace Shaders
{
	// The trace shader, as GLSL. Embedded rather than shipped as a file so the
	// driver is a single DLL, the same way VulkanDrv carries its shaders.
	std::string Trace();

	// The picture put back together from the denoised lighting: see Denoiser.
	std::string Composite();

	// The 2D pass. The engine draws its HUD, menus and console as tiles, and
	// without them the game renders but cannot be used.
	std::string TileVertex();
	std::string TileFragment();
}
