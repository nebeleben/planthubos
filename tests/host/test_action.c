/* tests/host/test_action.c */
#include "action.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_table(void) {
    assert(action_get(ACT_IRRIGATION_OPEN) != NULL);
    assert(strcmp(action_get(ACT_IRRIGATION_OPEN)->name, "irrigation.open") == 0);
    assert(action_get(ACT_IRRIGATION_OPEN)->param == ACTION_PARAM_DURATION_S);
    assert(action_get(ACT_IRRIGATION_OPEN)->param_max == 300);
    assert(action_get(ACT_SWITCH_ON)->param == ACTION_PARAM_NONE);
    assert(action_get(ACT_SWITCH_ON)->param_max == 0);
    assert(action_get(ACTION_COUNT) == NULL);
    assert(action_get(0xFF) == NULL);
}

static void test_by_name(void) {
    assert(action_by_name("pump.run")->id == ACT_PUMP_RUN);
    assert(action_by_name("nope") == NULL);
    assert(action_by_name(NULL) == NULL);
}

/* The bound is the whole point: a param above param_max is refused, and an
 * action with no param refuses any nonzero param. */
static void test_param_bound(void) {
    assert(action_param_ok(ACT_IRRIGATION_OPEN, 1));
    assert(action_param_ok(ACT_IRRIGATION_OPEN, 300));
    assert(!action_param_ok(ACT_IRRIGATION_OPEN, 301));
    assert(!action_param_ok(ACT_IRRIGATION_OPEN, 0));      /* a 0s open is not an open */
    assert(action_param_ok(ACT_SWITCH_ON, 0));
    assert(!action_param_ok(ACT_SWITCH_ON, 1));
    assert(!action_param_ok(ACTION_COUNT, 0));
}

int main(void) {
    test_table(); test_by_name(); test_param_bound();
    printf("test_action: all passed\n");
    return 0;
}
