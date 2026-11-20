cmake_minimum_required(VERSION 3.20)
project(uncharted_uwp LANGUAGES C CXX)
option(UNCHARTED_BUILD_GRAPHICS "Build the existing renderer for UWP" OFF)
option(UNCHARTED_LINK_RENDERER "Connect the existing renderer to the UWP loader" OFF)
option(UNCHARTED_BUILD_GUEST "Enable the controlled guest-memory/init/entry modes" OFF)
option(UNCHARTED_M5_VIDEO_CONTRACT "Enable CPU-only logical VideoOut contract, not GPU frames" OFF)
option(UNCHARTED_M5_HEADLESS_INSTANCE "Link renderer Vulkan instance factory for explicit M5 smoke" OFF)
option(UNCHARTED_M5_HEADLESS_DEVICE "Enable isolated renderer logical-device smoke" OFF)
set(UNCHARTED_TLS_PAD_KB 0 CACHE STRING "Static TLS KiB added to the UWP image (TLS limit test)")
if(UNCHARTED_M5_HEADLESS_DEVICE AND NOT UNCHARTED_M5_HEADLESS_INSTANCE)
  message(FATAL_ERROR "M5 renderer device smoke requires the headless instance mode")
endif()
if(UNCHARTED_M5_HEADLESS_INSTANCE AND NOT UNCHARTED_M5_VIDEO_CONTRACT)
  message(FATAL_ERROR "M5 renderer instance smoke requires the M5 VideoOut contract mode")
endif()
if(UNCHARTED_M5_VIDEO_CONTRACT AND NOT UNCHARTED_BUILD_GUEST)
  message(FATAL_ERROR "M5 VideoOut contract requires UNCHARTED_BUILD_GUEST")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang-cl: guest code uses the System V x64 ABI")
endif()
set(UNCHARTED_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(UNCHARTED_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.local/deps" CACHE PATH "Pinned third-party dependencies")
set(UNCHARTED_PTHREAD "${UNCHARTED_DEPS_ROOT}/winpthreads" CACHE PATH "UCRT winpthread headers and import library")
add_compile_definitions(_MODE_T_)
# AppContainer builds must resolve the CRT from the Store import libraries (MSVCP140_APP etc).
if(DEFINED ENV{VCToolsInstallDir} AND EXISTS "$ENV{VCToolsInstallDir}lib/x64/store/msvcprt.lib")
  link_directories(BEFORE "$ENV{VCToolsInstallDir}lib/x64/store")
endif()

set(UNCHARTED_UWP_SOURCES
  runtime runtime_audio runtime_content runtime_display runtime_fiber runtime_file
  runtime_kernel runtime_memory runtime_mutex runtime_rtc runtime_rwlock runtime_savedata
  runtime_sema runtime_services runtime_thread runtime_time runtime_trophy
  prepare_boot compat_win cpu_patch red_zone probe)
list(TRANSFORM UNCHARTED_UWP_SOURCES PREPEND "${UNCHARTED_ROOT}/src/")
list(TRANSFORM UNCHARTED_UWP_SOURCES APPEND ".c")

# The runtime C sources live in a static library so both the executable and the renderer DLL
# link the same implementation.
add_library(bbruntime STATIC platform/runtime_compat.c platform/milestone_services.c
  ${UNCHARTED_UWP_SOURCES})
target_include_directories(bbruntime PUBLIC "${UNCHARTED_ROOT}" "${UNCHARTED_ROOT}/src" platform
  platform/compat "${UNCHARTED_PTHREAD}/include" "${UNCHARTED_DEPS_ROOT}/sdl/include"
  "${UNCHARTED_DEPS_ROOT}/zydis/include"
  "${UNCHARTED_DEPS_ROOT}/zycore/include")
target_compile_definitions(bbruntime PUBLIC UNCHARTED_UWP NOMINMAX WIN32_LEAN_AND_MEAN)

target_compile_definitions(bbruntime PUBLIC _CRT_SECURE_NO_WARNINGS)
target_compile_options(bbruntime PRIVATE /W3
  $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att>)

add_executable(uncharted_uwp WIN32 platform/min_host.cpp platform/uwp_core_window.cpp platform/uwp_app_memory_stub.cpp)
target_link_libraries(uncharted_uwp PRIVATE bbruntime)
target_include_directories(uncharted_uwp PRIVATE
  "${UNCHARTED_ROOT}" "${UNCHARTED_ROOT}/src" platform platform/compat
  "${UNCHARTED_PTHREAD}/include" "${UNCHARTED_DEPS_ROOT}/zydis/include"
  "${UNCHARTED_DEPS_ROOT}/zycore/include")
target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_UWP NOMINMAX WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS _TIMESPEC_DEFINED)
if(UNCHARTED_BUILD_GUEST)
  target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_BUILD_GUEST)
