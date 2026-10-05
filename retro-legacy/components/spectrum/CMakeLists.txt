# The ODROID frontend is replaced by main/spectrum.c. Retain the CPU,
# 48K ROM, and snapshot codec from the pinned Spectemu port.
set(COMPONENT_SRCS z80.c z80_step.c z80_op1.c z80_op2.c z80_op3.c
    z80_op4.c z80_op5.c z80_op6.c z80optab.c spperif.c spect.c
    snapshot.c compr.c misc.c interf.c rom_imag.c)
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options(-O2 -DHAVE_CONFIG_H=1 -Wno-error -Wno-error=format)
