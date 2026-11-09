# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set(gpsp_core_sources
    "core/bios_data.S"
    "core/cheats.c"
    "core/cpu.cc"
    "core/gba_cc_lut.c"
    "core/gba_memory.c"
    "core/gba_rom_scan.c"
    "core/gbp.c"
    "core/input.c"
    "core/libretro/libretro.c"
    "core/libretro/libretro-common/compat/compat_posix_string.c"
    "core/libretro/libretro-common/compat/compat_strl.c"
    "core/libretro/libretro-common/compat/fopen_utf8.c"
    "core/libretro/libretro-common/encodings/encoding_utf.c"
    "core/libretro/libretro-common/file/file_path.c"
    "core/libretro/libretro-common/file/file_path_io.c"
    "core/libretro/libretro-common/streams/file_stream.c"
    "core/libretro/libretro-common/string/stdstring.c"
    "core/libretro/libretro-common/time/rtime.c"
    "core/libretro/libretro-common/vfs/vfs_implementation.c"
    "core/main.c"
    "core/memmap.c"
    "core/rfu.c"
    "core/savestate.c"
    "core/serial.c"
    "core/serial_proto.c"
    "core/sound.c"
    "core/video.cc"
    "core/riscv/rv32_arm_alu.c"
    "core/riscv/rv32_arm_branch.c"
    "core/riscv/rv32_arm_mul.c"
    "core/riscv/rv32_codegen.c"
    "core/riscv/rv32_fallback.c"
    "core/riscv/rv32_platform.c"
    "core/riscv/rv32_stub.S"
    "core/riscv/rv32_thumb_alu.c"
    "core/cpu_threaded.c"
    "core/riscv/rv32_arm_memory.c"
    "core/riscv/rv32_arm_io.S"
    "core/riscv/rv32_arm_palette.S"
    "core/riscv/rv32_arm_system.c"
    "core/riscv/rv32_block_emit.c"
    "core/riscv/rv32_block_link.c"
    "core/riscv/rv32_block_runtime.cc"
    "core/riscv/rv32_interpreter_stub.S"
    "core/riscv/rv32_ram_units.c"
    "core/riscv/rv32_rom_load.S"
    "core/riscv/rv32_thumb_memory.c"
    "core/riscv/rv32_thumb_multi_ram.S"
    "core/riscv/rv32_thumb_ram_helper.S"
    "core/riscv/rv32_thumb_ram_store.S"
    "core/riscv/rv32_flow_timing.c"
    "core/riscv/rv32_arm_flow_alu.c"
    "core/riscv/rv32_arm_flow_full.c"
    "core/riscv/rv32_thumb_flow_full.c"
    "core/riscv/rv32_thumb_exit_shared.S"
)


idf_component_register(SRCS ${gpsp_core_sources}
    INCLUDE_DIRS "core/libretro/libretro-common/include" "core/include"
    PRIV_INCLUDE_DIRS "core" "core/libretro"
    LDFRAGMENTS "linker.lf")
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "This gpSP RV32 port requires ESP32-P4")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE FRONTEND_SUPPORTS_RGB565
    HAVE_INTTYPES_H HAVE_STDINT_H HAVE_STRINGS_H INLINE=inline __LIBRETRO__
    GPSP_RV32_JIT=1 ROM_BUFFER_SIZE=24
    GPSP_RV32_ROM_CACHE_KIB=2048 GPSP_RV32_RAM_CACHE_KIB=2048 GPSP_RV32_CODE_CHUNK_KIB=256
    # WT0132P4 v1.3 traps on upstream's bexti instruction. Keep the RV32
    # dynarec, but generate only the original P4's base ISA and RVC subset.
    GPSP_RV32_USE_ZBA=0 GPSP_RV32_USE_ZBB=0 GPSP_RV32_USE_ZBS=0
    GPSP_RV32_USE_RVC=1 GPSP_RV32_USE_ZCB=0)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
    $<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/core>)
set_source_files_properties("core/video.cc" PROPERTIES COMPILE_OPTIONS "-O3")
