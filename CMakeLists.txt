# True Crime: New York City (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    # Pin the fallback SDK to the upstream commit this port's sdk/ tree is based on
    # (see patches/xboxxberecomp-local.patch). Override with -DXBE_SDK_TAG=<commit or tag>.
    set(XBE_SDK_TAG "f7b13fdbf9560a3b3928aea24e37c789b24408f0" CACHE STRING
        "Upstream XboxXBE-Recomp commit/tag used when sdk/ is not present")
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG ${XBE_SDK_TAG})
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

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

# The app uses the per-monitor DPI and dark-mode era APIs; current llvm-mingw still
# defaults WINVER below Windows 10, which hides their declarations.
target_compile_definitions(TrueCrimeNYC PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)

# Keep the SDK startup intact and invoke it after the launcher closes.
set_property(SOURCE "${SDK}/runtime/app/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS wWinMain=RecompMain)
target_sources(TrueCrimeNYC PRIVATE launcher/launcher.cpp launcher/version.rc)
target_include_directories(TrueCrimeNYC PRIVATE launcher)
set_property(SOURCE launcher/version.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.ico")
# Merge the long-path manifest into the manifest CMake already embeds.
target_link_options(TrueCrimeNYC PRIVATE
    "-Wl,/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/launcher/TrueCrimeNYC.manifest")
target_link_libraries(TrueCrimeNYC PRIVATE comdlg32 gdi32 user32)
