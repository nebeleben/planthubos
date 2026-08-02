#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "swarm_frame.h"

int main(void)
{
    uint8_t buf[64];

    /* sizes are part of the contract: they must not drift silently */
    assert(sizeof(swarm_pair_req_t) == 6);
    assert(sizeof(swarm_pair_ack_t) == 23);
    assert(sizeof(swarm_reading_t) == 23);

    /* --- reading round-trip --- */
    swarm_reading_t r = {
        .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_READING,
        .frame_cnt = 42, .mac = { 0x80, 0xEA, 0xCA, 0x89, 0x2A, 0x0A },
        .temp_dc = 301, .moisture_pct = 20, .battery_pct = 0xFF,
        .lux = 4116, .conductivity_us = 131, .rssi = -67, .age_s = 3,
    };
    size_t n = swarm_encode_reading(&r, buf, sizeof(buf));
    assert(n == sizeof(swarm_reading_t));
    assert(swarm_frame_type(buf, n) == SWARM_MSG_READING);
    swarm_reading_t out;
    assert(swarm_decode_reading(buf, n, &out));
    assert(memcmp(&r, &out, sizeof(r)) == 0);

    /* negative temperature and absent markers survive */
    r.temp_dc = -51; r.lux = 0xFFFFFFFFu; r.conductivity_us = 0xFFFF; r.moisture_pct = 0xFF;
    n = swarm_encode_reading(&r, buf, sizeof(buf));
    assert(swarm_decode_reading(buf, n, &out));
    assert(out.temp_dc == -51 && out.lux == 0xFFFFFFFFu);
    assert(out.conductivity_us == 0xFFFF && out.moisture_pct == 0xFF);

    /* --- pair req / ack round-trip --- */
    swarm_pair_req_t rq = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PAIR_REQ, .nonce = 0xDEADBEEF };
    n = swarm_encode_pair_req(&rq, buf, sizeof(buf));
    assert(n == sizeof(rq) && swarm_frame_type(buf, n) == SWARM_MSG_PAIR_REQ);
    swarm_pair_req_t rq_out;
    assert(swarm_decode_pair_req(buf, n, &rq_out) && rq_out.nonce == 0xDEADBEEF);

    swarm_pair_ack_t ak = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PAIR_ACK,
                            .channel = 6, .nonce = 0xDEADBEEF };
    for (int i = 0; i < SWARM_LMK_LEN; i++) ak.lmk[i] = (uint8_t)(i * 3 + 1);
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(n == sizeof(ak));
    swarm_pair_ack_t ak_out;
    assert(swarm_decode_pair_ack(buf, n, &ak_out));
    assert(ak_out.channel == 6 && ak_out.nonce == 0xDEADBEEF);
    assert(memcmp(ak_out.lmk, ak.lmk, SWARM_LMK_LEN) == 0);

    /* --- rejection: this decoder faces raw radio from anyone --- */
    n = swarm_encode_reading(&r, buf, sizeof(buf));
    buf[0] = SWARM_PROTO_VERSION + 1;                 /* future version */
    assert(swarm_frame_type(buf, n) == -1);
    assert(!swarm_decode_reading(buf, n, &out));
    buf[0] = SWARM_PROTO_VERSION;
    buf[1] = 99;                                       /* unknown type */
    assert(swarm_frame_type(buf, n) == -1);
    buf[1] = SWARM_MSG_READING;
    assert(swarm_frame_type(buf, n - 1) == -1);        /* truncated */
    assert(!swarm_decode_reading(buf, n - 1, &out));
    assert(swarm_frame_type(buf, n + 1) == -1);        /* over-long */
    assert(swarm_frame_type(buf, 0) == -1);            /* empty */
    assert(swarm_frame_type(buf, 1) == -1);            /* header only */

    /* type confusion: a PAIR_ACK-length buffer must not decode as a reading */
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(!swarm_decode_reading(buf, n, &out));

    /* encode refuses a too-small output buffer */
    assert(swarm_encode_reading(&r, buf, 4) == 0);

    printf("test_swarm_frame: OK\n");
    return 0;
}
