# True Crime: New York City (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is vendored in this repository. Keep sdk/ in the tree rather than
# falling back to a separate upstream checkout.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    message(FATAL_ERROR "The vendored sdk/ tree is missing; restore it from the repository root.")
endif()
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
# A MinGW-target clang (e.g. llvm-mingw first on PATH) builds and runs here, but its executable needs
# libc++.dll and libunwind.dll and its bundled clang.exe is a launcher: release packages must be
# built with the MSVC-target clang that build.bat selects. scripts/package-release.ps1 refuses them.
if(MINGW)
    # The presets name the compiler as plain clang/clang++, so a configure (or a regenerate after a
    # CMakeLists.txt change) in a shell with llvm-mingw first on PATH picks it up. On a regenerate
    # CMake then wipes the cache and the preset's CMAKE_BUILD_TYPE with it, leaving an unoptimised
    # MinGW build in the release folder. Stop instead: a Release (or untyped) MinGW build is that.
    if((CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE) AND NOT TCNYC_ALLOW_MINGW_RELEASE)
        message(FATAL_ERROR
            "A MinGW-target clang (${CMAKE_CXX_COMPILER}) was picked for build type "
            "'${CMAKE_BUILD_TYPE}'. Release builds need the "
            "MSVC-target clang: run build.bat, or put C:/Program Files/LLVM/bin first on PATH, delete "
            "${CMAKE_BINARY_DIR}/CMakeCache.txt and configure again. To build with MinGW anyway, pass "
            "-DTCNYC_ALLOW_MINGW_RELEASE=ON.")
    endif()
    message(WARNING "Configured with a MinGW-target clang (${CMAKE_CXX_COMPILER}). Fine for local builds; "
                    "release packages need the MSVC-target clang from Visual Studio or C:/Program Files/LLVM.")
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

# Host regression tests need the SDK's fetched dependencies (SDL3, toml++) but
# not the player's own XBE. CI uses this to run ctest on machines without game
# assets, so a missing XBE cannot take the tests out of the gate.
option(TCNYC_TESTS_ONLY "Configure only the host regression tests" OFF)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# Developer debug tools (src/dev/): off unless a dev build asks;
# release builds do not compile src/dev/ at all
option(TCNYC_DEV_TOOLS "Build the developer debug tools" OFF)
if(NOT TCNYC_DEV_TOOLS)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/dev/")
endif()

# The European (PAL) release is a separate XBE with its own layout. It builds
# from assets-pal/default.xbe and config/game-pal.toml into its own target.
# Use the pal preset to keep the generated code and build output separate.
option(TCNYC_PAL "Build for the European (PAL) XBE" OFF)
option(TCNYC_PAL_EU "Build for the second European PAL disc revision" OFF)
if(TCNYC_PAL_EU)
    set(TCNYC_PAL ON)
    set(GAME_TARGET TrueCrimeNYCPALEU)
    set(GAME_XBE assets-pal-eu/default.xbe)
    set(GAME_CONFIG game-pal-eu.toml)
    set(GAME_DIR game-pal-eu)
elseif(TCNYC_PAL)
    set(GAME_TARGET TrueCrimeNYCPAL)
    set(GAME_XBE assets-pal/default.xbe)
    set(GAME_CONFIG game-pal.toml)
    set(GAME_DIR game-pal)
else()
    set(GAME_TARGET TrueCrimeNYC)
    set(GAME_XBE default.xbe)
    set(GAME_CONFIG game.toml)
    set(GAME_DIR game)
endif()

