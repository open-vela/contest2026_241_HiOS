// SPDX-License-Identifier: Apache-2.0
#include "hios_tls_roots.h"
#include <pthread.h>
#include "ca_bundle.inc"
static pthread_once_t once = PTHREAD_ONCE_INIT;
static mbedtls_x509_crt roots;
static int loaded;
static void load_roots(void) {
 mbedtls_x509_crt_init(&roots);
 loaded = mbedtls_x509_crt_parse(&roots,
     (const unsigned char *)hios_agent_ca_pem, sizeof(hios_agent_ca_pem)) == 0;
}
mbedtls_x509_crt *hios_agent_ca_chain(void) {
 pthread_once(&once, load_roots);
 return loaded ? &roots : NULL;
}
