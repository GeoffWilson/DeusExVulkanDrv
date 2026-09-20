
#include "Precomp.h"
#include "ShaderManager.h"
#include "FileResource.h"
#include "UVulkanRenderDevice.h"

ShaderManager::ShaderManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
	ShaderBuilder::Init();

	Scene.VertexShader = ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource("shaders/Scene.vert", LoadShaderCode("shaders/Scene.vert", "#extension GL_EXT_nonuniform_qualifier : enable\r\n"))
		.DebugName("vertexShader")
		.Create("vertexShader", renderer->Device.get());

	Scene.FragmentShader = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/Scene.frag", LoadShaderCode("shaders/Scene.frag", "#extension GL_EXT_nonuniform_qualifier : enable\r\n#"))
		.DebugName("fragmentShader")
		.Create("fragmentShader", renderer->Device.get());

	Scene.FragmentShaderAlphaTest = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/Scene.frag", LoadShaderCode("shaders/Scene.frag", "#extension GL_EXT_nonuniform_qualifier : enable\r\n#define ALPHATEST"))
		.DebugName("fragmentShader")
		.Create("fragmentShader", renderer->Device.get());

	// Used for masked geometry when multisampling is on, where the alpha becomes
	// a coverage mask instead of a straight pass or fail.
	Scene.FragmentShaderAlphaToCoverage = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/Scene.frag", LoadShaderCode("shaders/Scene.frag", "#extension GL_EXT_nonuniform_qualifier : enable\r\n#define ALPHATEST\r\n#define ALPHATOCOVERAGE"))
		.DebugName("fragmentShader")
		.Create("fragmentShader", renderer->Device.get());

	Postprocess.VertexShader = ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource("shaders/PPStep.vert", LoadShaderCode("shaders/PPStep.vert"))
		.DebugName("ppVertexShader")
		.Create("ppVertexShader", renderer->Device.get());

	static const char* transferFunctions[2] = { nullptr, "HDR_MODE" };
	static const char* gammaModes[2] = { "GAMMA_MODE_D3D9", "GAMMA_MODE_XOPENGL" };
	static const char* colorModes[4] = { nullptr, "COLOR_CORRECT_MODE0", "COLOR_CORRECT_MODE1", "COLOR_CORRECT_MODE2" };
	for (int i = 0; i < 64; i++)
	{
		// Bit 5 picks HDR10 over scRGB and only means anything alongside bit 0,
		// so half of the combinations it introduces do not exist. Leaving those
		// slots empty keeps the startup cost of the second HDR path to the
		// sixteen variants that can actually be selected.
		if ((i & 32) && !(i & 1))
			continue;

		std::string defines;
		if (transferFunctions[i & 1]) defines += std::string("#define ") + transferFunctions[i & 1] + "\r\n";
		if (gammaModes[(i >> 1) & 1]) defines += std::string("#define ") + gammaModes[(i >> 1) & 1] + "\r\n";
		if (colorModes[(i >> 2) & 3]) defines += std::string("#define ") + colorModes[(i >> 2) & 3] + "\r\n";
		if ((i >> 4) & 1) defines += "#define SRGB_SCENE\r\n";
		if ((i >> 5) & 1) defines += "#define HDR10_MODE\r\n";

		Postprocess.FragmentPresentShader[i] = ShaderBuilder()
			.Type(ShaderType::Fragment)
			.AddSource("shaders/Present.frag", LoadShaderCode("shaders/Present.frag", defines))
			.DebugName("ppFragmentPresentShader")
			.Create("ppFragmentPresentShader", renderer->Device.get());
	}

	Bloom.Extract = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/BloomExtract.frag", LoadShaderCode("shaders/BloomExtract.frag"))
		.DebugName("BloomPass.Extract")
		.Create("BloomPass.Extract", renderer->Device.get());

	Bloom.Combine = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/BloomCombine.frag", LoadShaderCode("shaders/BloomCombine.frag"))
		.DebugName("BloomPass.Combine")
		.Create("BloomPass.Combine", renderer->Device.get());

	Bloom.BlurVertical = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/BlurVertical.frag", LoadShaderCode("shaders/Blur.frag", "#define BLUR_VERTICAL"))
		.DebugName("BloomPass.BlurVertical")
		.Create("BloomPass.BlurVertical", renderer->Device.get());

	Bloom.BlurHorizontal = ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource("shaders/BlurHorizontal.frag", LoadShaderCode("shaders/Blur.frag", "#define BLUR_HORIZONTAL"))
		.DebugName("BloomPass.BlurHorizontal")
		.Create("BloomPass.BlurHorizontal", renderer->Device.get());
}

ShaderManager::~ShaderManager()
{
	ShaderBuilder::Deinit();
}

std::string ShaderManager::LoadShaderCode(const std::string& filename, const std::string& defines)
{
	const char* shaderversion = R"(
		#version 450
		#extension GL_ARB_separate_shader_objects : enable
	)";
	return shaderversion + defines + "\r\n#line 1\r\n" + FileResource::readAllText(filename);
}
