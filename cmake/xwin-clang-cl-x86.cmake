# Cross-compile 32-bit Windows DLLs from Linux with clang-cl + lld-link,
# using the MSVC CRT and Windows SDK unpacked by xwin (see cmake/README-crossbuild.md).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT DEFINED XWIN_DIR)
	set(XWIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.xwin" CACHE PATH "xwin splat output directory")
endif()
get_filename_component(XWIN_DIR "${XWIN_DIR}" ABSOLUTE)

if(NOT EXISTS "${XWIN_DIR}/crt/include")
	message(FATAL_ERROR "No MSVC headers at ${XWIN_DIR}. Run the xwin splat step first (see cmake/README-crossbuild.md).")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_MT llvm-mt)
set(CMAKE_RC_COMPILER llvm-rc)

# clang-cl doesn't infer these from the host, so be explicit.
set(_XWIN_FLAGS "--target=i386-pc-windows-msvc -fuse-ld=lld-link")
foreach(_dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared)
	string(APPEND _XWIN_FLAGS " /imsvc${XWIN_DIR}/${_dir}")
endforeach()

set(CMAKE_C_FLAGS_INIT "${_XWIN_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_XWIN_FLAGS}")

set(_XWIN_LINK_FLAGS "/machine:x86")
foreach(_dir crt/lib/x86 sdk/lib/ucrt/x86 sdk/lib/um/x86)
	string(APPEND _XWIN_LINK_FLAGS " /libpath:${XWIN_DIR}/${_dir}")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# xwin only unpacks the release CRT, so never let a configure check ask for the
# debug one.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
