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
 *
 * is_event (zigbee-button-support): true when the rule's condition depends
 * on a momentary EVENT capability (button.action -- capability.h's
 * cdef->event). A pending press IS a discrete edge, so in EDGE mode an event
 * rule fires once per pending press: it does NOT consult or clear the `armed`
 * latch (the event's consumption at the end of the evaluation pass is the
 * re-arm), only `cooldown` debounces it. This is the fix for the defect where
 * an edge event rule fired on the first press only and never re-armed --
 * there is no observable cond-false pass between two quiet presses to re-arm
 * the classic latch. When is_event is false the classic value-cap edge logic
 * (armed latch, first-ever-true, re-arm on false) is used byte-for-byte.
 * LEVEL mode ignores is_event. Mutates *st (armed/ever_evaluated always,
 * last_fire_ts only when firing; the event edge branch leaves `armed`
 * untouched). */
bool rules_fsm_should_fire(rules_fsm_state_t *st, rules_mode_t mode,
                           uint32_t cooldown_s, uint32_t now_ts, bool cond,
                           bool is_event);

void rules_fsm_reset(rules_fsm_state_t *st);   /* boot/enable state: armed=true */
