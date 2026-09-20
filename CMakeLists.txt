# Cross-build of VulkanDrv for Deus Ex (engine 1112f).
#
# The Visual Studio solution remains the canonical build for Unreal Tournament.
# This CMake project exists so the Deus Ex configuration can be built and
# iterated on from Linux with clang-cl; it mirrors the DeusExRelease settings
# of VulkanDrv.vcxproj.

cmake_minimum_required(VERSION 3.20)
project(DeusExVulkanDrv CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEUSEX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty/DeusEx" CACHE PATH "Deus Ex 1112f SDK (headers + import libs)")

# ---------------------------------------------------------------- ZVulkan ----

set(ZVULKAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ZVulkan")

set(ZVULKAN_SOURCES
	src/vulkanbuilders.cpp
	src/vulkandevice.cpp
	src/vulkaninstance.cpp
	src/vulkansurface.cpp
	src/vulkanswapchain.cpp
	src/vk_mem_alloc/vk_mem_alloc.cpp
	src/volk/volk.c
	src/glslang/glslang/GenericCodeGen/CodeGen.cpp
	src/glslang/glslang/GenericCodeGen/Link.cpp
	src/glslang/glslang/MachineIndependent/attribute.cpp
	src/glslang/glslang/MachineIndependent/Constant.cpp
	src/glslang/glslang/MachineIndependent/glslang_tab.cpp
	src/glslang/glslang/MachineIndependent/InfoSink.cpp
	src/glslang/glslang/MachineIndependent/Initialize.cpp
	src/glslang/glslang/MachineIndependent/Intermediate.cpp
	src/glslang/glslang/MachineIndependent/intermOut.cpp
	src/glslang/glslang/MachineIndependent/IntermTraverse.cpp
	src/glslang/glslang/MachineIndependent/iomapper.cpp
	src/glslang/glslang/MachineIndependent/limits.cpp
	src/glslang/glslang/MachineIndependent/linkValidate.cpp
	src/glslang/glslang/MachineIndependent/parseConst.cpp
	src/glslang/glslang/MachineIndependent/ParseContextBase.cpp
	src/glslang/glslang/MachineIndependent/ParseHelper.cpp
	src/glslang/glslang/MachineIndependent/PoolAlloc.cpp
	src/glslang/glslang/MachineIndependent/preprocessor/Pp.cpp
	src/glslang/glslang/MachineIndependent/preprocessor/PpAtom.cpp
	src/glslang/glslang/MachineIndependent/preprocessor/PpContext.cpp
	src/glslang/glslang/MachineIndependent/preprocessor/PpScanner.cpp
	src/glslang/glslang/MachineIndependent/preprocessor/PpTokens.cpp
	src/glslang/glslang/MachineIndependent/propagateNoContraction.cpp
	src/glslang/glslang/MachineIndependent/reflection.cpp
	src/glslang/glslang/MachineIndependent/RemoveTree.cpp
	src/glslang/glslang/MachineIndependent/Scan.cpp
	src/glslang/glslang/MachineIndependent/ShaderLang.cpp
	src/glslang/glslang/MachineIndependent/SpirvIntrinsics.cpp
	src/glslang/glslang/MachineIndependent/SymbolTable.cpp
	src/glslang/glslang/MachineIndependent/Versions.cpp
	src/glslang/glslang/OSDependent/Windows/ossource.cpp
	src/glslang/glslang/ResourceLimits/ResourceLimits.cpp
	src/glslang/spirv/disassemble.cpp
	src/glslang/spirv/doc.cpp
	src/glslang/spirv/GlslangToSpv.cpp
	src/glslang/spirv/InReadableOrder.cpp
	src/glslang/spirv/Logger.cpp
	src/glslang/spirv/SpvBuilder.cpp
	src/glslang/spirv/SpvPostProcess.cpp
	src/glslang/spirv/SPVRemapper.cpp
	src/glslang/spirv/SpvTools.cpp)
list(TRANSFORM ZVULKAN_SOURCES PREPEND "${ZVULKAN_DIR}/")

add_library(zvulkan STATIC ${ZVULKAN_SOURCES})
target_include_directories(zvulkan PUBLIC
	"${ZVULKAN_DIR}"
	"${ZVULKAN_DIR}/include"
	"${ZVULKAN_DIR}/include/zvulkan")
target_compile_definitions(zvulkan PRIVATE _LIB)

# --------------------------------------------------------------- VulkanDrv ---

set(VULKANDRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/VulkanDrv")

add_library(VulkanDrv SHARED
	"${VULKANDRV_DIR}/BufferManager.cpp"
	"${VULKANDRV_DIR}/CommandBufferManager.cpp"
	"${VULKANDRV_DIR}/DescriptorSetManager.cpp"
	"${VULKANDRV_DIR}/FileResource.cpp"
	"${VULKANDRV_DIR}/FramebufferManager.cpp"
	"${VULKANDRV_DIR}/halffloat.cpp"
	"${VULKANDRV_DIR}/mat.cpp"
	"${VULKANDRV_DIR}/Precomp.cpp"
	"${VULKANDRV_DIR}/RenderPassManager.cpp"
	"${VULKANDRV_DIR}/SamplerManager.cpp"
	"${VULKANDRV_DIR}/SceneTextures.cpp"
	"${VULKANDRV_DIR}/ShaderManager.cpp"
	"${VULKANDRV_DIR}/TextureManager.cpp"
	"${VULKANDRV_DIR}/TextureUploader.cpp"
	"${VULKANDRV_DIR}/UploadManager.cpp"
	"${VULKANDRV_DIR}/UVulkanRenderDevice.cpp"
	"${VULKANDRV_DIR}/VulkanDrv.cpp")

set_target_properties(VulkanDrv PROPERTIES PREFIX "" OUTPUT_NAME "VulkanDrv")

# The SDK headers include each other with Windows' casing rules; see the script.
set(CASE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk-case-compat")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env python3
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/case-compat.py" "${CASE_COMPAT_DIR}"
		"${DEUSEX_SDK}/Core/Inc" "${DEUSEX_SDK}/Engine/Inc"
	COMMAND_ERROR_IS_FATAL ANY)

target_include_directories(VulkanDrv PRIVATE
	"${VULKANDRV_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty"
	"${DEUSEX_SDK}/Core/Inc"
	"${DEUSEX_SDK}/Engine/Inc"
	"${CASE_COMPAT_DIR}")

target_compile_definitions(VulkanDrv PRIVATE
	WIN32 NDEBUG _WINDOWS _USRDLL VULKANDRV_EXPORTS DEUSEX
	# Deus Ex's Core.dll exports the UNICODE flavour of the engine API (TCHAR is
	# UNICHAR), which the VS project gets from CharacterSet=Unicode. Together
	# with /Zc:wchar_t- below this makes TCHAR an unsigned short, the type the
	# 2000-era import libraries were built against.
	UNICODE _UNICODE
	# The SDK's hand written MASM blocks (FVertex::ComputeOutcode and friends)
	# are inline functions we never call, but clang-cl cannot parse their
	# struct-offset syntax, so take the C++ fallbacks instead.
	ASM=0 ASM3DNOW=0 ASMKNI=0)

# Matches the DeusExRelease ClCompile settings: static CRT, asynchronous
# exceptions, 4 byte struct packing (the engine's ABI) and a non-native wchar_t
# so the VC6-era import libs still link.
target_compile_options(VulkanDrv PRIVATE /MT /EHa /Zc:wchar_t- /GR /Zi)
target_link_options(VulkanDrv PRIVATE /DEBUG)
target_compile_options(zvulkan PRIVATE /MT /EHsc)

target_link_libraries(VulkanDrv PRIVATE
	zvulkan
	"${DEUSEX_SDK}/Core/Lib/Core.lib"
	"${DEUSEX_SDK}/Engine/Lib/Engine.lib"
	user32 gdi32 advapi32 ole32 shell32)
