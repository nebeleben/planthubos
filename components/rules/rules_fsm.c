#include "rules_fsm.h"

void rules_fsm_reset(rules_fsm_state_t *st) {
    st->armed = true;
    st->ever_evaluated = false;
    st->last_fire_ts = 0;
}

bool rules_fsm_should_fire(rules_fsm_state_t *st, rules_mode_t mode,
                           uint32_t cooldown_s, uint32_t now_ts, bool cond,
                           bool is_event) {
    /* Check if we're within cooldown window */
    bool in_cooldown = (cooldown_s > 0) && (st->last_fire_ts != 0) &&
                       (now_ts - st->last_fire_ts < cooldown_s);

    bool should_fire = false;

    if (mode == RULES_MODE_EDGE && is_event) {
        /* Event edge (button.action): each pending press is its own discrete
         * momentary edge, so a true condition fires once per press subject only
         * to cooldown -- the `armed` latch is neither consulted nor cleared
         * (there is no cond-false pass between two quiet presses to re-arm it;
         * the press's consumption at the end of the evaluation pass is the
         * re-arm). This is what lets a `mode edge` button rule re-fire on every
         * press instead of only the first. */
        should_fire = (cond && !in_cooldown);
    } else if (mode == RULES_MODE_EDGE) {
        /* Edge: fire on false->true transition (or first-ever true); re-arm on false */
        if (!st->ever_evaluated && cond) {
            /* First evaluation and condition is true */
            should_fire = true;
            st->armed = false;
        } else if (cond) {
            /* Condition is true; fire if armed and not in cooldown */
            if (st->armed && !in_cooldown) {
                should_fire = true;
                st->armed = false;
            }
        } else {
            /* Condition is false; re-arm for next transition */
            st->armed = true;
        }
    } else {
        /* LEVEL: fire on every true (if not in cooldown) */
        if (cond && !in_cooldown) {
            should_fire = true;
        }
    }

    st->ever_evaluated = true;

    if (should_fire) {
        st->last_fire_ts = now_ts;
    }

    return should_fire;
}
