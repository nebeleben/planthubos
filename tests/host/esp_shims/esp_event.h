#pragma once
/* Host-test shim -- see esp_err.h's comment in this same directory.
 * ESP_EVENT_DECLARE_BASE/ESP_EVENT_DEFINE_BASE mirror the real macros'
 * shape (an extern esp_event_base_t declared in the header, defined once
 * in the .c). esp_event_post() is a no-op: the host test asserts on
 * data_core's return values and the pending-event ring directly, not on
 * event delivery, and nothing in tests/host/ runs an ESP-IDF event loop to
 * receive it anyway. */
#include <stddef.h>
#include <stdint.h>

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(base) extern esp_event_base_t base
#define ESP_EVENT_DEFINE_BASE(base)  esp_event_base_t base = #base

static inline int esp_event_post(esp_event_base_t event_base, int32_t event_id,
                                  void *event_data, size_t event_data_size,
                                  uint32_t ticks_to_wait)
{
    (void)event_base; (void)event_id; (void)event_data;
    (void)event_data_size; (void)ticks_to_wait;
    return 0;
}
