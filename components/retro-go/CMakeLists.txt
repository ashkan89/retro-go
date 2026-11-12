set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/netplay libs/lodepng")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/netplay libs/lodepng")
if((IDF_VERSION_MAJOR EQUAL 5) AND (IDF_VERSION_MINOR GREATER 2)) # esp-idf 5.3+
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client esp_driver_ledc esp_driver_rmt driver")
elseif(IDF_VERSION_MAJOR EQUAL 5) # esp-idf 5.0 - 5.2
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client")
else() # esp-idf 4.1 - 4.4
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update esp_adc_cal json nvs_flash esp_wifi esp_http_client")
endif()

if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND COMPONENT_REQUIRES usb usb_host_hid esp_tinyusb)
endif()

list(APPEND COMPONENT_REQUIRES mbedtls)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND COMPONENT_REQUIRES esp_driver_ppa esp_wifi_remote esp_hosted)
endif()
register_component()

if(IDF_TARGET STREQUAL "esp32p4")
    # Apply P4 UTMI host detection and speed policy at initialization and
    # after recovery, before the hub driver powers the root port.
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=usb_dwc_hal_init"
        "-Wl,--wrap=usb_dwc_hal_core_soft_reset")

    # ESP-Hosted 2.13.1 starts its transport from a global constructor. On
    # this XiP P4 port that runs before the application can power VDD_IO_5
    # and initialize the runtime. Start it explicitly in rg_network_init().
    idf_component_get_property(hosted_lib espressif__esp_hosted COMPONENT_LIB)
    get_target_property(hosted_sources ${hosted_lib} SOURCES)
    list(LENGTH hosted_sources hosted_source_count)
    list(FILTER hosted_sources EXCLUDE REGEX "(^|/)port_esp_hosted_host_init[.]c$")
    list(LENGTH hosted_sources hosted_filtered_count)
    if(hosted_filtered_count EQUAL hosted_source_count)
        message(FATAL_ERROR "ESP-Hosted startup source changed; review explicit radio initialization")
    endif()
    set_property(TARGET ${hosted_lib} PROPERTY SOURCES "${hosted_sources}")

    target_sources(${COMPONENT_LIB} PRIVATE drivers/display/p4_pixels.S)
    if(RG_P4_SELFTEST)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE RG_P4_SELFTEST=1)
    endif()
endif()

if(RG_SEPARATE_DISPLAY_AUDIO)
    component_compile_options(-DRG_SEPARATE_DISPLAY_AUDIO=1)
endif()

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)
set_source_files_properties(
    rg_audio.c rg_display.c rg_utils.c rg_surface.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS
    -O2
)

# N8R2 launchers have a 0x170000 app budget. PNG artwork runs in the low-priority
# artwork task. The separate display core also lets the small-flash launcher favour
# compact display/surface/helpers code while keeping I2S at -O2.
if(RG_SEPARATE_DISPLAY_AUDIO AND RG_BUILD_TARGET MATCHES "_N8R2_")
    set_source_files_properties(libs/lodepng/lodepng.c rg_display.c rg_surface.c rg_utils.c PROPERTIES COMPILE_FLAGS -Os)
endif()

if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_PROJECT_VER)
    component_compile_options(-DRG_PROJECT_VER="${RG_PROJECT_VER}")
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

component_compile_options(-D${RG_BUILD_TARGET}=1)
component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
