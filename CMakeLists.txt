cmake_minimum_required(VERSION 3.25)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FEVERSCALER_VERSION)
string(STRIP "${FEVERSCALER_VERSION}" FEVERSCALER_VERSION)
if(NOT FEVERSCALER_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[A-Za-z0-9.-]+)?$")
  message(FATAL_ERROR "VERSION must contain a semantic version")
endif()
project(feverscaler VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}" LANGUAGES C CXX RC)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set(FEVERSCALER_FILE_FLAGS "0x0L")
if(FEVERSCALER_VERSION MATCHES "-")
  set(FEVERSCALER_FILE_FLAGS "VS_FF_PRERELEASE")
endif()
configure_file(src/version.h.in generated/feverscaler_version.h @ONLY)
configure_file(src/version.rc.in generated/feverscaler_version.rc @ONLY)
file(CONFIGURE OUTPUT generated/VERSION.txt CONTENT "${FEVERSCALER_VERSION}\n" NEWLINE_STYLE UNIX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Vulkan SDK installer sets VULKAN_SDK.
set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
find_program(GLSLC glslc HINTS "${VULKAN_SDK_DIR}/Bin" REQUIRED)

# ---- pinned dependencies, downloaded at configure time into build/_deps -------------------
# SOURCE_SUBDIR none: download only, without adding the dependency's own CMakeLists.txt.
include(FetchContent)
FetchContent_Declare(streamline
  URL https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip
  URL_HASH SHA256=f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48
  SOURCE_SUBDIR none)
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/8af6b4acae5a9388fd742b56fa79ece89d96f823.zip
  URL_HASH SHA256=b54d9fb9dc6e067e9b826f8cae9dab2212f98125d80c4f8e0ba239e5e6895025
  SOURCE_SUBDIR none)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
  URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8)
FetchContent_Declare(asiloader
  URL https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader-NoPDB_x64.zip
  URL_HASH SHA256=e5860e7d9a1805267535b65749575b5e406cc6ea3325c7392189c578815045d1)
# RTX30MFG-Unlock v1.2.2 with FeverScaler's patch, which changes only its disk logging.
FetchContent_Declare(rtx30mfg
  URL https://github.com/mcsoderh/RTX30MFG-Unlock/archive/21a2b9931f0c13f46a4b3b8a5856620d9698f88e.zip
  URL_HASH SHA256=e199238d31039d1b22484f3afac87be7b0b4e146127b268b0f68c24efa17d831
  PATCH_COMMAND git apply "${CMAKE_SOURCE_DIR}/patches/rtx30mfg-compact-logging.patch"
  SOURCE_SUBDIR source/native)
FetchContent_MakeAvailable(streamline minhook imgui asiloader)
# The unlock keeps its upstream build settings (dynamic CRT), so it is added before
# CMAKE_MSVC_RUNTIME_LIBRARY is set.
set(STREAMLINE_ROOT "${streamline_SOURCE_DIR}")
set(MFG_UNLOCK_BUILD_UNIVERSAL_UI OFF)
FetchContent_MakeAvailable(rtx30mfg)

# Static CRT: the plugin must not depend on the VC runtime the game happens to ship.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook (static), built from its sources so its own install rules stay out of the packages.
add_library(minhook STATIC
  "${minhook_SOURCE_DIR}/src/buffer.c"
  "${minhook_SOURCE_DIR}/src/hook.c"
  "${minhook_SOURCE_DIR}/src/trampoline.c"
  "${minhook_SOURCE_DIR}/src/hde/hde64.c")
target_include_directories(minhook PUBLIC "${minhook_SOURCE_DIR}/include")

