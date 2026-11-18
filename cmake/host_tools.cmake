# Locate the Arm toolchain and the prebuilt host tools (picotool, pioasm).
#
# B-4: on Windows the host tools must be the prebuilt binaries, located via
# picotool_DIR and pioasm_DIR, so no native host compiler is needed.
#
# Resolution order for each item:
#   1. a CMake variable given on the command line / in CMakeUserPresets.json
#   2. an environment variable of the same name
#   3. the default install location of the Raspberry Pi Pico VS Code
#      extension (%USERPROFILE%\.pico-sdk\...)
# If nothing is found on a non-Windows host, the SDK builds the tools from
# source with the host compiler (fine on Linux CI, not used on Windows).

set(PINNED_PICO_SDK_VERSION 2.2.0)
set(PINNED_TOOLCHAIN_VERSION 14_2_Rel1)
set(PINNED_PICOTOOL_VERSION 2.2.0)

if(WIN32 OR CMAKE_HOST_WIN32)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _user_home)
else()
    set(_user_home "$ENV{HOME}")
endif()
set(_pico_home "${_user_home}/.pico-sdk")

macro(_pick_path var candidate)
    if(NOT DEFINED ${var})
        if(DEFINED ENV{${var}})
            file(TO_CMAKE_PATH "$ENV{${var}}" _p)
            set(${var} "${_p}" CACHE PATH "${var}")
        elseif(EXISTS "${candidate}")
            set(${var} "${candidate}" CACHE PATH "${var}")
        endif()
    endif()
endmacro()

# Newest match of a glob pattern (natural version order), or "" if none.
function(_newest_match out)
    file(GLOB _cands LIST_DIRECTORIES true ${ARGN})
    if(_cands)
        list(SORT _cands COMPARE NATURAL ORDER DESCENDING)
        list(GET _cands 0 _first)
        set(${out} "${_first}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

_pick_path(PICO_TOOLCHAIN_PATH "${_pico_home}/toolchain/${PINNED_TOOLCHAIN_VERSION}")
if(NOT DEFINED PICO_TOOLCHAIN_PATH)
    # Another Arm toolchain version the extension installed (newest first).
    # Arm GNU folders start with a digit (e.g. 14_2_Rel1); the extension's
    # RISC-V toolchains start with a letter and must not be picked up.
    _newest_match(_tc "${_pico_home}/toolchain/[0-9]*")
    if(_tc)
        get_filename_component(_tcn "${_tc}" NAME)
        message(WARNING "Pinned Arm toolchain ${PINNED_TOOLCHAIN_VERSION} not found under "
                        "${_pico_home}/toolchain; using ${_tcn} instead (B-10 pins "
                        "${PINNED_TOOLCHAIN_VERSION}).")
        _pick_path(PICO_TOOLCHAIN_PATH "${_tc}")
    endif()
endif()

# picotool: the directory holding picotoolConfig.cmake. The extension's
# folder layout and version naming vary between releases, so search for the
# config file under .pico-sdk/picotool rather than assume one path. The SDK
# accepts any picotool 2.x >= 2.1.1 and stops with a clear error otherwise.
_pick_path(picotool_DIR "${_pico_home}/picotool/${PINNED_PICOTOOL_VERSION}/picotool")
if(NOT DEFINED picotool_DIR)
    _newest_match(_ptc
        "${_pico_home}/picotool/*/picotool/picotoolConfig.cmake"
        "${_pico_home}/picotool/*/picotoolConfig.cmake"
        "${_pico_home}/picotool/*/*/picotoolConfig.cmake"
        "${_pico_home}/picotool/*/picotool/picotool-config.cmake"
        "${_pico_home}/picotool/*/picotool-config.cmake")
    if(_ptc)
        get_filename_component(_ptd "${_ptc}" DIRECTORY)
        _pick_path(picotool_DIR "${_ptd}")
    endif()
endif()

# The extension ships pioasm in its "tools" bundle, which provides a CMake
# config file that sets pioasm_DIR.
if(NOT DEFINED pioasm_DIR AND NOT DEFINED ENV{pioasm_DIR})
    set(_tools "${_pico_home}/tools/${PINNED_PICO_SDK_VERSION}")
    if(EXISTS "${_tools}/pico-sdk-tools-config.cmake")
        set(pico-sdk-tools_DIR "${_tools}")
        include("${_tools}/pico-sdk-tools-config.cmake")
    elseif(EXISTS "${_tools}/pioasm")
        set(pioasm_DIR "${_tools}/pioasm" CACHE PATH "pioasm_DIR")
    endif()
endif()
_pick_path(pioasm_DIR "")

# Report what was found, so a failed lookup is obvious in the configure log.
foreach(_t PICO_TOOLCHAIN_PATH picotool_DIR pioasm_DIR)
    if(DEFINED ${_t})
        message(STATUS "host_tools: ${_t} = ${${_t}}")
    endif()
endforeach()

if(CMAKE_HOST_WIN32)
    foreach(_t picotool_DIR pioasm_DIR)
        if(NOT DEFINED ${_t})
            message(WARNING "${_t} is not set and no copy was found under ${_pico_home}. "
                            "Set ${_t} to the folder containing the prebuilt Windows tool's "
                            "CMake config file (see README.md, Manual setup); otherwise the SDK "
                            "will try to build it with a host compiler and fail on Windows (B-4).")
        endif()
    endforeach()
endif()
