if(NOT CONFIG_RG_NES_EMULATOR_NESTOPIA)
    register_component()
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/upstream/source/core/api/NstApiEmulator.hpp")
    message(FATAL_ERROR "Nestopia sources missing. Run: git submodule update --init --recursive")
endif()

option(RG_NESTOPIA_PORT "Use the embedded Nestopia hot paths and SRAM layout" ON)
set(NESTOPIA_CORE "${CMAKE_CURRENT_LIST_DIR}/upstream/source/core")
if(RG_NESTOPIA_PORT)
    file(GLOB_RECURSE NESTOPIA_INPUTS CONFIGURE_DEPENDS "${NESTOPIA_CORE}/*.hpp" "${NESTOPIA_CORE}/*.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${NESTOPIA_INPUTS} "${CMAKE_CURRENT_LIST_DIR}/prepare_core.py"
        "${CMAKE_CURRENT_LIST_DIR}/rg_nestopia_memory.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/rg_nestopia_profile.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/rg_nestopia_profile.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/rg_nestopia_tiles.inc")
    set(NESTOPIA_PREPARED "${CMAKE_BINARY_DIR}/nestopia-core")
    set(NESTOPIA_PREPARE_OPTIONS)
    if(NOT CONFIG_IDF_TARGET_ESP32P4)
        # Per-port invalidation metadata costs additional PSRAM. Keep the
        # smaller CPU map on S3; native tests exercise the P4 transformation.
        list(APPEND NESTOPIA_PREPARE_OPTIONS --no-direct-prg)
    endif()
    execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/prepare_core.py"
        "${NESTOPIA_CORE}" "${NESTOPIA_PREPARED}" ${NESTOPIA_PREPARE_OPTIONS}
        RESULT_VARIABLE NESTOPIA_PREPARE_RESULT)
    if(NOT NESTOPIA_PREPARE_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not prepare the pinned Nestopia core")
    endif()
    set(NESTOPIA_CORE "${NESTOPIA_PREPARED}")
endif()
file(GLOB_RECURSE COMPONENT_SRCS "${NESTOPIA_CORE}/*.cpp")
if(NOT RG_NESTOPIA_PORT)
    list(APPEND COMPONENT_SRCS "${CMAKE_CURRENT_LIST_DIR}/rg_nestopia_profile.cpp")
endif()

set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "upstream/source"
    "${NESTOPIA_CORE}"
    "${NESTOPIA_CORE}/api"
)

register_component()
# Xtensa uses 16-bit wchar_t: upstream's ASCII case-folding initializer
# promotes to int before storing back to wchar_t. Match the native core build
# and allow this safe conversion without changing the pinned submodule.
rg_setup_compile_options(-O3 -fno-rtti -fexceptions -DNST_NO_ZLIB -Wno-error -Wno-narrowing)
if(RG_NESTOPIA_PORT)
    # iNES parsing runs during cartridge loading, outside emulation.
    set_source_files_properties("${NESTOPIA_CORE}/NstCartridgeInes.cpp"
        PROPERTIES COMPILE_OPTIONS "-Os")
endif()
if(RG_NESTOPIA_PORT AND CONFIG_IDF_TARGET_ESP32S3)
    # Keep Xtensa literal pools with the template's IRAM COMDAT sections.
    set_source_files_properties("${NESTOPIA_CORE}/NstPpu.cpp"
        PROPERTIES COMPILE_OPTIONS "-mtext-section-literals")
endif()
if(RG_NESTOPIA_PROFILE AND RG_NESTOPIA_PORT)
    set_source_files_properties("${NESTOPIA_CORE}/rg_nestopia_profile.cpp"
        PROPERTIES COMPILE_DEFINITIONS RG_NESTOPIA_PROFILE=1)
    if(NOT RG_NESTOPIA_PROFILE_CORE STREQUAL "OFF")
        set_source_files_properties("${NESTOPIA_CORE}/NstPpu.cpp" "${NESTOPIA_CORE}/NstApu.cpp"
            PROPERTIES COMPILE_DEFINITIONS RG_NESTOPIA_PROFILE=1)
    endif()
endif()
if(RG_NESTOPIA_PROFILE AND NOT RG_NESTOPIA_PORT)
    set_source_files_properties(rg_nestopia_profile.cpp PROPERTIES COMPILE_DEFINITIONS RG_NESTOPIA_PROFILE=1)
endif()
