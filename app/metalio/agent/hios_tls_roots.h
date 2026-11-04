// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mbedtls/x509_crt.h"
#ifdef __cplusplus
extern "C" {
#endif
mbedtls_x509_crt *hios_agent_ca_chain(void);
#ifdef __cplusplus
}
#endif
