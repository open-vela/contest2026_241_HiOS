/*
 * esp_extractor_defaults.h shim — NuttX/OpenVela port override.
 *
 * The upstream esp_extractor_defaults.h unconditionally #includes nine
 * different extractor headers (esp_wav_extractor.h, esp_mp4_extractor.h,
 * esp_audio_es_extractor.h, esp_ogg_extractor.h, esp_ts_extractor.h,
 * esp_avi_extractor.h, esp_caf_extractor.h, esp_flv_extractor.h,
 * esp_raw_extractor.h).  For the Metalio Claw4 HLS port we currently
 * ship the HLS extractor only (as libhls_lib.a), and those other
 * extractor components are not part of the current download batch.
 *
 * Because nuttx_shims/ sits FIRST on the include path, this file shadows
 * the upstream one and provides a purely HLS-aware register_default()
 * helper that registers HLS only.  Callers that really want WAV/MP4/…
 * must enable those components separately first.
 */
#ifndef __ESP_EXTRACTOR_DEFAULTS_SHIM_H
#define __ESP_EXTRACTOR_DEFAULTS_SHIM_H

#include "esp_extractor_types.h"
#include "esp_extractor.h"
#include "esp_hls_extractor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register the default set of extractors for this build.
 *
 * Currently this only registers esp_hls_extractor — the HLS demuxer
 * required for radio_screen streaming.  Additional demuxers (WAV/MP4/
 * OGG/TS/...) can be appended here when/if their binary components
 * are added to the esp-gmf port.
 *
 * @return ESP_EXTRACTOR_ERR_OK on success, error code otherwise.
 */
esp_extractor_err_t esp_extractor_register_default(void);

/**
 * @brief  Unregister the default set of extractors (mirror of register).
 */
void esp_extractor_unregister_default(void);

/**
 * @brief  Unregister every registered extractor (including custom ones).
 */
void esp_extractor_unregister_all(void);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_EXTRACTOR_DEFAULTS_SHIM_H */