# partitions as xbe_symbols wrote them into the config
set(GAME_CONFIG_PATH ${CMAKE_CURRENT_SOURCE_DIR}/config/${GAME_CONFIG})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAME_CONFIG_PATH})
if(EXISTS ${GAME_CONFIG_PATH})
    file(STRINGS ${GAME_CONFIG_PATH} GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
set(TCNYC_REPO "${CMAKE_CURRENT_SOURCE_DIR}")
set(TCNYC_TEST_SDL SDL3::SDL3-static)
if(NOT TCNYC_TESTS_ONLY)
if(TCNYC_PAL_EU)
    include(sdk/cmake/CheckPalEu.cmake)
    tcnyc_check_pal_eu("${CMAKE_CURRENT_SOURCE_DIR}/${GAME_XBE}")
endif()
add_xbe_image(${GAME_TARGET} ${GAME_XBE} ${GAME_CONFIG} ${GAME_PARTITIONS} ${GAME_DIR} game)
if(TCNYC_DEV_TOOLS)
    target_compile_definitions(${GAME_TARGET} PRIVATE TCNYC_DEV_TOOLS=1)
endif()
if(TCNYC_PAL)
    target_compile_definitions(${GAME_TARGET} PRIVATE TCNYC_PAL=1)
    target_compile_definitions(grid_patch PRIVATE TCNYC_PAL=1)
    add_custom_target(recompile_TrueCrimeNYC DEPENDS recompile_${GAME_TARGET})
    if(TCNYC_PAL_EU)
        target_compile_definitions(${GAME_TARGET} PRIVATE TCNYC_PAL_EU=1)
        target_compile_definitions(grid_patch PRIVATE TCNYC_PAL_EU=1)
    endif()
endif()

# Optional NVIDIA DLSS SDK. The headers are proprietary, so they are not vendored: point this at a
# local NVIDIA/DLSS checkout (for example tools/DLSS) to compile the DLSS backend. Without it the
# game builds and behaves exactly as before.
set(TCNYC_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK checkout containing include/nvsdk_ngx.h")
set(TCNYC_DLSS_VK_SDK "" CACHE PATH
    "NVIDIA Windows NGX SDK for Vulkan DLSS (include/nvsdk_ngx_helpers_vk.h and lib/Windows_x86_64/rel/nvsdk_ngx.lib)")
if(TCNYC_DLSS_SDK)
    get_filename_component(TCNYC_DLSS_SDK_ABS "${TCNYC_DLSS_SDK}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(EXISTS "${TCNYC_DLSS_SDK_ABS}/include/nvsdk_ngx.h")
        target_compile_definitions(${GAME_TARGET} PRIVATE TCNYC_HAVE_DLSS=1)
        target_include_directories(${GAME_TARGET} PRIVATE "${TCNYC_DLSS_SDK_ABS}/include")
        message(STATUS "DLSS support enabled from ${TCNYC_DLSS_SDK_ABS}")
    else()
        message(WARNING "TCNYC_DLSS_SDK does not contain include/nvsdk_ngx.h; DLSS is disabled")
    endif()
else()
    message(STATUS "DLSS support disabled (set TCNYC_DLSS_SDK to an NVIDIA/DLSS checkout)")
endif()

# The app uses the per-monitor DPI and dark-mode era APIs; current llvm-mingw still
# defaults WINVER below Windows 10, which hides their declarations.
target_compile_definitions(${GAME_TARGET} PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)

# Keep the SDK startup intact and invoke it after the launcher closes.
set_property(SOURCE "${SDK}/runtime/app/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS wWinMain=RecompMain)
include(launcher/launcher.cmake)
tcnyc_add_launcher(${GAME_TARGET})
target_sources(${GAME_TARGET} PRIVATE launcher/windows_main.cpp launcher/version.rc
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
# Windows still uses ImGui 1.91, whose SDL3 backends refer to the SDL 3.1 names.
set_property(SOURCE
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS SDL_ENABLE_OLD_NAMES)
target_link_libraries(${GAME_TARGET} PRIVATE SDL3::SDL3-static)
set_property(SOURCE launcher/version.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.ico")
# Embed the long-path manifest. lld-link only accepts /MANIFESTINPUT with /manifest:embed; a
# MinGW-target link would hand that to mt.exe, so those builds carry it as a resource instead.
if(MINGW)
    target_sources(${GAME_TARGET} PRIVATE launcher/manifest.rc)
    set_property(SOURCE launcher/manifest.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.manifest")
else()
    target_link_options(${GAME_TARGET} PRIVATE
        "-Wl,/manifest:embed"
        "-Wl,/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.manifest")
endif()
target_link_libraries(${GAME_TARGET} PRIVATE shell32 gdi32 user32)
endif()  # NOT TCNYC_TESTS_ONLY

include(tests/HostTests.cmake)
