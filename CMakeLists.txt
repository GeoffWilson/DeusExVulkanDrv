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

# The SDK headers include each other with Windows' casing rules, and the D3D
# sources ask the Windows SDK for <D3D11.h> and <D3D12.h> when the files are
# lower case. Both are the same problem; see the script.
set(CASE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk-case-compat")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env python3
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/case-compat.py"
		--out "${CASE_COMPAT_DIR}"
		--scan "${DEUSEX_SDK}/Core/Inc" "${DEUSEX_SDK}/Engine/Inc"
	COMMAND_ERROR_IS_FATAL ANY)

# XWIN_DIR is set by the toolchain file; without it there is no Windows SDK to
# resolve against and this step is simply skipped.
if(DEFINED XWIN_DIR)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env python3
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/case-compat.py"
			--out "${CASE_COMPAT_DIR}"
			--scan "${CMAKE_CURRENT_SOURCE_DIR}/D3D11Drv" "${CMAKE_CURRENT_SOURCE_DIR}/D3D12Drv"
			--search "${XWIN_DIR}/sdk/include/um" "${XWIN_DIR}/sdk/include/shared"
		COMMAND_ERROR_IS_FATAL ANY)
endif()

# Everything the three render devices share. Their vcxproj DeusExRelease
# configurations differ only in the exports define and which graphics libraries
# they link, so the rest is stated once here.
function(configure_deusex_renderdevice target exports_define)
	target_include_directories(${target} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty"
		"${DEUSEX_SDK}/Core/Inc"
		"${DEUSEX_SDK}/Engine/Inc"
		"${CASE_COMPAT_DIR}")

	target_compile_definitions(${target} PRIVATE
		WIN32 NDEBUG _WINDOWS _USRDLL ${exports_define} DEUSEX
		# Deus Ex's Core.dll exports the UNICODE flavour of the engine API (TCHAR
		# is UNICHAR), which the VS project gets from CharacterSet=Unicode.
		# Together with /Zc:wchar_t- below this makes TCHAR an unsigned short, the
		# type the 2000-era import libraries were built against.
		UNICODE _UNICODE
		# The SDK's hand written MASM blocks (FVertex::ComputeOutcode and friends)
		# are inline functions we never call, but clang-cl cannot parse their
		# struct-offset syntax, so take the C++ fallbacks instead.
		ASM=0 ASM3DNOW=0 ASMKNI=0)

	# Matches the DeusExRelease ClCompile settings: static CRT, asynchronous
	# exceptions and a non-native wchar_t so the VC6-era import libs still link.
	# The engine's 4 byte packing is stated in each Precomp.h rather than with a
	# global /Zp, which clang-cl treats as a cap a pragma cannot widen past.
	target_compile_options(${target} PRIVATE /MT /EHa /Zc:wchar_t- /GR /Zi)
	target_link_options(${target} PRIVATE /DEBUG)

	set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${target}")

	target_link_libraries(${target} PRIVATE
		"${DEUSEX_SDK}/Core/Lib/Core.lib"
		"${DEUSEX_SDK}/Engine/Lib/Engine.lib"
		user32 gdi32 advapi32 ole32 shell32)
endfunction()

target_include_directories(VulkanDrv PRIVATE "${VULKANDRV_DIR}")
configure_deusex_renderdevice(VulkanDrv VULKANDRV_EXPORTS)
target_link_libraries(VulkanDrv PRIVATE zvulkan)
target_compile_options(zvulkan PRIVATE /MT /EHsc)

# ---------------------------------------------------------------- D3D11Drv ---

