#pragma once
#include <stdint.h>

/* Pure parser for Tuya's proprietary EF00 "datapoint" report payload. No
 * ESP-IDF -- tests/host/test_zb_tuya.c exercises it directly. See
 * docs/superpowers/specs/2026-09-27-planthub-tuya-ef00-datapoints-design.md
 * for the frame format. */
#define ZB_TUYA_MAX_DPS 12

typedef struct { uint8_t dp_id; uint8_t type; int32_t value; } zb_tuya_dp_t;

/* Parse the EF00 command payload (the bytes after the ZCL header: a
 * [status][seq] pair then repeating [dp_id][type][len:2 BE][value:len BE]).
 * Writes up to `max` datapoints to `out`, returns the count. Integer types
 * (0x01 bool, 0x02 value/int32, 0x04 enum, 0x05 bitmap) yield a signed int;
 * string (0x03) and raw (0x00) DPs are skipped. Stops safely (no over-read)
 * on a truncated frame, returning the DPs parsed so far. */
int zb_tuya_parse(const uint8_t *buf, uint16_t len, zb_tuya_dp_t *out, int max);
