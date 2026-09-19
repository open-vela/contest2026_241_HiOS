/*
 * esp_http_client.h — NuttX / OpenVela shim for ESP-IDF's esp_http_client.
 *
 * This is NOT the full ESP-IDF esp_http_client.  It exposes the small subset
 * of the API that is actually called by gmf_io's esp_gmf_io_http.c:
 *
 *   Types    : esp_http_client_handle_t, esp_http_client_config_t,
 *              esp_http_client_event_id_t, esp_http_client_event_t,
 *              esp_http_client_event_handle_t.
 *
 *   Functions: esp_http_client_init / cleanup
 *              esp_http_client_set_url / set_header / delete_header
 *              esp_http_client_get_post_field
 *              esp_http_client_open / close
 *              esp_http_client_fetch_headers
 *              esp_http_client_get_status_code / set_redirection / get_errno
 *              esp_http_client_get_socket
 *              esp_http_client_read / write
 *
 * The implementation lives in nuttx_http_adapter/nuttx_http_adapter.c and is
 * built as part of the gmf_io build block (see esp-gmf/Make.defs).
 *
 * HTTPS / TLS is not implemented by the adapter.  Passing an https:// URL
 * will cause esp_http_client_open() to return ESP_GMF_ERR_NOT_SUPPORT and
 * set errno = ENOTSUP.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Event IDs emitted by the shim during a request.                           */
/*  Only HTTP_EVENT_ON_HEADER is actually consumed by gmf_io_http.c.          */
/* -------------------------------------------------------------------------- */

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,        /* gmf_io_http.c watches this for Content-Encoding */
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

/* -------------------------------------------------------------------------- */
/*  Event struct forwarded to the registered event_handler callback.         */
/* -------------------------------------------------------------------------- */

typedef struct esp_http_client_event {
    void                  *client;
    void                  *user_data;
    esp_http_client_event_id_t  event_id;

    /* For HTTP_EVENT_ON_HEADER */
    const char            *header_key;
    const char            *header_value;

    /* For HTTP_EVENT_ON_DATA (not used by gmf_io_http.c) */
    void                  *data;
    int                    data_len;
} esp_http_client_event_t;

typedef esp_err_t (*esp_http_client_event_handle_t)(esp_http_client_event_t *evt);

/* -------------------------------------------------------------------------- */
/*  Client configuration struct.                                              */
/*  gmf_io_http.c only sets url, event_handler, user_data, timeout_ms,       */
/*  buffer_size, buffer_size_tx, cert_pem and crt_bundle_attach.              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char                      *url;
    esp_http_client_event_handle_t   event_handler;
    void                            *user_data;
    int                              timeout_ms;
    int                              buffer_size;       /* rx buffer */
    int                              buffer_size_tx;    /* tx buffer */
    const char                      *cert_pem;          /* ignored (no TLS) */
    esp_err_t                      (*crt_bundle_attach)(void *conf); /* ignored */

    /* The remaining fields are present for API completeness only. */
    const char                      *host;
    int                              port;
    const char                      *username;
    const char                      *password;
    const char                      *path;
    const char                      *query;
    const char                      *method;
    int                              max_redirection_count;
    bool                             disable_auto_redirect;
    bool                             is_async;
} esp_http_client_config_t;

/* -------------------------------------------------------------------------- */
/*  Opaque handle — the real struct lives in nuttx_http_adapter.c.            */
/* -------------------------------------------------------------------------- */

typedef struct esp_http_client *esp_http_client_handle_t;

/* -------------------------------------------------------------------------- */
/*  Public API surface — matches the call-sites in esp_gmf_io_http.c.         */
/*                                                                           */
/*  Error return convention:                                                  */
/*    Functions returning esp_err_t use ESP_OK / ESP_FAIL.                    */
/*    esp_http_client_read / write return the byte count or -1 on error.      */
/* -------------------------------------------------------------------------- */

esp_http_client_handle_t  esp_http_client_init       (const esp_http_client_config_t *config);
esp_err_t                 esp_http_client_cleanup    (esp_http_client_handle_t client);

esp_err_t                 esp_http_client_set_url    (esp_http_client_handle_t client, const char *url);
esp_err_t                 esp_http_client_set_header (esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t                 esp_http_client_delete_header (esp_http_client_handle_t client, const char *key);

/* Retrieves the POST body set via post_body/post_len (the shim exposes a
 * set_post_field helper; gmf_io_http.c only ever queries it and the value
 * is always NULL / 0 for HLS streaming reads).  Returns 0 on success. */
int                       esp_http_client_get_post_field (esp_http_client_handle_t client, char **data);

/* write_len bytes of body already been written, or -1 to signal that the
 * body length is unknown (gmf_io_http.c passes -1 for write streams). */
esp_err_t                 esp_http_client_open       (esp_http_client_handle_t client, int write_len);
esp_err_t                 esp_http_client_close      (esp_http_client_handle_t client);

/* Returns total Content-Length from response headers, or -1 if unknown. */
int64_t                   esp_http_client_fetch_headers (esp_http_client_handle_t client);

int                       esp_http_client_get_status_code (esp_http_client_handle_t client);
esp_err_t                 esp_http_client_set_redirection (esp_http_client_handle_t client);
int                       esp_http_client_get_errno    (esp_http_client_handle_t client);
int                       esp_http_client_get_socket   (esp_http_client_handle_t client);

/* Returns bytes read / written, or 0 for EOF, or -1 for error. */
int                       esp_http_client_read         (esp_http_client_handle_t client, char *buf, int len);
int                       esp_http_client_write        (esp_http_client_handle_t client, const char *buf, int len);

/* Helper: set POST body & length before open() (not used by gmf_io_http.c). */
esp_err_t                 esp_http_client_set_post_field (esp_http_client_handle_t client,
                                                          const char *data, int len);

#ifdef __cplusplus
}
#endif
