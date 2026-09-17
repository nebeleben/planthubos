/* tests/host/test_wrapper_bind.c */
#include "wrapper_bind.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t A[6] = {0xAA,0x01,0x02,0x03,0x04,0x05};
static const uint8_t B[6] = {0xBB,0x01,0x02,0x03,0x04,0x05};

static bool only_7(uint16_t id) { return id == 7; }

int main(void) {
    wrapper_bind_reset();
    assert(wrapper_bind_lookup(A) == 0);          /* empty */
    assert(!wrapper_bind_set(A, 0));              /* id 0 rejected */

    assert(wrapper_bind_set(A, 3));
    assert(wrapper_bind_lookup(A) == 3);
    assert(wrapper_bind_lookup(B) == 0);          /* mac-keyed isolation */
    assert(wrapper_bind_set(A, 7));               /* replace in place */
    assert(wrapper_bind_lookup(A) == 7);

    assert(wrapper_bind_set(B, 4));
    wrapper_bind_t list[WRAPPER_BIND_MAX];
    assert(wrapper_bind_list(list, WRAPPER_BIND_MAX) == 2);

    /* prune: only id 7 survives -> B(id4) dropped */
    assert(wrapper_bind_prune(only_7) == 1);
    assert(wrapper_bind_lookup(A) == 7 && wrapper_bind_lookup(B) == 0);

    assert(wrapper_bind_clear(A));
    assert(!wrapper_bind_clear(A));               /* already gone */
    assert(wrapper_bind_lookup(A) == 0);

    /* full table: fill then a new mac is refused, an existing one still updates */
    wrapper_bind_reset();
    uint8_t m[6] = {0};
    for (int i = 0; i < WRAPPER_BIND_MAX; i++) { m[0]=(uint8_t)i; assert(wrapper_bind_set(m, (uint16_t)(i+1))); }
    uint8_t extra[6] = {0xEE,0,0,0,0,0};
    assert(!wrapper_bind_set(extra, 1));          /* full */
    m[0]=0; assert(wrapper_bind_set(m, 99));       /* existing mac updates even when full */
    assert(wrapper_bind_lookup(m) == 99);

    /* serialize -> deserialize round-trip */
    uint8_t buf[512];
    size_t n = wrapper_bind_serialize(buf, sizeof buf);
    assert(n > 0);
    wrapper_bind_reset();
    assert(wrapper_bind_deserialize(buf, n));
    assert(wrapper_bind_lookup(m) == 99);
    assert(wrapper_bind_list(list, WRAPPER_BIND_MAX) == WRAPPER_BIND_MAX);

    /* corruption + truncation rejected, leaving the table empty */
    buf[n-1] ^= 0xFF; wrapper_bind_reset();
    assert(!wrapper_bind_deserialize(buf, n));
    assert(wrapper_bind_list(list, WRAPPER_BIND_MAX) == 0);
    assert(!wrapper_bind_deserialize(buf, 1));

    printf("test_wrapper_bind: OK\n");
    return 0;
}
