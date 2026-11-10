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
target_compile_options(TrueCrimeNYC PRIVATE -march=x86-64-v2 -fno-strict-aliasing -O2 -g)

if(TARGET SDL3::SDL3-static)
    set(SDL3_TARGET SDL3::SDL3-static)
else()
    set(SDL3_TARGET SDL3::SDL3)
endif()
target_link_libraries(TrueCrimeNYC PRIVATE ${SDL3_TARGET} PkgConfig::VULKAN PkgConfig::SHADERC pthread dl)
target_link_options(TrueCrimeNYC PRIVATE -static-libstdc++)
