cmake_minimum_required(VERSION 3.21)

# Shell project: build settings inherited from DFC Rev 2.16 (B-* requirements).
# See docs/REQUIREMENTS.md for what is carried over and what is still open.

# Board and platform. Set before the SDK is imported.
set(PICO_BOARD seeed_xiao_rp2350 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# The XIAO RP2350 has 2 MB of flash; the SDK board file defaults to 4 MB.
set(PICO_FLASH_SIZE_BYTES "2 * 1024 * 1024" CACHE STRING "Flash size (XIAO RP2350: 2 MB)")

# The SDK is the pinned git submodule unless PICO_SDK_PATH is given.
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk CACHE PATH "Pico SDK")
endif()

# Locate the toolchain and prebuilt host tools installed by the
# Raspberry Pi Pico VS Code extension, unless given explicitly.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/host_tools.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(usbdfc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Pico SDK >= 2.0.0 required (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

add_executable(firmware
    src/main.c
)

target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(firmware PRIVATE
    pico_stdlib
)

# All code executes from SRAM (also keeps core 1's USB timing free of XIP stalls).
pico_set_binary_type(firmware copy_to_ram)

# No stdio yet. Native USB is planned as a CDC device for diagnostics; the
# PIO-USB host port is added with the Pico-PIO-USB submodule (TBD).
pico_enable_stdio_uart(firmware 0)
pico_enable_stdio_usb(firmware 0)

# Strict warnings and stack checks on every C source in the image.
target_compile_options(firmware PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion>
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage -Wstack-usage=1024>
)

target_compile_definitions(firmware PRIVATE PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1)

# Per-file exceptions to the warning rules, each justified and limited to one
# file. Review this list whenever the SDK pin changes.
#  1. runtime_init.c uses the GNU keyword "asm" once (RP2350 DCP init).
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_runtime_init/runtime_init.c
    PROPERTIES COMPILE_DEFINITIONS "asm=__asm")

# Release keeps debug information.
target_compile_options(firmware PRIVATE $<$<CONFIG:Release>:-g>)

pico_add_extra_outputs(firmware)
