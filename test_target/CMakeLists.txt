cmake_minimum_required(VERSION 3.21)

# usbdfc test target (VE-1, VE-2): a Raspberry Pi Pico 2 acting as the
# downstream CDC-ACM device, with fault-injection modes and timestamps.

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../lib/pico-sdk CACHE PATH "Pico SDK")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/host_tools.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/pico_sdk_import.cmake)

project(usbdfc_test_target C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

pico_sdk_init()

add_executable(test_target
    src/main.c
    src/usb_descriptors.c
)

target_include_directories(test_target PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(test_target PRIVATE
    pico_stdlib
    tinyusb_device
    hardware_irq
    hardware_pwm
    hardware_uart
    pico_unique_id
)

# Timestamps in the USB interrupt (SOF, OUT completion, bus reset), and
# the "STALL on SET_LINE_CODING" mode, without modifying TinyUSB.
target_link_options(test_target PRIVATE
    -Wl,--wrap=dcd_event_handler
    -Wl,--wrap=cdcd_control_xfer_cb
)

target_compile_definitions(test_target PRIVATE
    PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1
)

pico_set_binary_type(test_target copy_to_ram)
pico_enable_stdio_uart(test_target 0)
pico_enable_stdio_usb(test_target 0)

target_compile_options(test_target PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion>
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage -Wstack-usage=1024>
)
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_runtime_init/runtime_init.c
    PROPERTIES COMPILE_DEFINITIONS "asm=__asm")

target_compile_options(test_target PRIVATE $<$<CONFIG:Release>:-g>)

pico_add_extra_outputs(test_target)
