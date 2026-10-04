#pragma once

#include <string>

namespace Shaders
{
	// The trace shader, as GLSL. Embedded rather than shipped as a file so the
	// driver is a single DLL, the same way VulkanDrv carries its shaders.
	std::string Trace();

	// Each fog light's shadow cube, traced before the trace reads it.
	std::string FogShadows();

	// The picture put back together from the denoised lighting: see Denoiser.
	std::string Composite();

	// The picture after DLSS Ray Reconstruction: see RayReconstruction.
	std::string Finish();

	// The 2D pass. The engine draws its HUD, menus and console as tiles, and
	// without them the game renders but cannot be used.
	std::string TileVertex();
	std::string TileFragment();
	// The modulated tiles as the HUD's own image for a headset takes them,
	// which has no picture under it to darken: see CreateTilePipeline.
	std::string TileFragmentModulatedHud();

	// The game's Brightness setting, applied over the finished picture - the
	// 2D and all - as a gamma curve.
	std::string Brightness();
	std::string Encode();

	// The eyes' pictures and the HUD as a headset takes them: in linear
	// light, with the Brightness on. See HeadsetOutput.
	std::string HeadsetEncode();
}
