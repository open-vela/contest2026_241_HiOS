#ifndef METALIO_APP_DNS_H
#define METALIO_APP_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Select an app-local cellular resolver without changing NuttX's global
 * resolver, which remains in use by unrelated services such as radio. */
void metalio_app_dns_set_cellular(const uint32_t *servers, size_t count);
void metalio_app_dns_set_wifi(void);
/* Pin application-owned sockets to the active cellular interface when 4G is
 * selected. This does not affect system or radio sockets. */
int metalio_app_bind_active_interface(int fd);
/* True when app-owned requests are routed through the NT26 cellular link. */
int metalio_app_cellular_active(void);
int metalio_app_resolve_ipv4(const char *hostname, uint32_t *address);
// Interactive voice only: selected network, bounded two-second DNS.
int metalio_voice_resolve_ipv4(const char *hostname, uint32_t *address);
int metalio_media_resolve_ipv4(const char *hostname, uint32_t *address);

#ifdef __cplusplus
}
#endif

#endif
