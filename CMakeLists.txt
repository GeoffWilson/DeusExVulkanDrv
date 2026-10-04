# Cross-build of VulkanDrv for Deus Ex (engine 1112f).
#
# The Visual Studio solution remains the canonical build for Unreal Tournament.
# This CMake project exists so the Deus Ex configuration can be built and
# iterated on from Linux with clang-cl; it mirrors the DeusExRelease settings
# of VulkanDrv.vcxproj.
#
# PATHTRACER_GAME=UT469 builds PathTracerDrv for Unreal Tournament 469 instead,
# against OldUnreal's SDK (UT469_SDK), 64-bit or 32-bit as the toolchain is;
# see the UT469 section below.

cmake_minimum_required(VERSION 3.20)
project(DeusExVulkanDrv CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEUSEX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty/DeusEx" CACHE PATH "Deus Ex 1112f SDK (headers + import libs)")

# Which game PathTracerDrv is built for. DeusEx: the 32-bit configuration
# builds every render device for Deus Ex and the 64-bit one the path tracer's
# helper. UT469: PathTracerDrv for Unreal Tournament 469, whose Windows builds
# come in both widths - the 64-bit configuration builds the helper beside it.
set(PATHTRACER_GAME "DeusEx" CACHE STRING "Game PathTracerDrv is built for: DeusEx or UT469")
set_property(CACHE PATHTRACER_GAME PROPERTY STRINGS DeusEx UT469)
# OldUnreal's SDK for the 469 patches (OldUnreal-UTPatch469f-SDK-Windows.zip
# from github.com/OldUnreal/UnrealTournamentPatches), unpacked. Not part of this
# repository: its headers are Epic's, under the Unreal licence.
set(UT469_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../UT469fSDK" CACHE PATH "OldUnreal UT 469 SDK (Core, Engine and Render headers and import libs)")

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
target_compile_options(zvulkan PRIVATE /MT /EHsc)

# NVIDIA's NRD denoiser, when cmake/build-nrd.sh has built it: x86/NRD.lib and
# x64/NRD.lib. The tracing is 64-bit, and links the latter.
set(PATHTRACER_NRD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.nrd" CACHE PATH "Output of cmake/build-nrd.sh")
# NVIDIA's DLSS Ray Reconstruction, when cmake/fetch-dlss.sh has fetched the
# SDK. Its runtime, nvngx_dlssd.dll, goes beside the executable, where NGX
# looks.
set(PATHTRACER_DLSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.dlss" CACHE PATH "Output of cmake/fetch-dlss.sh")
# Khronos's OpenXR loader and headers, when cmake/fetch-openxr.sh has fetched
# them, for a headset. The loader, openxr_loader.dll, goes beside the helper,
# which opens it only when a headset is asked for.
set(PATHTRACER_OPENXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.openxr" CACHE PATH "Output of cmake/fetch-openxr.sh")
# AMD's FSR 3.1 upscaler, when cmake/fetch-fsr.sh has fetched FidelityFX SDK
# v1.1.4 and compiled its shaders: for a GPU that cannot run Ray
# Reconstruction, the trace runs at a fraction of the screen's size and FSR
# brings it up to it. Only the upscaler, the Vulkan backend and what they
# share are built, against ZVulkan's Vulkan headers, the backend's own
# Vulkan calls going through volk's pointers as everything else's do.
set(PATHTRACER_FSR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.fsr" CACHE PATH "Output of cmake/fetch-fsr.sh")
set(PATHTRACER_FSR OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS "${PATHTRACER_FSR_DIR}/generated/ffx_fsr3upscaler_accumulate_pass_permutations.h")
	set(PATHTRACER_FSR ON)
	set(FFX_SDK "${PATHTRACER_FSR_DIR}/sdk")
	add_library(ffx_fsr3upscaler STATIC
		"${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
		"${FFX_SDK}/src/backends/vk/ffx_vk.cpp"
		"${FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp"
		"${FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
		"${FFX_SDK}/src/shared/ffx_assert.cpp"
		"${FFX_SDK}/src/shared/ffx_message.cpp"
		"${FFX_SDK}/src/shared/ffx_object_management.cpp"
		"${FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp")
	target_include_directories(ffx_fsr3upscaler PUBLIC "${FFX_SDK}/include")
	target_include_directories(ffx_fsr3upscaler PRIVATE
		"${FFX_SDK}/src/shared"
		"${FFX_SDK}/src/backends/shared"
		"${FFX_SDK}/src/backends/shared/blob_accessors"
		"${FFX_SDK}/src/components"
		"${PATHTRACER_FSR_DIR}/generated")
	target_link_libraries(ffx_fsr3upscaler PRIVATE zvulkan)
	target_compile_definitions(ffx_fsr3upscaler PRIVATE FFX_FSR3UPSCALER _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
	target_compile_options(ffx_fsr3upscaler PRIVATE /MT /EHsc /FIvolk/volk.h -Wno-everything)
endif()
set(PATHTRACERDRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/PathTracerDrv")

# The tracing: everything that turns the scene into a picture on the GPU, and
# knows nothing of the engine. Built into PathTracerHelper.exe, or into a
# 64-bit game's render device itself.
set(PATHTRACER_TRACE_SOURCES
	"${PATHTRACERDRV_DIR}/AccelStructure.cpp"
	"${PATHTRACERDRV_DIR}/Denoiser.cpp"
	"${PATHTRACERDRV_DIR}/FrameUploads.cpp"
	"${PATHTRACERDRV_DIR}/FsrUpscaler.cpp"
	"${PATHTRACERDRV_DIR}/RayReconstruction.cpp"
	"${PATHTRACERDRV_DIR}/TraceHost.cpp"
	"${PATHTRACERDRV_DIR}/TraceRenderer.cpp")

# NRD and DLSS for whichever target does the tracing.
function(link_trace_libraries target)
	if(EXISTS "${PATHTRACER_NRD_DIR}/x64/NRD.lib")
		message(STATUS "${target}: denoising with NRD from ${PATHTRACER_NRD_DIR}")
		target_include_directories(${target} PRIVATE "${PATHTRACER_NRD_DIR}/src/Include")
		target_compile_definitions(${target} PRIVATE PATHTRACER_NRD NRD_STATIC_LIBRARY)
		target_link_libraries(${target} PRIVATE "${PATHTRACER_NRD_DIR}/x64/NRD.lib")
	else()
		message(STATUS "${target}: no NRD at ${PATHTRACER_NRD_DIR}/x64, building without the denoiser")
	endif()
	if(EXISTS "${PATHTRACER_DLSS_DIR}/x64/nvsdk_ngx_s.lib")
		message(STATUS "${target}: DLSS Ray Reconstruction from ${PATHTRACER_DLSS_DIR}")
		target_include_directories(${target} PRIVATE "${PATHTRACER_DLSS_DIR}/include")
		target_compile_definitions(${target} PRIVATE PATHTRACER_DLSS)
		target_link_libraries(${target} PRIVATE "${PATHTRACER_DLSS_DIR}/x64/nvsdk_ngx_s.lib")
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PATHTRACER_DLSS_DIR}/nvngx_dlssd.dll" "$<TARGET_FILE_DIR:${target}>")
	else()
		message(STATUS "${target}: no DLSS SDK at ${PATHTRACER_DLSS_DIR}, building without Ray Reconstruction")
	endif()
	if(PATHTRACER_FSR)
		message(STATUS "${target}: FSR 3.1 from ${PATHTRACER_FSR_DIR}")
		target_compile_definitions(${target} PRIVATE PATHTRACER_FSR)
		target_link_libraries(${target} PRIVATE ffx_fsr3upscaler)
	else()
		message(STATUS "${target}: no FSR at ${PATHTRACER_FSR_DIR}/generated, building without it")
	endif()
endfunction()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT PATHTRACER_GAME STREQUAL "UT469")

# ---------------------------------------------------------- PathTracerHelper ---

# For Deus Ex a 64-bit build makes only the path tracer's helper: the render
# devices are engine plugins, and the engine is 32-bit. See
# PathTracerDrv/TraceProtocol.h. Unreal Tournament 469 comes 64-bit as well,
# and its 64-bit render device traces in the game's own process, needing no
# helper: see below.
add_executable(PathTracerHelper
	${PATHTRACER_TRACE_SOURCES}
	"${PATHTRACERDRV_DIR}/HeadsetOutput.cpp"
	"${PATHTRACERDRV_DIR}/HelperMain.cpp"
	"${PATHTRACERDRV_DIR}/mat.cpp"
	"${PATHTRACERDRV_DIR}/Shaders.cpp")
target_include_directories(PathTracerHelper PRIVATE "${PATHTRACERDRV_DIR}")
# PATHTRACER_HEADSET: the trace sources built here, and only here, can show on a headset.
target_compile_definitions(PathTracerHelper PRIVATE PATHTRACER_HELPER PATHTRACER_HEADSET WIN32 NDEBUG _CONSOLE)
target_compile_options(PathTracerHelper PRIVATE /MT /EHsc /Zi)
target_link_options(PathTracerHelper PRIVATE /DEBUG)
target_link_libraries(PathTracerHelper PRIVATE zvulkan user32 advapi32)
link_trace_libraries(PathTracerHelper)
if(EXISTS "${PATHTRACER_OPENXR_DIR}/include/openxr/openxr.h")
	message(STATUS "PathTracerHelper: headsets through OpenXR from ${PATHTRACER_OPENXR_DIR}")
	target_include_directories(PathTracerHelper PRIVATE "${PATHTRACER_OPENXR_DIR}/include")
	target_compile_definitions(PathTracerHelper PRIVATE PATHTRACER_OPENXR)
	add_custom_command(TARGET PathTracerHelper POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PATHTRACER_OPENXR_DIR}/openxr_loader.dll" "$<TARGET_FILE_DIR:PathTracerHelper>")
else()
	message(STATUS "PathTracerHelper: no OpenXR SDK at ${PATHTRACER_OPENXR_DIR}, building without headsets")
endif()

endif()

set(PATHTRACERDRV_SOURCES
	"${PATHTRACERDRV_DIR}/LevelScene.cpp"
	"${PATHTRACERDRV_DIR}/Materials.cpp"
	"${PATHTRACERDRV_DIR}/mat.cpp"
	"${PATHTRACERDRV_DIR}/PathTracerDrv.cpp"
	"${PATHTRACERDRV_DIR}/PhotoMode.cpp"
	"${PATHTRACERDRV_DIR}/Precomp.cpp"
	"${PATHTRACERDRV_DIR}/Shaders.cpp"
	"${PATHTRACERDRV_DIR}/TextureCache.cpp"
	"${PATHTRACERDRV_DIR}/TraceClient.cpp"
	"${PATHTRACERDRV_DIR}/UPathTracerRenderDevice.cpp")

if(PATHTRACER_GAME STREQUAL "UT469")

# ----------------------------------------------------- PathTracerDrv (UT469) ---

# The same device, built against the 469 headers: without DEUSEX, Precomp.h
# defines OLDUNREAL469SDK, which is what the sources test for UT. The flags
# follow the SDK's own CMakeLists for a package: Unicode, the 469 build
# defines, and on 32-bit the old /Zc:wchar_t- ABI. The headers state their
# own packing, and VulkanDrv, the device this one is closest to, is built
# with natural alignment rather than /Zp4.
if(NOT EXISTS "${UT469_SDK}/Engine/Inc/UnRenDev.h")
	message(FATAL_ERROR "No UT 469 SDK at ${UT469_SDK} (set UT469_SDK)")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(UT469_ARCH x64)
	set(UT469_ENGINE_ARCH amd64)
else()
	set(UT469_ARCH x86)
	set(UT469_ENGINE_ARCH x86)
endif()

set(CASE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk-case-compat")
file(MAKE_DIRECTORY "${CASE_COMPAT_DIR}")
if(NOT CMAKE_HOST_WIN32)
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env python3
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/case-compat.py"
		--out "${CASE_COMPAT_DIR}"
		--scan "${UT469_SDK}/Core/Inc" "${UT469_SDK}/Engine/Inc" "${UT469_SDK}/Render/Inc" "${PATHTRACERDRV_DIR}"
		--search "${UT469_SDK}/Core/Inc" "${UT469_SDK}/Engine/Inc" "${UT469_SDK}/Render/Inc"
	COMMAND_ERROR_IS_FATAL ANY)
endif()

add_library(PathTracerDrv SHARED ${PATHTRACERDRV_SOURCES})

# 64-bit, it traces in the game's own process, on its own device: the tracing's
# sources are built in, as the helper builds them - with its stand-ins for the
# engine's macros, since they know nothing of the engine - and TraceClient
# hands them each batch directly (PATHTRACER_LOCAL). 32-bit, it starts the
# helper as Deus Ex's device does.
if(UT469_ARCH STREQUAL "x64")
	target_sources(PathTracerDrv PRIVATE ${PATHTRACER_TRACE_SOURCES})
	set_source_files_properties(${PATHTRACER_TRACE_SOURCES} PROPERTIES COMPILE_DEFINITIONS PATHTRACER_HELPER)
	target_compile_definitions(PathTracerDrv PRIVATE PATHTRACER_LOCAL)
	link_trace_libraries(PathTracerDrv)

	# PathTracerHelperTest's scene and checks, traced the way this device
	# traces: in the harness's own process, on its own device.
	#   cmake --build build-ut469-x64 --target PathTracerLocalTest
	add_executable(PathTracerLocalTest EXCLUDE_FROM_ALL
		${PATHTRACER_TRACE_SOURCES}
		"${PATHTRACERDRV_DIR}/HelperTest.cpp"
		"${PATHTRACERDRV_DIR}/TraceClient.cpp"
		"${PATHTRACERDRV_DIR}/mat.cpp"
		"${PATHTRACERDRV_DIR}/Shaders.cpp")
	target_include_directories(PathTracerLocalTest PRIVATE "${PATHTRACERDRV_DIR}")
	target_compile_definitions(PathTracerLocalTest PRIVATE PATHTRACER_HELPER PATHTRACER_LOCAL WIN32 NDEBUG _CONSOLE)
	target_compile_options(PathTracerLocalTest PRIVATE /MT /EHsc)
	target_link_libraries(PathTracerLocalTest PRIVATE zvulkan user32 advapi32)
	link_trace_libraries(PathTracerLocalTest)
endif()

target_include_directories(PathTracerDrv PRIVATE
	"${PATHTRACERDRV_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty"
	"${UT469_SDK}/Core/Inc"
	"${UT469_SDK}/Engine/Inc"
	"${UT469_SDK}/Render/Inc"
	"${CASE_COMPAT_DIR}")
target_compile_definitions(PathTracerDrv PRIVATE
	WIN32 _WINDOWS _USRDLL NDEBUG _RELEASE RELEASE UNICODE _UNICODE
	UTGLR_UT_BUILD UTGLR_USES_ALPHABLEND=1 ENGINE_ARCHITECTURE="${UT469_ENGINE_ARCH}"
	_CRT_SECURE_NO_WARNINGS _CRT_NO_VA_START_VALIDATION
	# UnX86.h gives MSVC operators on SSE vectors, which clang-cl already has
	# built in and will not have declared again; this is the guard it skips
	# them for.
	__XNAMATHVECTOR_INL__)
if(UT469_ARCH STREQUAL "x64")
	target_compile_definitions(PathTracerDrv PRIVATE WIN64=1)
else()
	target_compile_definitions(PathTracerDrv PRIVATE WINDOWS_IGNORE_PACKING_MISMATCH=1)
	target_compile_options(PathTracerDrv PRIVATE /Zc:wchar_t- /arch:SSE2)
	target_link_options(PathTracerDrv PRIVATE /LARGEADDRESSAWARE)
endif()
target_compile_options(PathTracerDrv PRIVATE /MT /EHa /GR /Zi)
target_link_options(PathTracerDrv PRIVATE /DEBUG)
# UObject's inline operator delete is guarded, and so has a static local
# holding its name, which Core.dll exports - but clang-cl numbers that
# static's scope one more than MSVC did, and asks for a name Core does not
# have. Pointed at the one it does.
if(UT469_ARCH STREQUAL "x64")
	target_link_options(PathTracerDrv PRIVATE
		"/alternatename:__imp_?__FUNC_NAME__@?3???3UObject@@SAXPEAX_K@Z@4QB_WB=__imp_?__FUNC_NAME__@?2???3UObject@@SAXPEAX_K@Z@4QB_WB")
endif()
set_target_properties(PathTracerDrv PROPERTIES PREFIX "" OUTPUT_NAME "PathTracerDrv")
target_link_directories(PathTracerDrv PRIVATE
	"${UT469_SDK}/Core/Lib/${UT469_ARCH}"
	"${UT469_SDK}/Engine/Lib/${UT469_ARCH}"
	"${UT469_SDK}/Render/Lib/${UT469_ARCH}")
target_link_libraries(PathTracerDrv PRIVATE
	"${UT469_SDK}/Core/Lib/${UT469_ARCH}/Core.lib"
	"${UT469_SDK}/Engine/Lib/${UT469_ARCH}/Engine.lib"
	"${UT469_SDK}/Render/Lib/${UT469_ARCH}/Render.lib"
	zvulkan user32 gdi32 advapi32 ole32 shell32 windowscodecs)

elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)

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
# A Windows host resolves those spellings itself, so the whole step - and its
# dependency on python - is only for the cross build.
set(CASE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk-case-compat")
file(MAKE_DIRECTORY "${CASE_COMPAT_DIR}")
if(NOT CMAKE_HOST_WIN32)
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

# The engine's side of the path tracer: the level, the actors, the textures
# and the 2D. The tracing is PathTracerHelper's, built by a 64-bit
# configuration of this project and started from beside this DLL.
add_library(PathTracerDrv SHARED ${PATHTRACERDRV_SOURCES})

target_include_directories(PathTracerDrv PRIVATE "${PATHTRACERDRV_DIR}")
configure_deusex_renderdevice(PathTracerDrv PATHTRACERDRV_EXPORTS)
# ole32 and windowscodecs for photo mode's PNGs (PhotoMode.cpp).
target_link_libraries(PathTracerDrv PRIVATE zvulkan ole32 windowscodecs)

# Drives the helper with a made up scene and writes what it traces to a file,
# so the helper and the channel can be checked without the game.
add_executable(PathTracerHelperTest EXCLUDE_FROM_ALL
	"${PATHTRACERDRV_DIR}/HelperTest.cpp"
	"${PATHTRACERDRV_DIR}/TraceClient.cpp")
target_include_directories(PathTracerHelperTest PRIVATE "${PATHTRACERDRV_DIR}")
target_compile_options(PathTracerHelperTest PRIVATE /MT /EHsc)
target_link_libraries(PathTracerHelperTest PRIVATE zvulkan user32)

endif()

# ------------------------------------------------------------------- spikes ---

# Not built by default: one-question programs about the environment, not part of
# any driver. See spike/README.md.
#   cmake --build build-deusex --target vkrtcheck
add_executable(vkrtcheck EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/spike/vkrtcheck.cpp")
target_include_directories(vkrtcheck PRIVATE "${ZVULKAN_DIR}/include")
target_compile_options(vkrtcheck PRIVATE /MT /EHsc)
set_target_properties(vkrtcheck PROPERTIES OUTPUT_NAME "vkrtcheck")

# Can a 32-bit process show what a 64-bit one ray traced? Built once 32-bit
# (vkxshare.exe, the game's side) and once 64-bit (vkxshare64.exe, the helper
# it starts), from the same file; see spike/README.md.
#   cmake --build build-deusex --target vkxshare
#   cmake --build build-x64 --target vkxshare      # configured with xwin-clang-cl-x64.cmake
add_executable(vkxshare EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/spike/vkxshare.cpp")
target_include_directories(vkxshare PRIVATE "${ZVULKAN_DIR}/include")
target_compile_options(vkxshare PRIVATE /MT /EHsc)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set_target_properties(vkxshare PROPERTIES OUTPUT_NAME "vkxshare64")
else()
	set_target_properties(vkxshare PROPERTIES OUTPUT_NAME "vkxshare")
endif()

# Can the path tracer's 64-bit helper run DLSS Ray Reconstruction? 64-bit only,
# since NGX is, and only once cmake/fetch-dlss.sh has fetched the SDK. The
# Ray Reconstruction runtime is copied beside it, where NGX looks.
#   cmake --build build-x64 --target ngxcheck
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS "${PATHTRACER_DLSS_DIR}/x64/nvsdk_ngx_s.lib")
	add_executable(ngxcheck EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/spike/ngxcheck.cpp")
	target_include_directories(ngxcheck PRIVATE "${PATHTRACER_DLSS_DIR}/include")
	target_compile_definitions(ngxcheck PRIVATE WIN32 NDEBUG _CONSOLE)
	target_compile_options(ngxcheck PRIVATE /MT /EHsc)
	target_link_libraries(ngxcheck PRIVATE zvulkan "${PATHTRACER_DLSS_DIR}/x64/nvsdk_ngx_s.lib" advapi32)
	add_custom_command(TARGET ngxcheck POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PATHTRACER_DLSS_DIR}/nvngx_dlssd.dll" "$<TARGET_FILE_DIR:ngxcheck>")
endif()
