set(COMPONENT_SRCDIRS ". codecs")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go")

register_component()

# The target macro selects the screen geometry, audio routing and GPIO map that the media
# UI reads out of retro-go's config.h, so it has to reach this component too.
rg_setup_compile_options()

# The decoders and the DSP block are the only hot paths here; everything else is UI and
# parsing, which is much better off small because of the tiny instruction cache.
component_compile_options(
    -Os
    -Wno-unused-function
    -Wno-char-subscripts
    # Display strings are deliberately clipped to the width of their fixed buffers; every
    # such site uses snprintf, so truncation is the designed behaviour rather than a defect.
    -Wno-format-truncation
)

set_source_files_properties(
    codecs/codec_mp3.c
    codecs/codec_wav.c
    media_eq.c
    media_fft.c
    media_audio.c
    media_image.c
    PROPERTIES COMPILE_FLAGS -O2
)

# dr_flac's own headers trip these under -Wall; the code is upstream and unmodified.
set_source_files_properties(
    codecs/codec_flac.c
    PROPERTIES COMPILE_FLAGS "-O2 -Wno-unused-but-set-variable -Wno-type-limits"
)

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()
