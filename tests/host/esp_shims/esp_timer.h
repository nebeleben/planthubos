#pragma once
/* Host-test shim -- see esp_err.h's comment in this same directory. Only
 * esp_timer_get_time() is used by data_core.c, always to derive a
 * now_s = get_time()/1000000 uptime; a real monotonic host clock is a
 * faithful enough stand-in (the host test cares about "moves forward
 * monotonically", not the specific epoch). */
#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}
