# Linux build for the SteamVR driver: produces driver_cardboardplusplus.so
# consumed by vrserver via the driver factory in DeviceFactory.cpp.
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential cmake pkg-config libavcodec-dev libavformat-dev \
#                        libavutil-dev libswscale-dev libvulkan-dev libx11-dev
#   SteamVR's openvr_api.so: pass -DCBPP_OPENVR_DIR=~/.steam/steam/steamapps/common/SteamVR/bin/linux64
cmake_minimum_required(VERSION 3.16)
project(driver_cardboardplusplus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

# openvr_api.so ships with SteamVR rather than the distro; point CBPP_OPENVR_DIR at
# bin/linux64 or let the env var OPENVR_DIR do it.
if(NOT CBPP_OPENVR_DIR)
    if(DEFINED ENV{OPENVR_DIR})
        set(CBPP_OPENVR_DIR "$ENV{OPENVR_DIR}")
    endif()
endif()
find_library(OPENVR_API_LIB
    NAMES openvr_api
    HINTS ${CBPP_OPENVR_DIR}
    PATHS "$ENV{HOME}/.steam/steam/steamapps/common/SteamVR/bin/linux64"
          "$ENV{HOME}/.local/share/Steam/steamapps/common/SteamVR/bin/linux64"
          "/usr/lib"
    NO_DEFAULT_PATH)
if(NOT OPENVR_API_LIB)
    message(FATAL_ERROR "openvr_api.so not found. Pass -DCBPP_OPENVR_DIR=<SteamVR>/bin/linux64")
endif()
message(STATUS "openvr_api: ${OPENVR_API_LIB}")

set(CBPP_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vk_video)

set(CBPP_SOURCES
    src/DeviceFactory.cpp
    src/DeviceProvider.cpp
    src/DriverLog.cpp
    src/HandDriver.cpp
    src/Discovery.cpp
    src/UdpTransport.cpp
    src/BridgeServer.cpp
    src/EncoderSetup.cpp
    src/EncodingThread.cpp
    src/VideoEncoder.cpp
    src/VideoEncoderFFmpeg.cpp
    src/VideoEncoderShaders.cpp
    src/VulkanContext.cpp
    src/VulkanCompose.cpp
    src/DirectModePosix.cpp)

# Shaders: the HLSL lives once in include/shaders/*.hlsli as a raw string, so the
# Windows build compiles it at runtime and the Linux build extracts the same text,
# runs dxc at build time and embeds the SPIR-V as C arrays. No runtime dxc needed.
find_program(CBPP_DXC_EXE NAMES dxc
    HINTS /opt/dxc/bin /usr/local/bin /usr/bin
    DOC "DirectXShaderCompiler used to build the SPIR-V for the Vulkan pipelines")
if(NOT CBPP_DXC_EXE)
    message(FATAL_ERROR "dxc not found. Install the DirectXShaderCompiler release and pass -DCBPP_DXC_EXE=/path/to/dxc")
endif()
message(STATUS "dxc: ${CBPP_DXC_EXE}")

set(CBPP_SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spirv)
set(CBPP_SPIRV_HEADERS)
foreach(shader compose_vs compose_ps nv12_cs)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/include/shaders/${shader}.hlsli _hlsli_text)
    string(REGEX MATCH "\\)\"([^;]*)\";" _hlsli_match "${_hlsli_text}")
    if(NOT _hlsli_match)
        message(FATAL_ERROR "no raw string found in ${shader}.hlsli")
    endif()
    set(_hlsl ${CMAKE_CURRENT_BINARY_DIR}/${shader}.hlsl)
    file(WRITE ${_hlsl} "${CMAKE_MATCH_1}")
    set(_spv ${CBPP_SPIRV_DIR}/${shader}.spv)
    set(_hdr ${CBPP_SPIRV_DIR}/${shader}.spv.h)
    if(shader STREQUAL "compose_vs")
        set(_profile "vs_6_0")
    elseif(shader STREQUAL "compose_ps")
        set(_profile "ps_6_0")
    else()
        set(_profile "cs_6_0")
    endif()
    add_custom_command(
        OUTPUT ${_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CBPP_SPIRV_DIR}
        COMMAND ${CBPP_DXC_EXE} -spirv -T ${_profile} -E main -fspv-target-env=vulkan1.1 -O3 ${_hlsl} -Fo ${_spv}
        COMMAND ${CMAKE_COMMAND}
                -DSPV=${_spv} -DHDR=${_hdr} -DNAME=CBPPSpirv_${shader}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake
        DEPENDS ${_hlsl}
        COMMENT "dxc ${shader} -> SPIR-V"
        VERBATIM)
    list(APPEND CBPP_SPIRV_HEADERS ${_hdr})
endforeach()

add_custom_target(cbpp_shaders DEPENDS ${CBPP_SPIRV_HEADERS})

add_library(driver_cardboardplusplus SHARED ${CBPP_SOURCES})
add_dependencies(driver_cardboardplusplus cbpp_shaders)

target_include_directories(driver_cardboardplusplus PRIVATE ${CBPP_INCLUDE_DIRS} ${CBPP_SPIRV_DIR})
target_compile_definitions(driver_cardboardplusplus PRIVATE
    VK_USE_PLATFORM_XLIB_KHR
    _FILE_OFFSET_BITS=64)

target_compile_options(driver_cardboardplusplus PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

target_link_libraries(driver_cardboardplusplus PRIVATE
    ${OPENVR_API_LIB}
    PkgConfig::FFMPEG
    PkgConfig::VULKAN
    PkgConfig::X11
    pthread
    dl)

# Shaders are compiled at build time into embedded SPIR-V (see the dxc block above);
# nothing shader related is resolved at runtime on Linux.
install(TARGETS driver_cardboardplusplus LIBRARY DESTINATION bin)