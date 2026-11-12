cmake_minimum_required(VERSION 3.25)
project(TrueCrimeNYC CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GEN "${REPO}/generated/game")

# the xbe's SHA-1: the executable checks the game it finds is the release it was made from
set(XBE_FILE "${REPO}/assets/default.xbe")
if(NOT EXISTS "${XBE_FILE}")
    message(FATAL_ERROR
        "Missing game image: ${XBE_FILE}\n"
        "The Linux build needs the matching default.xbe to embed its SHA-1. Game assets are "
        "not part of this repository; copy your own assets/default.xbe (and the extracted game "
        "folders beside it) into assets/ before configuring. CI runners must provide it too.")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)

# Dear ImGui: the same release the Windows build uses, for the Graphics menu
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(IMGUI_SRC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
# the package carries libshaderc beside the binary; the rpath makes it win over the host's
set(CMAKE_BUILD_RPATH "$ORIGIN/lib")
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

file(GLOB GEN_SRC "${GEN}/x86_recomp.*.cpp" "${GEN}/x86_func_mapping.cpp")

file(GLOB RT_SRC "${REPO}/sdk/runtime/*/*.cpp")
list(FILTER RT_SRC EXCLUDE REGEX "/app/|/gpu/d3d12\\.cpp$|/gpu/texturedumper\\.cpp$|/audio/xaudio2\\.cpp$")
list(APPEND RT_SRC "${REPO}/sdk/runtime/app/game_files.cpp")

file(GLOB GAME_SRC CONFIGURE_DEPENDS "${REPO}/src/*/*.cpp")
list(FILTER GAME_SRC EXCLUDE REGEX "/linux/app_stub\\.cpp$|/linux/input_stub\\.cpp$|/linux/graphics_menu_stub\\.cpp$")
# The developer debug tools (src/dev/) are Windows-only: they use the
# Win32 key and window APIs and are never part of a Linux build.
list(FILTER GAME_SRC EXCLUDE REGEX "/src/dev/")

set(HOST
    "${CMAKE_CURRENT_SOURCE_DIR}/app_sdl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/settings_linux.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio_sdl3.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backend_sw.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backend_vk.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/texturedumper_linux.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backend_select.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/main_sdl.cpp")

set(HARNESS
    "${REPO}/sdk/harness/reference_renderer.cpp")

# the xbe's SHA-1: the executable checks the game it finds is the release it was made from
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XBE_FILE}")
file(SHA1 "${XBE_FILE}" XBE_SHA1)
set(XBE_HASH "${CMAKE_CURRENT_BINARY_DIR}/TrueCrimeNYC_xbe.cpp")
file(CONFIGURE OUTPUT "${XBE_HASH}" CONTENT "extern const char kImageSha1[] = \"${XBE_SHA1}\";\n")

# AMD FidelityFX FSR 3.1 (SDK 1.1.4, MIT) for the Vulkan temporal upscaler: AMD's sources with their SPIR-V
# permutation headers generated on Windows by scripts/gen_fsr3_vk_shaders.py (sdk/third_party/fsr3_vk)
set(FSR3_VK "${REPO}/sdk/third_party/fsr3_vk")
add_library(ffx_fsr3_vk STATIC
    "${FSR3_VK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${FSR3_VK}/src/backends/vk/ffx_vk.cpp"
    "${FSR3_VK}/src/backends/shared/ffx_shader_blobs.cpp"
    "${FSR3_VK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${FSR3_VK}/src/shared/ffx_assert.cpp"
    "${FSR3_VK}/src/shared/ffx_message.cpp"
    "${FSR3_VK}/src/shared/ffx_object_management.cpp"
    "${FSR3_VK}/src/shared/ffx_breadcrumbs_list.cpp"
    "${FSR3_VK}/ffx_linux_stubs.cpp")
target_include_directories(ffx_fsr3_vk PRIVATE
    "${FSR3_VK}/include/FidelityFX/host" "${FSR3_VK}/src/shared" "${FSR3_VK}/src/components"
    "${FSR3_VK}/src/backends/shared" "${FSR3_VK}/src/backends/shared/blob_accessors" "${FSR3_VK}/generated")
target_include_directories(ffx_fsr3_vk PUBLIC "${FSR3_VK}/include")
target_compile_definitions(ffx_fsr3_vk PRIVATE FFX_FSR3UPSCALER FFX_GCC)
target_compile_options(ffx_fsr3_vk PRIVATE -include "${FSR3_VK}/ffx_linux_compat.h" -O2 -w -fPIC)
target_link_libraries(ffx_fsr3_vk PRIVATE PkgConfig::VULKAN)

add_executable(TrueCrimeNYC ${GEN_SRC} ${RT_SRC} ${GAME_SRC} ${HOST} ${HARNESS} ${IMGUI_SRC} ${XBE_HASH})

target_include_directories(TrueCrimeNYC PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${REPO}/src"
    "${REPO}/sdk/runtime"
    "${GEN}"
    "${REPO}/sdk/harness"
    "${REPO}/sdk/harness/compat"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    ${SDL3_INCLUDE_DIRS})

target_compile_definitions(TrueCrimeNYC PRIVATE XBE_RAW_VERTICES=1 XBE_RENDER_THREAD=1)
# TCNYC_FRAME_POINTERS=ON keeps frame pointers for the sampling profiler's callers (TCNYC_SAMPLE)
option(TCNYC_FRAME_POINTERS "keep frame pointers (profiling)" OFF)
target_compile_options(TrueCrimeNYC PRIVATE -march=x86-64-v2 -fno-strict-aliasing -O2 -g
                       $<$<BOOL:${TCNYC_FRAME_POINTERS}>:-fno-omit-frame-pointer>)

if(TARGET SDL3::SDL3-static)
    set(SDL3_TARGET SDL3::SDL3-static)
else()
    set(SDL3_TARGET SDL3::SDL3)
endif()
target_link_libraries(TrueCrimeNYC PRIVATE ${SDL3_TARGET} PkgConfig::VULKAN PkgConfig::SHADERC ffx_fsr3_vk pthread dl)
set(TCNYC_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK checkout for experimental Vulkan DLSS")
if(TCNYC_DLSS_SDK)
    get_filename_component(DLSS_SDK "${TCNYC_DLSS_SDK}" ABSOLUTE BASE_DIR "${REPO}")
    set(NGX_LIB "${DLSS_SDK}/lib/Linux_x86_64/libnvsdk_ngx.a")
    if(EXISTS "${DLSS_SDK}/include/nvsdk_ngx_helpers_vk.h" AND EXISTS "${NGX_LIB}")
        target_compile_definitions(TrueCrimeNYC PRIVATE TCNYC_HAVE_DLSS=1)
        target_include_directories(TrueCrimeNYC SYSTEM PRIVATE "${DLSS_SDK}/include")
        target_link_libraries(TrueCrimeNYC PRIVATE "${NGX_LIB}" dl)
        message(STATUS "Experimental Vulkan DLSS enabled from ${DLSS_SDK}")
    else()
        message(WARNING "TCNYC_DLSS_SDK is missing Vulkan headers or Linux libnvsdk_ngx.a")
    endif()
endif()
target_link_options(TrueCrimeNYC PRIVATE -static-libstdc++)
