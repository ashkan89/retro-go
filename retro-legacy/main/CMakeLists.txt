if(RG_LEGACY_SELFTEST)
    set(AUDIO_DMA_TEST_SOURCE "${CMAKE_BINARY_DIR}/audio_dma_test.c")
    execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/test_audio_dma.py"
        --emit-c "${AUDIO_DMA_TEST_SOURCE}" RESULT_VARIABLE AUDIO_DMA_TEST_RESULT)
    if(NOT AUDIO_DMA_TEST_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to generate the I2S consumer regression test")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../../components/retro-go/drivers/audio/i2s.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../retro-core/main/main_snes.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../fmsx/main/main.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/test_audio_dma.py")
    set(COMPONENT_SRCS "spectrum.c" "prosystem.c" "stella.cpp" "../tests/firmware.c" "${AUDIO_DMA_TEST_SOURCE}")
else()
    set(COMPONENT_SRCDIRS ".")
endif()
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options(-O2)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
