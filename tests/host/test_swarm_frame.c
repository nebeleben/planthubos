#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "swarm_frame.h"

int main(void)
{
    uint8_t buf[64];

    /* sizes are part of the contract: they must not drift silently.
     * PAIR_ACK grew by 3 bytes (protocol v2's inherited "country" field).
     * PAIR_REQ, PING and PONG all happen to be the same 6-byte shape
     * (version+type+nonce) -- see the type-confusion checks below, which
     * matter precisely because of that overlap. */
    assert(sizeof(swarm_pair_req_t) == 6);
    assert(sizeof(swarm_pair_ack_t) == 26);
    assert(sizeof(swarm_reading_t) == 23);
    assert(sizeof(swarm_ping_t) == 6);
    assert(sizeof(swarm_pong_t) == 6);

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
                            .channel = 6, .nonce = 0xDEADBEEF, .country = "CH" };
    for (int i = 0; i < SWARM_LMK_LEN; i++) ak.lmk[i] = (uint8_t)(i * 3 + 1);
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(n == sizeof(ak));
    swarm_pair_ack_t ak_out;
    assert(swarm_decode_pair_ack(buf, n, &ak_out));
    assert(ak_out.channel == 6 && ak_out.nonce == 0xDEADBEEF);
    assert(memcmp(ak_out.lmk, ak.lmk, SWARM_LMK_LEN) == 0);
    assert(memcmp(ak_out.country, "CH", 3) == 0);

    /* the world-safe "01" default round-trips too (third byte is the NUL,
     * not a third country-code character -- see swarm_frame.h) */
    memcpy(ak.country, "01", 3);
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(swarm_decode_pair_ack(buf, n, &ak_out));
    assert(memcmp(ak_out.country, "01", 3) == 0);

    /* --- PING / PONG round-trip (protocol v2 liveness) --- */
    swarm_ping_t ping = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PING, .nonce = 0xC0FFEEu };
    n = swarm_encode_ping(&ping, buf, sizeof(buf));
    assert(n == sizeof(ping) && swarm_frame_type(buf, n) == SWARM_MSG_PING);
    swarm_ping_t ping_out;
    assert(swarm_decode_ping(buf, n, &ping_out) && ping_out.nonce == 0xC0FFEEu);

    swarm_pong_t pong = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PONG, .nonce = 0xC0FFEEu };
    n = swarm_encode_pong(&pong, buf, sizeof(buf));
    assert(n == sizeof(pong) && swarm_frame_type(buf, n) == SWARM_MSG_PONG);
    swarm_pong_t pong_out;
    assert(swarm_decode_pong(buf, n, &pong_out) && pong_out.nonce == 0xC0FFEEu);

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

    /* type confusion: a PAIR_ACK-length buffer must not decode as a reading
     * (also exercises the two types no longer sharing a length in v2) */
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(!swarm_decode_reading(buf, n, &out));

    /* type confusion within same-length frames: PAIR_REQ, PING and PONG are
     * all 6 bytes, so this is the case that actually depends on
     * swarm_frame_type() checking the type byte and not just the length. */
    n = swarm_encode_pair_req(&rq, buf, sizeof(buf));
    assert(swarm_frame_type(buf, n) == SWARM_MSG_PAIR_REQ);
    assert(!swarm_decode_ping(buf, n, &ping_out));
    assert(!swarm_decode_pong(buf, n, &pong_out));

    n = swarm_encode_ping(&ping, buf, sizeof(buf));
    assert(!swarm_decode_pair_req(buf, n, &rq_out));
    assert(!swarm_decode_pong(buf, n, &pong_out));

    /* encode refuses a too-small output buffer */
    assert(swarm_encode_reading(&r, buf, 4) == 0);
    assert(swarm_encode_ping(&ping, buf, 4) == 0);

    printf("test_swarm_frame: OK\n");
    return 0;
}
