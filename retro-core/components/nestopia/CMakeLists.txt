file(GLOB_RECURSE COMPONENT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/upstream/source/core/*.cpp"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "upstream/source"
    "upstream/source/core"
    "upstream/source/core/api"
)

register_component()
rg_setup_compile_options(-O3 -fno-rtti -fexceptions -DNST_NO_ZLIB -Wno-error)
