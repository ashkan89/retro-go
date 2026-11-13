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

# Theme artwork and the full web UI are installed from the SD-card asset bundle.

# Limit the SD manager and streaming ZIP reader code size on small-flash targets.
set_source_files_properties(sdcard_zip.c sdcard_manager.c PROPERTIES COMPILE_FLAGS -Os)
