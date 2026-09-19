/* ROM memcpy/memmove/strcmp hard-lock on SPIRAM operands.
 * memset: SPIRAM-aware (software on PSRAM, ROM on DRAM) — full software
 * memset previously blue-screened Home/LVGL.
 * This script is linked last so it overrides esp32p4.rom*.ld absolutes. */
memcpy = metalio_wn_memcpy;
memmove = metalio_wn_memmove;
memset = metalio_wn_memset;
strcmp = metalio_wn_strcmp;
