#include <assert.h>
#include <stdio.h>
#include "rules_fsm.h"

int main(void) {
    rules_fsm_state_t s; uint32_t t = 1000;

    /* EDGE: first true fires; staying true does not; false re-arms; true fires again */
    rules_fsm_reset(&s);
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t, true));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+10, true));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+20, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, t+30, true));

    /* EDGE + cooldown: transition during cooldown is suppressed AND stays
       suppressed (no deferred fire) */
    rules_fsm_reset(&s); t = 2000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t, true));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+100, false));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+200, true));  /* cooldown */
    assert(!rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+300, false));
    assert( rules_fsm_should_fire(&s, RULES_MODE_EDGE, 3600, t+3601, true));

    /* LEVEL: every true fires, false never does */
    rules_fsm_reset(&s); t = 9000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t, true));
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t+1, true));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 0, t+2, false));

    /* LEVEL + cooldown: spacing enforced */
    rules_fsm_reset(&s); t = 20000;
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t, true));
    assert(!rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t+59, true));
    assert( rules_fsm_should_fire(&s, RULES_MODE_LEVEL, 60, t+60, true));

    /* reset() semantics: an already-true condition after reboot fires once */
    rules_fsm_reset(&s);
    assert(rules_fsm_should_fire(&s, RULES_MODE_EDGE, 0, 30000, true));

    printf("test_rules_fsm: all passed\n");
    return 0;
}
