#pragma once
#include <stdbool.h>
#include <stdint.h>

/* M2 spec Sec.5: one-time V1->V2 on-disk data wipe, gated on NVS key
 * "data_fmt" (namespace "planthub"). Kept in its own header, separate from
 * app_config.h's other declarations, because data_fmt_decide() below must
 * stay host-testable (tests/host/test_data_fmt.c links data_fmt.c directly
 * with a plain `cc`) -- app_config.h unconditionally pulls in "esp_err.h"
 * for its NVS-backed wifi/hub-name/sensor-name calls, which does not exist
 * outside an ESP-IDF build and would break that host link. None of the four
 * functions below need esp_err_t, so this header has zero ESP-IDF
 * dependencies and app_config.h simply re-exports it (#include below) so
 * existing app_config.h consumers (main.c) see these declarations too. */

#define DATA_FMT_CURRENT 2

/* Pure decision (host-tested): given the stored marker, what to do. */
typedef enum { DATA_FMT_OK = 0, DATA_FMT_WIPE = 1, DATA_FMT_FUTURE = 2 } data_fmt_action_t;
data_fmt_action_t data_fmt_decide(bool present, uint8_t stored);

/* Device side: reads NVS "data_fmt", wipes V1 data when needed, writes the
 * current marker, and latches the UI notice. Returns true if a wipe happened. */
bool data_fmt_apply(const char *storage_base);
/* True while the user has not dismissed the post-wipe notice. */
bool data_fmt_notice_pending(void);
void data_fmt_dismiss_notice(void);

/* NOT part of the task-5 brief's verbatim interface block, but required to
 * wire the spec's FUTURE rule into main.c: "the FUTURE case must NOT wipe
 * and must NOT proceed with data init". data_fmt_apply()'s own bool return
 * only distinguishes "a wipe happened" from "it didn't" -- both DATA_FMT_OK
 * and DATA_FMT_FUTURE return false there, so main.c needs a second signal
 * to tell "nothing to do" apart from "refuse to touch data, downgrade
 * detected". True unless this boot's data_fmt_apply() call found a stored
 * marker newer than DATA_FMT_CURRENT; true before data_fmt_apply() has ever
 * been called (main.c always calls it first, immediately after NVS +
 * LittleFS are up, so no caller can observe that default in practice). */
bool data_fmt_safe(void);
