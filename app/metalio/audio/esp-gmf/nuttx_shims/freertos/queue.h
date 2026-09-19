/*
 * freertos/queue.h redirect — gmf_core does not use FreeRTOS queues directly
 * (all data-bus operations go through the OAL). This file exists so
 * #include "freertos/queue.h" resolves without error.
 */
#include "freertos/FreeRTOS.h"
