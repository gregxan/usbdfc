cmake_minimum_required(VERSION 3.21)

# Build settings inherited from DFC Rev 2.16 (B-* requirements), extended
# for the PIO-USB host (USBH-S1, USBH-S2). See docs/REQUIREMENTS.md.

# Board and platform. Set before the SDK is imported.
set(PICO_BOARD seeed_xiao_rp2350 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# The XIAO RP2350 has 2 MB of flash; the SDK board file defaults to 4 MB.
set(PICO_FLASH_SIZE_BYTES "2 * 1024 * 1024" CACHE STRING "Flash size (XIAO RP2350: 2 MB)")

# The SDK is the pinned git submodule unless PICO_SDK_PATH is given.
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk CACHE PATH "Pico SDK")
endif()

# USBH-S2: Pico-PIO-USB pinned submodule; TinyUSB's family.cmake picks it up.
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB CACHE PATH "Pico-PIO-USB")

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

if(NOT TARGET tinyusb_pico_pio_usb)
    check_and_add_pico_pio_usb_support()
endif()
if(NOT TARGET tinyusb_pico_pio_usb)
    message(FATAL_ERROR "Pico-PIO-USB not found at ${PICO_PIO_USB_PATH} (git submodule update --init)")
endif()

option(APP_TEST_PINS "Drive the test GPIOs D8/D3 (HW-9)" OFF)
option(APP_STALL_HOOK "V-14 watchdog stall hook" OFF)
option(MAG_MODE_SINGLE "Single-measurement mode for V-18 (section 8a H2)" OFF)
set(MAG_R_TICK "10" CACHE STRING "Tick of the R trigger within the cycle (5 only for V-19)")

add_executable(firmware
    src/main.c
    src/mag.c
    src/tick.c
    src/diag.c
    src/prof.c
    src/record.c
    src/mailbox.c
    src/usbh.c
)

pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/src/i2c_bus_clear.pio)

target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(firmware PRIVATE
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_dma
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_sync
    hardware_timer
    hardware_watchdog
    tinyusb_host
    tinyusb_pico_pio_usb
)

target_compile_definitions(firmware PRIVATE
    APP_TEST_PINS=$<BOOL:${APP_TEST_PINS}>
    APP_STALL_HOOK=$<BOOL:${APP_STALL_HOOK}>
    MAG_MODE_SINGLE=$<BOOL:${MAG_MODE_SINGLE}>
    MAG_R_TICK=${MAG_R_TICK}u
    # No default alarm pool: nothing here needs it (USBH-S1a rule 5).
    PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1
)

# USBH-T6, section 7.1: observe transfer results TinyUSB's CDC host drops.
target_link_options(firmware PRIVATE -Wl,--wrap=hcd_event_handler)

# All code executes from SRAM (also keeps core 1's USB timing free of XIP stalls).
pico_set_binary_type(firmware copy_to_ram)

# No stdio: native USB is a diagnostic-only CDC device (NU-1, not yet built);
# GPIO0/1 carry the PIO-USB host.
pico_enable_stdio_uart(firmware 0)
pico_enable_stdio_usb(firmware 0)

# B-22, B-23: strict warnings and stack checks on every C source in the image.
target_compile_options(firmware PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion>
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage -Wstack-usage=1024>
)

target_compile_definitions(firmware PRIVATE PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1)

# B-25: per-file exceptions to the warning rules, each justified and limited
# to one file. Review this list whenever the SDK pin or USBH-S2 changes.
#  1. runtime_init.c uses the GNU keyword "asm" once (RP2350 DCP init).
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_runtime_init/runtime_init.c
    PROPERTIES COMPILE_DEFINITIONS "asm=__asm")
include(${CMAKE_CURRENT_LIST_DIR}/cmake/third_party_warnings.cmake)

# Release keeps debug information.
target_compile_options(firmware PRIVATE $<$<CONFIG:Release>:-g>)

pico_add_extra_outputs(firmware)
