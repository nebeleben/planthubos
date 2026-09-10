#pragma once
/* Host-test shim -- see esp_err.h's comment in this same directory for why
 * this exists. Real printf-style checking (format attribute) is kept so a
 * format/argument mismatch in data_core.c still fails the host build, same
 * as it would under the real esp_log.h. Every level data_core.c actually
 * uses (ESP_LOGW/ESP_LOGD/ESP_LOGI) shares one function so `tag` is always
 * referenced -- avoids an "unused variable" warning on data_core.c's static
 * TAG under -Werror. */
#include <stdarg.h>
#include <stdio.h>

__attribute__((format(printf, 2, 3)))
static inline void esp_log_shim_write(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#define ESP_LOGE(tag, fmt, ...) esp_log_shim_write((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_shim_write((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_shim_write((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_shim_write((tag), (fmt), ##__VA_ARGS__)
