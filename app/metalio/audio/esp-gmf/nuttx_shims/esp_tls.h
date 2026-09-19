/*
 * esp_tls.h — NuttX shim stub.
 *
 * The gmf_io build does not use esp_tls directly from C sources; only
 * media_lib_sal's TLS port includes it.  For gmf_io's HTTP element the
 * connection-level security is handled (or intentionally left unimplemented)
 * inside nuttx_http_adapter.c.
 *
 * We provide the minimal typedef set required to satisfy any stray
 * #include "esp_tls.h" so compilation doesn't error out.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_tls esp_tls_t;

/* Opaque — never instantiated by the shimmed callers today. */
struct esp_tls_cfg {
    const char *alpn_protos;
    bool        skip_common_name;
    const char *cert_pem;
    size_t      cert_len;
    const char *client_cert_pem;
    size_t      client_cert_len;
    const char *client_key_pem;
    size_t      client_key_len;
};
typedef struct esp_tls_cfg esp_tls_cfg_t;

enum esp_tls_conn_state {
    ESP_TLS_INIT = 0,
    ESP_TLS_CONNECTING,
    ESP_TLS_HANDSHAKE,
    ESP_TLS_FAIL,
    ESP_TLS_DONE,
};

#ifdef __cplusplus
}
#endif
