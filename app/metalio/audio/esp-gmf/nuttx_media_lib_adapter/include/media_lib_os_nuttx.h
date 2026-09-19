/*
 * Public API for the NuttX pthread-based media_lib_sal OS adapter.
 *
 * Call media_lib_os_nuttx_install() once during boot (before any
 * esp_hls_stream / esp_extractor code runs) to register the OS
 * vtable into media_lib_sal's dispatcher.  After that, every call
 * through media_lib_malloc / media_lib_mutex_create / ... will
 * be dispatched to the NuttX POSIX implementation in
 * nuttx_media_lib_adapter/media_lib_os_nuttx.c.
 */
#ifndef MEDIA_LIB_OS_NUTTX_H
#define MEDIA_LIB_OS_NUTTX_H

#ifdef __cplusplus
extern "C" {
#endif

void media_lib_os_nuttx_install(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_LIB_OS_NUTTX_H */
