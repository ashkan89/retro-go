if(NOT CONFIG_RG_NES_EMULATOR_NESTOPIA)
    register_component()
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/upstream/source/core/api/NstApiEmulator.hpp")
    message(FATAL_ERROR "Nestopia sources missing. Run: git submodule update --init --recursive")
endif()

file(GLOB_RECURSE COMPONENT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/upstream/source/core/*.cpp"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "upstream/source"
    "upstream/source/core"
    "upstream/source/core/api"
)

register_component()
# Xtensa uses 16-bit wchar_t: upstream's ASCII case-folding initializer
# promotes to int before storing back to wchar_t. Match the native core build
# and allow this safe conversion without changing the pinned submodule.
rg_setup_compile_options(-O3 -fno-rtti -fexceptions -DNST_NO_ZLIB -Wno-error -Wno-narrowing)
