#pragma once
/* Host-test shim -- see esp_err.h's comment in this same directory. Only
 * esp_timer_get_time() is used by data_core.c, always to derive a
 * now_s = get_time()/1000000 uptime; a real monotonic host clock is a
 * faithful enough stand-in (the host test cares about "moves forward
 * monotonically", not the specific epoch).
 *
 * esp_timer_handle_t (Task 5, zigbee-button-support): rules_internal.h's
 * rule_t struct declares a `timer` field of this type -- an opaque handle,
 * never dereferenced by anything a host test links (rules_resolver.c, the
 * only rules_internal.h consumer any host test compiles) -- so this only
 * needs to be a distinct pointer type, matching the real esp_timer.h's own
 * `typedef struct esp_timer *esp_timer_handle_t` shape closely enough to
 * make rule_t's layout compile. */
#include <stdint.h>
#include <time.h>

typedef struct esp_timer *esp_timer_handle_t;

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}
