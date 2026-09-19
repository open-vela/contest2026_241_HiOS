/*
 * Embed Claw4 custom WakeNet model (wn9l_hai1tai4ling2 / Hi 钛灵).
 */
    .section .rodata.srmodels,"a"
    .align 4
    .global g_metalio_srmodels_bin
    .global g_metalio_srmodels_bin_end
g_metalio_srmodels_bin:
    .incbin "model/srmodels.bin"
g_metalio_srmodels_bin_end:
