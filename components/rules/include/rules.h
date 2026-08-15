#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "psvm.h"

#define RULES_MAX        32
#define RULES_SRC_MAX    4096
#define RULES_PSBC_MAX   2048
#define RULES_NAME_MAX   48

typedef struct {
    uint32_t id;
    char     name[RULES_NAME_MAX + 1];
    bool     enabled;
    uint8_t  mode;           /* rules_mode_t */
    uint32_t cooldown_s, every_s;
    /* live status */
    bool     ready; char not_ready_reason[48];
    psvm_err_t last_err;
    /* MONOTONIC uptime seconds (esp_timer_get_time()/1000000), never epoch --
     * epoch jumps on time sync and would break cooldown gating (spec §1's
     * cooldown/every semantics, and rules_fsm.h's now_ts contract). Epoch is
     * used ONLY for event_log timestamps, which event_log_append() derives
     * itself; the API layer can convert these to epoch later if it needs to. */
    uint32_t last_eval_ts, last_fire_ts, fire_count;
} rule_info_t;

void   rules_init(void);                     /* loads store, starts engine task */
size_t rules_list(rule_info_t *out, size_t max);
bool   rules_get_source(uint32_t id, char *buf, size_t buflen);
/* Create (id_inout==0) or update; validates bytecode via psvm_validate and
 * meta limits. Returns ESP_OK / ESP_ERR_INVALID_ARG (reason into errbuf) /
 * ESP_ERR_NO_MEM (table full). */
int    rules_upsert(uint32_t *id_inout, const char *name, const char *source,
                    const uint8_t *psbc, size_t psbc_len, bool enabled,
                    uint8_t mode, uint32_t cooldown_s, uint32_t every_s,
                    char *errbuf, size_t errlen);
bool   rules_delete(uint32_t id);
bool   rules_set_enabled(uint32_t id, bool enabled);
/* Dry-run: evaluate now; fills refs/action captures for the /test endpoint. */
typedef struct { char ref_desc[64]; float value; uint32_t age_s; bool ready; } rules_test_ref_t;
typedef struct { uint8_t builtin; char msg[128]; } rules_test_action_t;
int    rules_test(uint32_t id, bool *ready, bool *cond, bool *would_fire,
                  rules_test_ref_t *refs, size_t *nrefs,
                  rules_test_action_t *acts, size_t *nacts);
void   rules_notify_value_update(void);      /* ISR-unsafe-free: sets event bit */
