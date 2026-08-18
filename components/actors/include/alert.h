#pragma once
#include <stdint.h>

/* The safety core's report path (M5b Task 5). Actuator alerts -- a close
 * that could not be confirmed, a guard refusal, a command dropped for
 * expiry -- need somewhere to go that:
 *
 *   - producers on several tasks (the NimBLE host task, an esp_timer
 *     callback, the httpd task) can reach WITHOUT blocking (an esp_timer
 *     callback must not block, so this is a critical section, not a
 *     mutex), and
 *   - never touches flash from a task whose stack can't afford it. M5a had
 *     to cut its event-log write entirely because the BLE decoder task's
 *     3072 B stack could not hold event_log_append()'s LittleFS fopen/
 *     fwrite chain plus the SSE/MQTT hooks (~1.5-2 KB). The rules engine
 *     task's stack (8192 B, measured 1636 B free) is the one place sized
 *     for that chain, because it already pays it for its own
 *     event_log_append() call inside psvm_run.
 *
 * So the ring is split in two: alert_post() (this header) is a fixed-size
 * record append under a critical section -- cheap and shallow, safe from
 * any task, nothing that touches the radio may touch flash. alert_drain()
 * is called by the rules engine task alone, at the top of each evaluation
 * pass, and is the only thing that turns a queued alert into an
 * event_log_append() call. */

/* Not exhaustive on purpose -- Tasks 6, 7 and 9 add alert producers (the
 * actor table's guard refusals, the command queue's TTL drop, a safety
 * close that could not be confirmed) and may add codes as they need them;
 * this enum exists so callers don't have to invent ad hoc integers for the
 * codes already known about. `code` travels as a plain uint8_t on the
 * wire (alert_rec_t, alert_post()'s signature) so a later addition here
 * never changes either. */
typedef enum {
    ALERT_CODE_UNKNOWN = 0,        /* refused: unknown device/action */
    ALERT_CODE_BOUND,              /* refused: parameter over bound */
    ALERT_CODE_LOCKOUT,            /* refused: device locked out */
    ALERT_CODE_COOLDOWN,           /* refused: cooldown not elapsed */
    ALERT_CODE_RATE,               /* refused: over the hourly cap */
    ALERT_CODE_COMMAND_EXPIRED,    /* command dropped past its deadline */
    ALERT_CODE_CLOSE_UNCONFIRMED,  /* safety close sent but not confirmed */
} alert_code_t;

#define ALERT_RING_LEN 8

typedef struct {
    uint8_t  level;
    uint8_t  code;         /* alert_code_t */
    int8_t   dev_idx;
    uint8_t  action_id;
    uint16_t param;
} alert_rec_t;

/* Producers -- the NimBLE host task, an esp_timer callback, the httpd task
 * -- only append a fixed-size record. Cheap and shallow: nothing that
 * touches the radio may touch flash.
 *
 * alert_drain() is called by the rules engine task, which is the ONLY task
 * with the stack for event_log_append's LittleFS + SSE + MQTT chain (8192
 * B, measured 1636 B free). M5a had to cut its event-log write entirely
 * because the decoder task's 3072 B could not hold that chain -- this is
 * the same constraint answered by routing rather than by a new task, which
 * the heap budget could not afford anyway. */
void alert_post(uint8_t level, uint8_t code, int dev_idx, uint8_t action_id, uint16_t param);
void alert_drain(void);
