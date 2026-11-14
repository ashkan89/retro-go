// Eight RGB565 byte swaps per iteration using the P4 PIE vector unit.
// The wrapper guarantees a positive multiple of 8 pixels. Preserve CFG while
// enabling hardware misaligned accesses for odd-width and cropped rows.
    .text
    .align 4
    .global p4_swap_pixels
    .type p4_swap_pixels, @function
p4_swap_pixels:
    esp.movx.r.cfg a4
    ori a3, a4, 3
    esp.movx.w.cfg a3
    la a3, .Lp4_shift_left
    esp.vldbc.16.ip q2, a3, 0
    la a3, .Lp4_shift_right
    esp.vldbc.16.ip q3, a3, 0
    la a3, .Lp4_low_byte
    esp.vldbc.16.ip q4, a3, 0
.Lp4_swap_loop:
    esp.vld.128.ip q0, a1, 16
    esp.vsld.16 q1, q0, q2
    esp.vsld.16 q0, q0, q3
    // Discard sign extension from the right shift before merging the bytes.
    esp.andq q0, q0, q4
    esp.orq q0, q0, q1
    esp.vst.128.ip q0, a0, 16
    addi a2, a2, -8
    bnez a2, .Lp4_swap_loop
    esp.movx.w.cfg a4
    ret
    .size p4_swap_pixels, .-p4_swap_pixels
    // Same ownership/alignment rules as p4_swap_pixels, without byte swapping.
    // pixels must be a positive multiple of eight.
    .align 4
    .global p4_copy_pixels
    .type p4_copy_pixels, @function
p4_copy_pixels:
    esp.movx.r.cfg a4
    ori a3, a4, 3
    esp.movx.w.cfg a3
.Lp4_copy_loop:
    esp.vld.128.ip q0, a1, 16
    esp.vst.128.ip q0, a0, 16
    addi a2, a2, -8
    bnez a2, .Lp4_copy_loop
    esp.movx.w.cfg a4
    ret
    .size p4_copy_pixels, .-p4_copy_pixels
    .section .rodata
    .align 4
.Lp4_shift_left:
    .half 8
.Lp4_shift_right:
    .half -8
.Lp4_low_byte:
    .half 0x00ff