# ---- shaders: GLSL -> SPIR-V -> C initializer lists ---------------------------------
set(SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
set(SHADERS camera_mv.comp replay_mesh.vert replay_skinned.vert replay.frag)
set(SHADER_HEADERS)
foreach(S ${SHADERS})
  set(OUT "${SHADER_OUT_DIR}/${S}.spv.h")
  add_custom_command(
    OUTPUT "${OUT}"
    COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=c -o "${OUT}" "${SHADER_SRC_DIR}/${S}"
    DEPENDS "${SHADER_SRC_DIR}/${S}" "${SHADER_SRC_DIR}/common.glsl"
    COMMENT "glslc ${S}")
  list(APPEND SHADER_HEADERS "${OUT}")
endforeach()
add_custom_target(feverscaler_shaders DEPENDS ${SHADER_HEADERS})

# ---- Dear ImGui (dev menu): core + Win32 input + Vulkan renderer, functions loaded at runtime --
set(IMGUI_DIR "${imgui_SOURCE_DIR}")
add_library(imgui STATIC
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${VULKAN_SDK_DIR}/Include")
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES
  IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---- the plugin (.asi, loaded by Ultimate ASI Loader) --------------------------------
add_library(feverscaler SHARED
  "${CMAKE_BINARY_DIR}/generated/feverscaler_version.rc"
  src/dllmain.cpp
  src/log.cpp
  src/diagnostics.cpp
  src/config.cpp
  src/hooks.cpp
  src/sl_bridge.cpp
  src/spirv.cpp
  src/spirv_patch.cpp
  src/tracker.cpp
  src/gpu.cpp
  src/frame.cpp
  src/mip_bias.cpp
  src/motion_match.cpp
  src/overlay.cpp
  src/game_scale.cpp
  src/game_settings.cpp
  src/bridge.cpp
  src/vram.cpp
  src/vkmath.cpp)
add_dependencies(feverscaler feverscaler_shaders)
target_include_directories(feverscaler PRIVATE
  src
  "${CMAKE_BINARY_DIR}/generated"
  "${SHADER_OUT_DIR}"
  "${VULKAN_SDK_DIR}/Include"
  "${streamline_SOURCE_DIR}/include")
target_compile_definitions(feverscaler PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler PRIVATE /W3 /EHsc /utf-8)
target_link_libraries(feverscaler PRIVATE minhook imgui user32 dxgi)
set_target_properties(feverscaler PROPERTIES SUFFIX ".asi" OUTPUT_NAME "feverscaler")
target_link_options(feverscaler PRIVATE "/MAP:${CMAKE_BINARY_DIR}/feverscaler.map")

# ---- tests (no GPU or game needed) ---------------------------------------------------
enable_testing()
add_executable(feverscaler_fg_test tests/fg_test.cpp src/diagnostics.cpp)
target_include_directories(feverscaler_fg_test PRIVATE src "${VULKAN_SDK_DIR}/Include"
  "${streamline_SOURCE_DIR}/include")
target_compile_definitions(feverscaler_fg_test PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler_fg_test PRIVATE /W3 /EHsc /utf-8)
add_test(NAME frame_generation COMMAND feverscaler_fg_test)
add_executable(feverscaler_logging_test tests/logging_test.cpp src/log.cpp src/diagnostics.cpp)
target_include_directories(feverscaler_logging_test PRIVATE src "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(feverscaler_logging_test PRIVATE FEVERSCALER_TESTS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler_logging_test PRIVATE /W3 /EHsc /utf-8)
add_test(NAME bounded_diagnostics COMMAND feverscaler_logging_test)
add_executable(feverscaler_unlock_logging_test tests/unlock_logging_test.cpp)
target_include_directories(feverscaler_unlock_logging_test PRIVATE
  "${rtx30mfg_SOURCE_DIR}/source/native"
  "${streamline_SOURCE_DIR}/external/ngx-sdk/include")
target_compile_definitions(feverscaler_unlock_logging_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler_unlock_logging_test PRIVATE /W3 /EHsc /utf-8)
add_test(NAME unlock_counters_without_csv COMMAND feverscaler_unlock_logging_test)
add_test(NAME unlock_trace_size_limit COMMAND feverscaler_unlock_logging_test trace)
add_executable(feverscaler_motion_test tests/motion_match_test.cpp src/motion_match.cpp)
target_include_directories(feverscaler_motion_test PRIVATE src)
target_compile_options(feverscaler_motion_test PRIVATE /W3 /EHsc /utf-8)
add_test(NAME motion_history COMMAND feverscaler_motion_test)
add_executable(feverscaler_render_test tests/render_test.cpp src/diagnostics.cpp src/mip_bias.cpp
  src/motion_match.cpp src/vkmath.cpp src/spirv.cpp src/spirv_patch.cpp)
target_include_directories(feverscaler_render_test PRIVATE src "${VULKAN_SDK_DIR}/Include"
  "${streamline_SOURCE_DIR}/include")
target_compile_definitions(feverscaler_render_test PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler_render_test PRIVATE /W3 /EHsc /utf-8)
target_link_libraries(feverscaler_render_test PRIVATE minhook user32)
add_test(NAME render_safety COMMAND feverscaler_render_test)
add_executable(feverscaler_bridge_test tests/bridge_test.cpp)
target_include_directories(feverscaler_bridge_test PRIVATE src "${VULKAN_SDK_DIR}/Include")
target_compile_definitions(feverscaler_bridge_test PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(feverscaler_bridge_test PRIVATE /W3 /EHsc /utf-8)
target_link_libraries(feverscaler_bridge_test PRIVATE minhook)
add_test(NAME settings_bridge COMMAND feverscaler_bridge_test)

# ---- install and package ---------------------------------------------------------------
# The drop-in layout, whose root is the game folder (the one with TransportFever3.exe). Adding or
# removing a file here also means updating the layout list in tests/package_test.ps1 and the
# removal list in docs/INSTALL.txt.
set(OWN scripts/feverscaler)
install(FILES "$<TARGET_FILE:feverscaler>" DESTINATION scripts)
install(FILES "$<TARGET_FILE:RTX40MFGAuto>" "$<TARGET_FILE:RTX40MFGCore>" DESTINATION .)
install(FILES "${asiloader_SOURCE_DIR}/dinput8.dll" DESTINATION . RENAME winhttp.dll)
# Only the load point (plugins load from the loader's DllMain). The loader's other defaults stay:
# root .asi files (the unlock) load before scripts\, and other mods' plugins keep loading.
file(CONFIGURE OUTPUT generated/winhttp.ini CONTENT "[GlobalSets]\nDontLoadFromDllMain=0\n" NEWLINE_STYLE CRLF)
install(FILES "${CMAKE_BINARY_DIR}/generated/winhttp.ini" DESTINATION .)
install(FILES docs/INSTALL.txt DESTINATION . RENAME FeverScaler-README.txt)
set(STREAMLINE_DLLS)
foreach(DLL sl.interposer sl.common sl.dlss_g sl.dlss nvngx_dlss sl.reflex sl.pcl nvngx_dlssg NvLowLatencyVk)
  list(APPEND STREAMLINE_DLLS "${streamline_SOURCE_DIR}/bin/x64/${DLL}.dll")
endforeach()
install(FILES ${STREAMLINE_DLLS} DESTINATION ${OWN}/Streamline)
install(DIRECTORY settings/ DESTINATION ${OWN}/settings)
install(DIRECTORY licenses/ DESTINATION ${OWN}/licenses)
install(FILES patches/rtx30mfg-compact-logging.patch DESTINATION ${OWN}/licenses)
install(FILES LICENSE DESTINATION ${OWN} RENAME LICENSE.txt)
install(FILES docs/diagnostics.txt DESTINATION ${OWN} RENAME DIAGNOSTICS.txt)
install(FILES THIRD_PARTY_NOTICES.txt "${CMAKE_BINARY_DIR}/generated/VERSION.txt"
  DESTINATION ${OWN})

set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_FILE_NAME "FeverScaler-v${FEVERSCALER_VERSION}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
include(CPack)