set(D3D11DRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/D3D11Drv")

# The OpenXR headers are not bundled with this repository and the vcxproj does
# not build OpenXRBackend.cpp either, so the default is a stub whose nullptr the
# caller already treats as "no headset, run mono". Point this at the real file
# if you have the SDK on the include path.
option(D3D11DRV_OPENXR "Build the OpenXR VR backend (needs the OpenXR SDK headers)" OFF)
if(D3D11DRV_OPENXR)
	set(D3D11DRV_VR_SOURCE "OpenXRBackend.cpp")
else()
	set(D3D11DRV_VR_SOURCE "OpenXRBackendStub.cpp")
endif()

add_library(D3D11Drv SHARED
	"${D3D11DRV_DIR}/CycleTimer.cpp"
	"${D3D11DRV_DIR}/D3D11Drv.cpp"
	"${D3D11DRV_DIR}/FileResource.cpp"
	"${D3D11DRV_DIR}/halffloat.cpp"
	"${D3D11DRV_DIR}/mat.cpp"
	"${D3D11DRV_DIR}/${D3D11DRV_VR_SOURCE}"
	"${D3D11DRV_DIR}/Precomp.cpp"
	"${D3D11DRV_DIR}/TextureManager.cpp"
	"${D3D11DRV_DIR}/TextureUploader.cpp"
	"${D3D11DRV_DIR}/UD3D11RenderDevice.cpp"
	"${D3D11DRV_DIR}/UploadManager.cpp"
	"${D3D11DRV_DIR}/UTF16.cpp")

target_include_directories(D3D11Drv PRIVATE "${D3D11DRV_DIR}")
configure_deusex_renderdevice(D3D11Drv D3D11DRV_EXPORTS)
target_link_libraries(D3D11Drv PRIVATE d3d11 dxgi d3dcompiler dxguid)

# ---------------------------------------------------------------- D3D12Drv ---

set(D3D12DRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/D3D12Drv")

add_library(D3D12Drv SHARED
	"${D3D12DRV_DIR}/D3D12Drv.cpp"
	"${D3D12DRV_DIR}/D3D12MemAlloc/D3D12MemAlloc.cpp"
	"${D3D12DRV_DIR}/FileResource.cpp"
	"${D3D12DRV_DIR}/halffloat.cpp"
	"${D3D12DRV_DIR}/mat.cpp"
	"${D3D12DRV_DIR}/Precomp.cpp"
	"${D3D12DRV_DIR}/TextureManager.cpp"
	"${D3D12DRV_DIR}/TextureUploader.cpp"
	"${D3D12DRV_DIR}/UD3D12RenderDevice.cpp"
	"${D3D12DRV_DIR}/UploadManager.cpp"
	"${D3D12DRV_DIR}/UTF16.cpp")

target_include_directories(D3D12Drv PRIVATE "${D3D12DRV_DIR}")
configure_deusex_renderdevice(D3D12Drv D3D12DRV_EXPORTS)
target_link_libraries(D3D12Drv PRIVATE d3d12 dxgi d3dcompiler dxguid)

# ------------------------------------------------------------- PathTracerDrv ---

set(PATHTRACERDRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/PathTracerDrv")

add_library(PathTracerDrv SHARED
	"${PATHTRACERDRV_DIR}/AccelStructure.cpp"
	"${PATHTRACERDRV_DIR}/Denoiser.cpp"
	"${PATHTRACERDRV_DIR}/LevelScene.cpp"
	"${PATHTRACERDRV_DIR}/Materials.cpp"
	"${PATHTRACERDRV_DIR}/mat.cpp"
	"${PATHTRACERDRV_DIR}/PathTracerDrv.cpp"
	"${PATHTRACERDRV_DIR}/Precomp.cpp"
	"${PATHTRACERDRV_DIR}/Shaders.cpp"
	"${PATHTRACERDRV_DIR}/TextureCache.cpp"
	"${PATHTRACERDRV_DIR}/UPathTracerRenderDevice.cpp")

target_include_directories(PathTracerDrv PRIVATE "${PATHTRACERDRV_DIR}")
configure_deusex_renderdevice(PathTracerDrv PATHTRACERDRV_EXPORTS)
target_link_libraries(PathTracerDrv PRIVATE zvulkan)

# NVIDIA's NRD denoiser, when cmake/build-nrd.sh has built it. Without it the
# driver builds as before and PT DENOISE says why it cannot.
set(PATHTRACER_NRD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.nrd" CACHE PATH "Output of cmake/build-nrd.sh")
if(EXISTS "${PATHTRACER_NRD_DIR}/x86/NRD.lib")
	message(STATUS "PathTracerDrv: denoising with NRD from ${PATHTRACER_NRD_DIR}")
	target_include_directories(PathTracerDrv PRIVATE "${PATHTRACER_NRD_DIR}/src/Include")
	target_compile_definitions(PathTracerDrv PRIVATE PATHTRACER_NRD NRD_STATIC_LIBRARY)
	target_link_libraries(PathTracerDrv PRIVATE "${PATHTRACER_NRD_DIR}/x86/NRD.lib")
else()
	message(STATUS "PathTracerDrv: no NRD at ${PATHTRACER_NRD_DIR}, building without the denoiser")
endif()

# ------------------------------------------------------------------- spikes ---

# Not built by default: one-question programs about the environment, not part of
# any driver. See spike/README.md.
#   cmake --build build-deusex --target vkrtcheck
add_executable(vkrtcheck EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/spike/vkrtcheck.cpp")
target_include_directories(vkrtcheck PRIVATE "${ZVULKAN_DIR}/include")
target_compile_options(vkrtcheck PRIVATE /MT /EHsc)
set_target_properties(vkrtcheck PROPERTIES OUTPUT_NAME "vkrtcheck")
