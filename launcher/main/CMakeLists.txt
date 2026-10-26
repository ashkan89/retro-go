set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options(-O2 -Wno-error=format -Wno-error=char-subscripts)

# add_custom_command(OUTPUT images.c
#                    COMMAND python ${COMPONENT_DIR}/gen_images.py
#                    VERBATIM)
# add_custom_target(images DEPENDS images.c)
# add_dependencies(${COMPONENT_LIB} images)

# Limit the SD manager and streaming ZIP reader code size on small-flash targets.
set_source_files_properties(sdcard_zip.c sdcard_manager.c PROPERTIES COMPILE_FLAGS -Os)
