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

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# Developer debug tools (src/dev/): off unless a dev build asks;
# release builds do not compile src/dev/ at all
option(TCNYC_DEV_TOOLS "Build the developer debug tools" OFF)
if(NOT TCNYC_DEV_TOOLS)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/dev/")
endif()

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(TrueCrimeNYC default.xbe game.toml ${GAME_PARTITIONS} game)
if(TCNYC_DEV_TOOLS)
    target_compile_definitions(TrueCrimeNYC PRIVATE TCNYC_DEV_TOOLS=1)
endif()

# Optional NVIDIA DLSS SDK. The headers are proprietary, so they are not vendored: point this at a
# local NVIDIA/DLSS checkout (for example tools/DLSS) to compile the DLSS backend. Without it the
# game builds and behaves exactly as before.
set(TCNYC_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK checkout containing include/nvsdk_ngx.h")
if(TCNYC_DLSS_SDK)
    get_filename_component(TCNYC_DLSS_SDK_ABS "${TCNYC_DLSS_SDK}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(EXISTS "${TCNYC_DLSS_SDK_ABS}/include/nvsdk_ngx.h")
        target_compile_definitions(TrueCrimeNYC PRIVATE TCNYC_HAVE_DLSS=1)
        target_include_directories(TrueCrimeNYC PRIVATE "${TCNYC_DLSS_SDK_ABS}/include")
        message(STATUS "DLSS support enabled from ${TCNYC_DLSS_SDK_ABS}")
    else()
        message(WARNING "TCNYC_DLSS_SDK does not contain include/nvsdk_ngx.h; DLSS is disabled")
    endif()
else()
    message(STATUS "DLSS support disabled (set TCNYC_DLSS_SDK to an NVIDIA/DLSS checkout)")
endif()

# The app uses the per-monitor DPI and dark-mode era APIs; current llvm-mingw still
# defaults WINVER below Windows 10, which hides their declarations.
target_compile_definitions(TrueCrimeNYC PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)

# Keep the SDK startup intact and invoke it after the launcher closes.
set_property(SOURCE "${SDK}/runtime/app/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS wWinMain=RecompMain)
target_sources(TrueCrimeNYC PRIVATE launcher/launcher.cpp launcher/version.rc)
target_include_directories(TrueCrimeNYC PRIVATE launcher)
set_property(SOURCE launcher/version.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.ico")
# Embed the long-path manifest. lld-link only accepts /MANIFESTINPUT with /manifest:embed; a
# MinGW-target link would hand that to mt.exe, so those builds carry it as a resource instead.
if(MINGW)
    target_sources(TrueCrimeNYC PRIVATE launcher/manifest.rc)
    set_property(SOURCE launcher/manifest.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.manifest")
else()
    target_link_options(TrueCrimeNYC PRIVATE
        "-Wl,/manifest:embed"
        "-Wl,/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.manifest")
endif()
target_link_libraries(TrueCrimeNYC PRIVATE comdlg32 gdi32 user32)
