set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options(-O2 -Wno-error=format -Wno-error=char-subscripts)

option(RG_P4_SELFTEST "Run P4 JPEG and PPA hardware readback tests at launcher boot" OFF)
if(RG_P4_SELFTEST)
    if(NOT IDF_TARGET STREQUAL "esp32p4")
        message(FATAL_ERROR "RG_P4_SELFTEST requires ESP32-P4")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RG_P4_SELFTEST=1)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_jpeg idf::esp_driver_ppa)
endif()

# Keep the management interface in firmware, available even without an SD card.
# Generate deterministically and rebuild when either the page or packer changes.
set(WEBUI_ASSET "${CMAKE_CURRENT_BINARY_DIR}/webui_asset.c")
add_custom_command(OUTPUT "${WEBUI_ASSET}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/pack_webui.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html" "${WEBUI_ASSET}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/pack_webui.py"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${WEBUI_ASSET}")

# Limit the SD manager and streaming ZIP reader code size on small-flash targets.
set_source_files_properties(sdcard_zip.c sdcard_manager.c PROPERTIES COMPILE_FLAGS -Os)
