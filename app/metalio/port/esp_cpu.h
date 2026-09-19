#ifndef ESP_CPU_H
#define ESP_CPU_H
#include <stdint.h>
static inline int esp_cpu_get_core_id(void) { return 0; }
static inline uint32_t esp_cpu_get_cycle_count(void) { return 0; }
#endif
