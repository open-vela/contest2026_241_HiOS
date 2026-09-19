/*
 * esp_log.h redirect — maps to the project's esp_log_shim.h.
 * Provides ESP_LOGE/W/I/D/V macros using NuttX syslog.
 */
#ifndef __ESP_LOG_REDIRECT_H
#define __ESP_LOG_REDIRECT_H
#include "esp_log_shim.h"

/* __FILENAME__ is used by esp_gmf_err.h macros. GCC doesn't define it
 * natively — alias it to __FILE__ (full path). The log output will be
 * slightly longer but functionally identical. */
#ifndef __FILENAME__
#define __FILENAME__ __FILE__
#endif

/* unlikely/likely — GCC builtins used by esp_gmf_err.h error-check macros. */
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#endif /* __ESP_LOG_REDIRECT_H */
