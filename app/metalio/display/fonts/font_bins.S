/*
 * Embed xiaozhi common PuHui CBin fonts for Metalio chat / UI.
 * Paths are relative to -I display/fonts when assembling.
 */

    .section .rodata.metalio_fonts, "a"
    .balign 4

    .global metalio_font_puhui_30_bin
    .type   metalio_font_puhui_30_bin, @object
metalio_font_puhui_30_bin:
    .incbin "font_puhui_common_30_4.bin"
    .global metalio_font_puhui_30_bin_end
metalio_font_puhui_30_bin_end:

    .balign 4
    .global metalio_font_puhui_20_bin
    .type   metalio_font_puhui_20_bin, @object
metalio_font_puhui_20_bin:
    .incbin "font_puhui_common_20_4.bin"
    .global metalio_font_puhui_20_bin_end
metalio_font_puhui_20_bin_end:
