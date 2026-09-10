#pragma once
/* Host-test shim -- see ../esp_err.h's comment. The host test runs single-
 * threaded (no FreeRTOS scheduler), so xSemaphoreTake()/xSemaphoreGive()
 * only need to satisfy data_core.c's call sites, not provide real mutual
 * exclusion. xSemaphoreCreateMutex() returns a fixed non-NULL handle so
 * data_core_init()'s "!s_mutex -> ESP_ERR_NO_MEM" check passes. */
#include <stdint.h>

typedef void *SemaphoreHandle_t;

#define portMAX_DELAY ((uint32_t)0xFFFFFFFFu)

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int dummy_handle;
    return (SemaphoreHandle_t)&dummy_handle;
}

static inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks_to_wait)
{
    (void)sem; (void)ticks_to_wait;
    return 1; /* pdTRUE */
}

static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return 1; /* pdTRUE */
}
