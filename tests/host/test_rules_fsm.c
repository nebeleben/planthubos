#include <assert.h>
#include <stdio.h>
#include "rules_fsm.h"

int main(void) {
    rules_fsm_state_t s; uint32_t t = 1000;

    /* EDGE (value cap, is_event=false): first true fires; staying true does
       not; false re-arms; true fires again */
    rules_fsm_reset(&s);
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t, true, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+10, true, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+20, false, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+30, true, false));

    /* EDGE + cooldown: transition during cooldown is suppressed AND stays
       suppressed (no deferred fire) */
    rules_fsm_reset(&s); t = 2000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t, true, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+100, false, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+200, true, false));  /* cooldown */
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+300, false, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+3601, true, false));

    /* LEVEL: every true fires, false never does */
    rules_fsm_reset(&s); t = 9000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t, true, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t+1, true, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t+2, false, false));

    /* LEVEL + cooldown: spacing enforced */
    rules_fsm_reset(&s); t = 20000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t, true, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t+59, true, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t+60, true, false));

    /* reset() semantics: an already-true condition after reboot fires once */
    rules_fsm_reset(&s);
    assert(rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, 30000, true, false));

    /* --- Momentary EVENT edge cap (button.action), zigbee-button-support ---
       Each pending press is a discrete edge: a `mode edge` event rule must
       fire once PER press, even with no intervening cond-false pass (there is
       none between two quiet presses). This is the C2 regression: before the
       fix the second press was swallowed by the `armed` latch. */

    /* EVENT edge, cooldown 0: press fires; a SECOND press with NO intervening
       cond-false ALSO fires (the defect: this used to latch after press 1). */
    rules_fsm_reset(&s); t = 40000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t,   true, true));  /* press 1 */
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+5, true, true));  /* press 2, re-fires */
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+9, true, true));  /* press 3, re-fires */

    /* EVENT edge, cooldown 1s: two presses within the window -> second is
       suppressed by cooldown; a press after the window -> fires again. */
    rules_fsm_reset(&s); t = 50000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 1, t,   true, true));  /* fires */
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 1, t,   true, true));  /* same second: cooldown */
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 1, t+1, true, true));  /* window elapsed: fires */

    /* Regression: NON-event edge (is_event=false) still LATCHES -- a press
       fires, an immediate second press (cond true, no cond-false between)
       does NOT fire. Proves classic value-cap edge behavior is unchanged. */
    rules_fsm_reset(&s); t = 60000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t,   true, false));  /* fires */
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+1, true, false));  /* latched */

    printf("test_rules_fsm: all passed\n");
    return 0;
}
