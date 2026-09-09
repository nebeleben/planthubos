#pragma once
/* Host-test shim -- see ../esp_err.h's comment. data_core.c includes this
 * header but never uses anything declared directly in it (only
 * freertos/semphr.h's SemaphoreHandle_t, xSemaphoreTake/Give and
 * portMAX_DELAY), so there is nothing to stub here. */