endif()
target_compile_options(uncharted_uwp PRIVATE /W3
  $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att>
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_link_options(uncharted_uwp PRIVATE /APPCONTAINER /HIGHENTROPYVA:NO
  /STACK:16777216,16777216 /OPT:REF)
target_link_directories(uncharted_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
set(ZYAN_ZYCORE_PATH "${UNCHARTED_DEPS_ROOT}/zycore" CACHE PATH "" FORCE)
foreach(option ZYDIS_BUILD_SHARED_LIB ZYDIS_BUILD_EXAMPLES ZYDIS_BUILD_TOOLS ZYDIS_BUILD_TESTS
               ZYDIS_BUILD_MAN ZYDIS_BUILD_DOXYGEN ZYCORE_BUILD_SHARED_LIB ZYCORE_BUILD_EXAMPLES
               ZYCORE_BUILD_TESTS)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(zyan_saved_c "${CMAKE_C_STANDARD_LIBRARIES}")
set(zyan_saved_cxx "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES_INIT}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES_INIT}")
add_subdirectory("${UNCHARTED_DEPS_ROOT}/zydis" "${CMAKE_BINARY_DIR}/deps/zydis" EXCLUDE_FROM_ALL)
set(CMAKE_C_STANDARD_LIBRARIES "${zyan_saved_c}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${zyan_saved_cxx}")
target_link_libraries(uncharted_uwp PRIVATE WindowsApp
  "${UNCHARTED_PTHREAD}/lib/libwinpthread.dll.a" Zydis)

set(UNCHARTED_LAYOUT "${CMAKE_BINARY_DIR}/layout")
# The layout must exist before any POST_BUILD copy; otherwise a clean build creates a *file*
# named "layout" and the later directory commands fail.
file(MAKE_DIRECTORY "${UNCHARTED_LAYOUT}")
if(UNCHARTED_BUILD_GRAPHICS)
  add_compile_options(/clang:-Wno-unused-command-line-argument)
  include(graphics/dependencies.cmake)
  # The M2 probe/present bridge lives in bbgpu_dll.dll (see below); the executable is only a host
# and must not pull renderer/vulkan-hpp code, or its globals are duplicated across modules.
# target_sources(uncharted_uwp PRIVATE graphics/probe.cpp graphics/present_bridge.cpp)
  target_include_directories(uncharted_uwp PRIVATE "${UNCHARTED_ROOT}/gpu/shim" "${UNCHARTED_ROOT}/gpu/shadps4" graphics)
  target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_BUILD_GRAPHICS VK_USE_PLATFORM_WIN32_KHR)
  target_link_libraries(uncharted_uwp PRIVATE Vulkan::Headers d3d12 dxgi)
  if(UNCHARTED_LINK_RENDERER)
    set(UNCHARTED_UWP ON)
    set(BB_LTO OFF CACHE BOOL "" FORCE)
    add_subdirectory("${UNCHARTED_ROOT}/gpu" "${CMAKE_BINARY_DIR}/gpu")
    target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_LINK_RENDERER)
    target_compile_definitions(bbgpu PRIVATE UNCHARTED_UWP VK_USE_PLATFORM_WIN32_KHR)
    if(UNCHARTED_M5_VIDEO_CONTRACT)
      target_compile_definitions(bbgpu PRIVATE UNCHARTED_M5_VIDEO_CONTRACT)
      target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_M5_VIDEO_CONTRACT)
    endif()
    if(UNCHARTED_M5_HEADLESS_INSTANCE)
      target_compile_definitions(bbgpu PRIVATE UNCHARTED_M5_HEADLESS_INSTANCE)
      target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_M5_HEADLESS_INSTANCE)
    endif()
    if(UNCHARTED_M5_HEADLESS_DEVICE)
      target_compile_definitions(bbgpu PRIVATE UNCHARTED_M5_HEADLESS_DEVICE)
      target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_M5_HEADLESS_DEVICE)
    endif()
    # Single module: renderer/HLE/runtime live in bbgpu_dll.dll (exports in platform/bbgpu_exports.def).
    # The executable must NOT link bbgpu: doing so duplicated every global (Vulkan dispatcher, VMA,
    # Vram, MemoryManager) in two modules, and the DLL then called a null Vram::allows / a stale
    # allocator - the "execute at 0" crashes at varying points.
    set_property(TARGET bbgpu PROPERTY INTERFACE_LINK_LIBRARIES "")
    # Renderer DLL: built and packaged beside the static build so the console can prove that
    # the same code runs when loaded from a DLL. The executable keeps the static path for now.
    add_library(bbgpu_dll SHARED platform/bbgpu_exports.def platform/bbgpu_dll_anchor.cpp platform/bbgpu_dll_probe.cpp
      platform/milestone_services_guest.c
      platform/renderer_smoke.cpp platform/main.cpp platform/uwp_core_window.cpp
      ${CMAKE_CURRENT_LIST_DIR}/graphics/probe.cpp
      ${CMAKE_CURRENT_LIST_DIR}/graphics/present_bridge.cpp
      ${CMAKE_CURRENT_LIST_DIR}/graphics/swapchain.cpp
      ${CMAKE_CURRENT_LIST_DIR}/graphics/window.cpp)
    target_link_libraries(bbgpu_dll PRIVATE bbgpu bbruntime WindowsApp d3d12 dxgi Vulkan::Headers
      fmt::fmt miniz::miniz sirit PkgConfig::XXHASH PkgConfig::FFMPEG Zydis Boost::headers SDL3::SDL3-static
      $<TARGET_NAME_IF_EXISTS:magic_enum::magic_enum> tsl::robin_map GPUOpen::VulkanMemoryAllocator
      "${UNCHARTED_PTHREAD}/lib/libwinpthread.dll.a")
    target_include_directories(bbgpu_dll PRIVATE "${CMAKE_CURRENT_LIST_DIR}/graphics"
      "${UNCHARTED_ROOT}/gpu/shim" "${UNCHARTED_ROOT}/gpu/shadps4" "${UNCHARTED_ROOT}/gpu"
      "${UNCHARTED_ROOT}/gpu/third_party/gcn/include" "${UNCHARTED_ROOT}/gpu/third_party/half/include")
    target_compile_definitions(bbgpu_dll PRIVATE VK_USE_PLATFORM_WIN32_KHR UNCHARTED_UWP
      UNCHARTED_M5_VIDEO_CONTRACT UNCHARTED_M5_HEADLESS_INSTANCE UNCHARTED_M5_HEADLESS_DEVICE
      UNCHARTED_BUILD_GRAPHICS UNCHARTED_LINK_RENDERER UNCHARTED_BUILD_GUEST)
    set_target_properties(bbgpu_dll PROPERTIES PREFIX "")
    # Indirect calls into the packaged ICD crash when issued from this module; disable the
    # control-flow guards for the diagnostic build so a CET/CFG check cannot be the cause.
    target_compile_options(bbgpu_dll PRIVATE "/guard:cf-" $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    target_link_options(bbgpu_dll PRIVATE "/APPCONTAINER")
    target_link_options(bbgpu_dll PRIVATE "/CETCOMPAT:NO")
    target_compile_options(bbgpu_dll PRIVATE "/FI${UNCHARTED_ROOT}/xbox/graphics/compat.h")
    target_include_directories(bbgpu_dll PRIVATE "${UNCHARTED_ROOT}/xbox/platform"
      "${UNCHARTED_ROOT}/xbox/platform/compat")
    if(DEFINED ENV{VCToolsInstallDir})
      set(BBGPU_DLL_STORE "$ENV{VCToolsInstallDir}lib/x64/store")
      if(EXISTS "${BBGPU_DLL_STORE}/msvcprt.lib")
        target_link_options(bbgpu_dll PRIVATE "/APPCONTAINER" "/DEF:${CMAKE_CURRENT_LIST_DIR}/platform/bbgpu_exports.def"
          "/NODEFAULTLIB:msvcprt" "/NODEFAULTLIB:vcruntime")
        target_link_libraries(bbgpu_dll PRIVATE "${BBGPU_DLL_STORE}/msvcprt.lib"
          "${BBGPU_DLL_STORE}/vcruntime.lib")
      endif()
    endif()
    # Attach the copy to bbgpu_dll itself: attaching it to the executable meant that a rebuild of the
    # renderer alone never refreshed the layout, so packages shipped the previous DLL.
    add_custom_command(TARGET bbgpu_dll POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bbgpu_dll>" "${UNCHARTED_LAYOUT}")
    if(UNCHARTED_BUILD_GUEST)
      # Event queues use fmt logging; keep desktop renderer/media deps out of the app link.
      target_link_libraries(uncharted_uwp PRIVATE fmt::fmt)
    endif()
    if(UNCHARTED_M5_HEADLESS_DEVICE)
      # Pulling the host passes and the texture cache into the console link reaches miniz,
      # the SPIR-V assembler and xxhash; they are static and AppContainer-safe.
      target_link_libraries(uncharted_uwp PRIVATE fmt::fmt miniz::miniz sirit PkgConfig::XXHASH)
    endif()
    # The console AppContainer must never import the desktop MSVCP140.dll.
    if(DEFINED ENV{VCToolsInstallDir})
      set(UNCHARTED_STORE_CRT "$ENV{VCToolsInstallDir}lib/x64/store")
      if(EXISTS "${UNCHARTED_STORE_CRT}/msvcprt.lib")
        target_link_options(uncharted_uwp PRIVATE "/NODEFAULTLIB:msvcprt")
        target_link_libraries(uncharted_uwp PRIVATE "${UNCHARTED_STORE_CRT}/msvcprt.lib")
      endif()
    endif()
    if(UNCHARTED_TLS_PAD_KB GREATER 0)
      math(EXPR UNCHARTED_TLS_PAD_BYTES "${UNCHARTED_TLS_PAD_KB} * 1024")
      target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_TLS_PAD_BYTES=${UNCHARTED_TLS_PAD_BYTES})
    endif()
  endif()

  if(UNCHARTED_DOZEN_DLL)
    add_custom_command(TARGET uncharted_uwp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DOZEN_DLL}" "${UNCHARTED_LAYOUT}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DXIL_DLL}" "${UNCHARTED_LAYOUT}")
  endif()
  add_custom_command(TARGET uncharted_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UNCHARTED_LAYOUT}/Assets" "${UNCHARTED_LAYOUT}/ThirdParty"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:uncharted_uwp>" "${UNCHARTED_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DEPS_ROOT}/libwinpthread/bin/libwinpthread-1.dll" "${UNCHARTED_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_PTHREAD}/share/licenses/winpthreads/COPYING" "${UNCHARTED_LAYOUT}/ThirdParty/winpthreads-COPYING.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_ROOT}/LICENSE" "${UNCHARTED_LAYOUT}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md" "${UNCHARTED_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.xml" "${UNCHARTED_LAYOUT}")
endif()
