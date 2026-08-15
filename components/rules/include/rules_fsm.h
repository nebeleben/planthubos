#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { RULES_MODE_EDGE = 0, RULES_MODE_LEVEL = 1 } rules_mode_t;

typedef struct {
    bool     armed;          /* edge mode: true = a false-eval has re-armed us */
    bool     ever_evaluated;
    uint32_t last_fire_ts;   /* 0 = never fired */
} rules_fsm_state_t;

/* Pure decision: given this evaluation's condition result, should actions
 * fire? Spec section 1 semantics: edge fires on false->true transition (or
 * first-ever true), re-arms on any false eval; level fires on every true;
 * cooldown_s gates both (spacing from last_fire_ts; 0 = no gate).
 * Mutates *st (armed/ever_evaluated always; last_fire_ts only when firing). */
bool rules_fsm_should_fire(rules_fsm_state_t *st, rules_mode_t mode,
                           uint32_t cooldown_s, uint32_t now_ts, bool cond);

void rules_fsm_reset(rules_fsm_state_t *st);   /* boot/enable state: armed=true */
