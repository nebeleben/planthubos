#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "swarm_frame.h"

int main(void)
{
    uint8_t buf[300];

    /* v4 header: version, type, len (LE16 = bytes after the header) */
    {
        swarm_checkin_t c = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_CHECKIN,
                              .power_mode = 2, .wake_counter = 12345 };
        uint8_t buf[16];
        size_t n = swarm_encode_checkin(&c, buf, sizeof buf);
        assert(n == 9);
        assert(buf[0] == 4 && buf[1] == SWARM_MSG_CHECKIN);
        assert(buf[2] == 5 && buf[3] == 0);                 /* len = 9 - 4 */
        swarm_checkin_t out;
        assert(swarm_decode_checkin(buf, n, &out));
        buf[2] = 6;                                          /* len lies */
        assert(!swarm_decode_checkin(buf, n, &out));
        buf[2] = 5; buf[0] = 3;                              /* v3 rejected */
        assert(!swarm_decode_checkin(buf, n, &out));
    }

    /* sizes are part of the contract: they must not drift silently.
     * PAIR_ACK grew by 3 bytes (protocol v2's inherited "country" field).
     * PAIR_REQ, PING and PONG all happen to be the same 6-byte shape
     * (version+type+nonce) -- see the type-confusion checks below, which
     * matter precisely because of that overlap. Protocol v4 added a 2-byte
     * `len` field to every frame's header, so every one of these grew by 2.
     * M7 then added `radio_role` to PAIR_REQ alone (after `len`, ahead of
     * the still-load-bearing `nonce` -- see swarm_frame.h), so it no
     * longer shares PING/PONG's size. */
    assert(sizeof(swarm_pair_req_t) == 9);
    assert(sizeof(swarm_pair_ack_t) == 28);
    assert(sizeof(swarm_reading_t) == 25);
    assert(sizeof(swarm_ping_t) == 8);
    assert(sizeof(swarm_pong_t) == 8);
    /* protocol v3 additions */
    assert(sizeof(swarm_forget_t) == 10);  /* +6 for target_mac, closing the "forgets every node" defect */
    /* +4 for session_id on both (M5c hardware round 1 fix: OTA_STATUS became
     * broadcast/plaintext and needs a way to reject a status from a stale or
     * aborted session -- see swarm_frame.h). */
    assert(sizeof(swarm_ota_begin_t) == 60);
    assert(sizeof(swarm_ota_chunk_t) == 10 + SWARM_OTA_CHUNK_DATA);
    assert(sizeof(swarm_ota_status_t) == 14);
    assert(sizeof(swarm_ota_abort_t) == 5);

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
    /* the encoder fills `len` on the wire regardless of what `r.len` held
     * in memory (it was never set above); mirror that into `r` so the
     * full-struct comparison below reflects the encoder's contract rather
     * than an incidental zero-initialized field. */
    r.len = (uint16_t)(n - SWARM_HDR_LEN);
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
    /* Mirror what the hub actually puts in ack.country, not the convenient
     * pre-terminated shape it never produces on the wire: IDF's
     * wifi_country_t.cc is NOT NUL-terminated -- its third octet is the
     * 802.11d "environment" character ('O'/'I'/'X'/' '), so a real
     * esp_wifi_get_country() read-back for Switzerland comes back as
     * {'C','H','O'}, never {'C','H','\0'}. This is exactly the shape that
     * went unexercised until the very next real pairing (existing boards
     * were migrated, not re-paired) and turned into an out-of-bounds read on
     * the node. pairing.c's hub_task() forces ack.country[2] = '\0' right
     * after its memcpy specifically so this never reaches the wire -- mirror
     * that same fix here rather than asserting the un-terminated raw shape. */
    memcpy(ak.country, "CH", 2);
    ak.country[2] = '\0';
    n = swarm_encode_pair_ack(&ak, buf, sizeof(buf));
    assert(n == sizeof(ak));
    swarm_pair_ack_t ak_out;
    assert(swarm_decode_pair_ack(buf, n, &ak_out));
    assert(ak_out.channel == 6 && ak_out.nonce == 0xDEADBEEF);
    assert(memcmp(ak_out.lmk, ak.lmk, SWARM_LMK_LEN) == 0);
    assert(memcmp(ak_out.country, "CH", 3) == 0);

    /* the world-safe "01" default, same story: NUL-terminate before encoding,
     * same as hub_task() does, rather than assuming the raw country-code
     * bytes already end in NUL. */
    memcpy(ak.country, "01", 2);
    ak.country[2] = '\0';
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

    /* --- FORGET round-trip (protocol v3, target_mac added M5c) --- */
    swarm_forget_t forget = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_FORGET,
                               .target_mac = { 0x64, 0xE8, 0x33, 0x88, 0x6A, 0xDC } };
    n = swarm_encode_forget(&forget, buf, sizeof(buf));
    assert(n == sizeof(forget) && swarm_frame_type(buf, n) == SWARM_MSG_FORGET);
    swarm_forget_t forget_out;
    assert(swarm_decode_forget(buf, n, &forget_out));
    assert(memcmp(forget_out.target_mac, forget.target_mac, 6) == 0);

    /* --- OTA_BEGIN round-trip --- */
    swarm_ota_begin_t begin = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_BEGIN,
                                 .session_id = 0xA5A5A5A5u, .total_len = 1048576 };
    for (int i = 0; i < 32; i++) begin.sha256[i] = (uint8_t)(i * 7 + 3);
    memcpy(begin.fw_version, "0.7.0", 6);
    n = swarm_encode_ota_begin(&begin, buf, sizeof(buf));
    assert(n == sizeof(begin) && swarm_frame_type(buf, n) == SWARM_MSG_OTA_BEGIN);
    swarm_ota_begin_t begin_out;
    assert(swarm_decode_ota_begin(buf, n, &begin_out));
    assert(begin_out.session_id == 0xA5A5A5A5u);
    assert(begin_out.total_len == 1048576);
    assert(memcmp(begin_out.sha256, begin.sha256, 32) == 0);
    assert(memcmp(begin_out.fw_version, "0.7.0", 6) == 0);

    /* --- OTA_CHUNK round-trip: full-size chunk --- */
    swarm_ota_chunk_t chunk = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_CHUNK,
                                 .offset = 4096, .len = SWARM_OTA_CHUNK_DATA };
    for (int i = 0; i < SWARM_OTA_CHUNK_DATA; i++) chunk.data[i] = (uint8_t)i;
    n = swarm_encode_ota_chunk(&chunk, buf, sizeof(buf));
    assert(n == 10 + SWARM_OTA_CHUNK_DATA);
    assert(swarm_frame_type(buf, n) == SWARM_MSG_OTA_CHUNK);
    swarm_ota_chunk_t chunk_out;
    assert(swarm_decode_ota_chunk(buf, n, &chunk_out));
    assert(chunk_out.offset == 4096 && chunk_out.len == SWARM_OTA_CHUNK_DATA);
    assert(memcmp(chunk_out.data, chunk.data, SWARM_OTA_CHUNK_DATA) == 0);

    /* --- OTA_CHUNK round-trip: short final chunk, NOT padded on the wire --- */
    swarm_ota_chunk_t last = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_CHUNK,
                                .offset = 1048570, .len = 7 };
    for (int i = 0; i < 7; i++) last.data[i] = (uint8_t)(0x10 + i);
    n = swarm_encode_ota_chunk(&last, buf, sizeof(buf));
    assert(n == 10 + 7);                                /* true size, not padded */
    assert(swarm_frame_type(buf, n) == SWARM_MSG_OTA_CHUNK);
    swarm_ota_chunk_t last_out;
    memset(&last_out, 0xAA, sizeof(last_out));         /* poison to catch stale bytes */
    assert(swarm_decode_ota_chunk(buf, n, &last_out));
    assert(last_out.offset == 1048570 && last_out.len == 7);
    assert(memcmp(last_out.data, last.data, 7) == 0);
    /* bytes past len must be a defined value (zeroed), never leftover/poison */
    for (int i = 7; i < SWARM_OTA_CHUNK_DATA; i++) assert(last_out.data[i] == 0);

    /* --- OTA_STATUS round-trip, one per state --- */
    swarm_ota_status_t st = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_STATUS,
                               .session_id = 0xA5A5A5A5u, .state = OTA_ST_RECEIVING, .err = 0,
                               .next_offset = 12800 };
    n = swarm_encode_ota_status(&st, buf, sizeof(buf));
    assert(n == sizeof(st) && swarm_frame_type(buf, n) == SWARM_MSG_OTA_STATUS);
    swarm_ota_status_t st_out;
    assert(swarm_decode_ota_status(buf, n, &st_out));
    assert(st_out.session_id == 0xA5A5A5A5u);
    assert(st_out.state == OTA_ST_RECEIVING && st_out.next_offset == 12800);
    st.state = OTA_ST_FAILED; st.err = 5;
    n = swarm_encode_ota_status(&st, buf, sizeof(buf));
    assert(swarm_decode_ota_status(buf, n, &st_out));
    assert(st_out.state == OTA_ST_FAILED && st_out.err == 5);

    /* --- OTA_ABORT round-trip --- */
    swarm_ota_abort_t abrt = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_ABORT, .reason = 2 };
    n = swarm_encode_ota_abort(&abrt, buf, sizeof(buf));
    assert(n == sizeof(abrt) && swarm_frame_type(buf, n) == SWARM_MSG_OTA_ABORT);
    swarm_ota_abort_t abrt_out;
    assert(swarm_decode_ota_abort(buf, n, &abrt_out));
    assert(abrt_out.reason == 2);

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

    /* explicit v2 rejection: a real v2 peer's frame (old protocol byte),
     * not just "any future version" -- v3 must refuse to talk to it. */
    n = swarm_encode_reading(&r, buf, sizeof(buf));
    buf[0] = 2;
    assert(swarm_frame_type(buf, n) == -1);
    assert(!swarm_decode_reading(buf, n, &out));
    buf[0] = SWARM_PROTO_VERSION;

    /* --- OTA_CHUNK rejection: the highest-risk decoder in the codebase.
     * swarm_frame_type() only accepts a RANGE for this type; the exact
     * length carried in the `len` field must be independently checked by
     * swarm_decode_ota_chunk() against the real buffer length before any
     * data byte is read. A hostile or buggy sender controls every byte
     * here, including `len` itself. --- */
    {
        enum { hdr = 10 }; /* version+type+hdr_len+offset+len */

        /* declared len too large for the actual (short) buffer: the frame
         * claims a full 200-byte chunk but only hdr+50 bytes were sent. */
        uint8_t raw[hdr + SWARM_OTA_CHUNK_DATA];
        memset(raw, 0, sizeof(raw));
        raw[0] = SWARM_PROTO_VERSION;
        raw[1] = SWARM_MSG_OTA_CHUNK;
        uint16_t declared = SWARM_OTA_CHUNK_DATA;
        memcpy(raw + offsetof(swarm_ota_chunk_t, len), &declared, sizeof(declared));
        size_t short_buf_len = hdr + 50;
        /* v4 generic header: buf[2..3] must equal (buffer length - 4) for
         * swarm_frame_type() to accept it at all -- set it to match
         * short_buf_len (== long_buf_len below) so the OTA_CHUNK-range
         * check, not this generic one, is what's under test here. */
        uint16_t hdr_len_field = (uint16_t)(short_buf_len - SWARM_HDR_LEN);
        memcpy(raw + 2, &hdr_len_field, sizeof(hdr_len_field));
        /* within swarm_frame_type()'s accepted RANGE (still <= hdr+CHUNK_DATA)... */
        assert(swarm_frame_type(raw, short_buf_len) == SWARM_MSG_OTA_CHUNK);
        /* ...but the decoder must still reject it: len says 200, buffer has 50. */
        swarm_ota_chunk_t bad_out;
        assert(!swarm_decode_ota_chunk(raw, short_buf_len, &bad_out));

        /* declared len too small for the actual (longer) buffer: len says 7
         * bytes of data but the buffer actually carries 50. */
        declared = 7;
        memcpy(raw + offsetof(swarm_ota_chunk_t, len), &declared, sizeof(declared));
        size_t long_buf_len = hdr + 50;
        /* long_buf_len == short_buf_len, so the header field set above still matches. */
        assert(swarm_frame_type(raw, long_buf_len) == SWARM_MSG_OTA_CHUNK);
        assert(!swarm_decode_ota_chunk(raw, long_buf_len, &bad_out));

        /* declared len exceeds SWARM_OTA_CHUNK_DATA outright -- must be
         * rejected regardless of what the buffer length claims to match. */
        declared = SWARM_OTA_CHUNK_DATA + 1;
        memcpy(raw + offsetof(swarm_ota_chunk_t, len), &declared, sizeof(declared));
        size_t cap_buf_len = hdr + SWARM_OTA_CHUNK_DATA; /* matches old (wrong) cap only */
        hdr_len_field = (uint16_t)(cap_buf_len - SWARM_HDR_LEN);
        memcpy(raw + 2, &hdr_len_field, sizeof(hdr_len_field));
        assert(!swarm_decode_ota_chunk(raw, cap_buf_len, &bad_out));

        /* truncated buffer: shorter than even the fixed header. Set
         * raw[2..3] to match this buffer's own length first, so it's the
         * chunk-specific range check under test here, not the generic
         * header-length mismatch left over from the block above. */
        size_t trunc_len = hdr - 1;
        hdr_len_field = (uint16_t)(trunc_len - SWARM_HDR_LEN);
        memcpy(raw + 2, &hdr_len_field, sizeof(hdr_len_field));
        assert(swarm_frame_type(raw, trunc_len) == -1);
        assert(!swarm_decode_ota_chunk(raw, trunc_len, &bad_out));

        /* over-long buffer: longer than header + max data. Same fix: match
         * raw[2..3] to this buffer's length so the OTA_CHUNK range check
         * (not the generic header-length check) is what rejects it. */
        size_t overlong_len = hdr + SWARM_OTA_CHUNK_DATA + 1;
        hdr_len_field = (uint16_t)(overlong_len - SWARM_HDR_LEN);
        memcpy(raw + 2, &hdr_len_field, sizeof(hdr_len_field));
        assert(swarm_frame_type(raw, overlong_len) == -1);
    }

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
    assert(swarm_encode_ota_chunk(&chunk, buf, 4) == 0);

    /* encode refuses an in-memory len that already exceeds the cap, even
     * with plenty of output buffer -- callers must not be able to construct
     * an invalid on-wire chunk via the encoder either. */
    swarm_ota_chunk_t oversize = chunk;
    oversize.len = SWARM_OTA_CHUNK_DATA + 1;
    assert(swarm_encode_ota_chunk(&oversize, buf, sizeof(buf)) == 0);

    /* M7: CHECKIN round-trip */
    {
        swarm_checkin_t c = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_CHECKIN,
                              .power_mode = 2, .wake_counter = 12345 };
        uint8_t buf[16];
        size_t n = swarm_encode_checkin(&c, buf, sizeof buf);
        assert(n == 9);
        swarm_checkin_t out;
        assert(swarm_decode_checkin(buf, n, &out));
        assert(out.power_mode == 2 && out.wake_counter == 12345);
        assert(!swarm_decode_checkin(buf, n - 1, &out));       /* short */
        buf[0] = 99;
        assert(!swarm_decode_checkin(buf, n, &out));           /* bad version */
    }
    /* M7: CHECKIN_ACK round-trip */
    {
        swarm_checkin_ack_t a = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_CHECKIN_ACK,
                                  .command = SWARM_CHECKIN_CMD_SET_MODE, .arg = 1 };
        uint8_t buf[8];
        size_t n = swarm_encode_checkin_ack(&a, buf, sizeof buf);
        assert(n == 6);
        swarm_checkin_ack_t out;
        assert(swarm_decode_checkin_ack(buf, n, &out));
        assert(out.command == SWARM_CHECKIN_CMD_SET_MODE && out.arg == 1);
        assert(!swarm_decode_checkin_ack(buf, 3, &out));
        buf[1] = SWARM_MSG_CHECKIN;                            /* wrong type */
        assert(!swarm_decode_checkin_ack(buf, n, &out));
    }

    /* v4: DEVICE_ANNOUNCE round trip, sizes, bounds */
    {
        swarm_device_announce_t a = { .dev = { .kind = 2, .addr = {1,2,3,4,5,6,7,8} },
            .endpoint = 1, .interviewed = 1, .name_len = 5, .name = "Light",
            .cap_count = 2, .cap_ids = {2, 7}, .cap_clusters = {0x0400, 0x0001},
            .action_count = 1, .action_ids = {0} };
        uint8_t buf[80];
        size_t n = swarm_encode_device_announce(&a, buf, sizeof buf);
        /* 4 hdr + 1 kind + 8 addr + 1 ep + 1 iv + 1 + 5 name + 1 + 2*(1+2) + 1 + 1
         * = 4+1+8+1+1+1+5+1+6+1+1 = 30 (the brief's own listed terms sum to 30,
         * not the 32 its assert originally claimed -- arithmetic slip, fixed here). */
        assert(n == 30);
        swarm_device_announce_t o;
        assert(swarm_decode_device_announce(buf, n, &o));
        assert(o.dev.kind == 2 && o.dev.addr[7] == 8 && o.endpoint == 1 && o.interviewed == 1);
        assert(o.name_len == 5 && memcmp(o.name, "Light", 5) == 0);
        assert(o.cap_count == 2 && o.cap_ids[1] == 7 && o.cap_clusters[1] == 0x0001);
        assert(o.action_count == 1 && o.action_ids[0] == 0);
        assert(!swarm_decode_device_announce(buf, n - 1, &o));      /* short */
        uint8_t big[80]; memcpy(big, buf, n); big[15] = 30;           /* name_len > max */
        assert(!swarm_decode_device_announce(big, n, &o));
        memcpy(big, buf, n); big[21] = 5;                             /* cap_count > max */
        assert(!swarm_decode_device_announce(big, n, &o));
        swarm_device_announce_t z = { .dev = { .kind = 2 } };         /* empty lists, empty name */
        n = swarm_encode_device_announce(&z, buf, sizeof buf);
        assert(n == 4 + 1 + 8 + 1 + 1 + 1 + 1 + 1);
        assert(swarm_decode_device_announce(buf, n, &o) && o.cap_count == 0 && o.name_len == 0);
        assert(swarm_encode_device_announce(&a, buf, 10) == 0);       /* cap too small */
    }
    /* v4: DEVICE_GONE */
    {
        swarm_device_gone_t g = { .dev = { .kind = 2, .addr = {9,9,9,9,9,9,9,9} } };
        uint8_t buf[16]; size_t n = swarm_encode_device_gone(&g, buf, sizeof buf);
        assert(n == 13);
        swarm_device_gone_t o; assert(swarm_decode_device_gone(buf, n, &o) && o.dev.addr[0] == 9);
        buf[1] = SWARM_MSG_MEASUREMENT; assert(!swarm_decode_device_gone(buf, n, &o));
    }
    /* v4: MEASUREMENT */
    {
        swarm_measurement_t m = { .dev = { .kind = 2, .addr = {1} }, .cap_id = 2, .value = 16.5f, .age_s = 7 };
        uint8_t buf[32]; size_t n = swarm_encode_measurement(&m, buf, sizeof buf);
        assert(n == 4 + 9 + 1 + 4 + 4);
        swarm_measurement_t o; assert(swarm_decode_measurement(buf, n, &o));
        assert(o.cap_id == 2 && o.value == 16.5f && o.age_s == 7);
        assert(!swarm_decode_measurement(buf, n + 1, &o));
    }
    /* v4: COORD_STATUS */
    {
        swarm_coord_status_t s = { .radio_role = 2, .formed = 1, .channel = 15, .pan_id = 0x489a, .permit_s = 179, .device_count = 3 };
        uint8_t buf[16]; size_t n = swarm_encode_coord_status(&s, buf, sizeof buf);
        assert(n == 4 + 7);
        swarm_coord_status_t o; assert(swarm_decode_coord_status(buf, n, &o) && o.pan_id == 0x489a && o.permit_s == 179);
    }
    /* v4: COMMAND / COMMAND_ACK */
    {
        swarm_command_t c = { .seq = 0x1234, .ttl_s = 30, .op = SWARM_CMD_DEVICE_RENAME,
            .dev = { .kind = 2, .addr = {1,2,3,4,5,6,7,8} }, .arg = 0, .name_len = 3, .name = "Bob" };
        uint8_t buf[64]; size_t n = swarm_encode_command(&c, buf, sizeof buf);
        assert(n == 4 + 2 + 2 + 1 + 9 + 2 + 1 + 3);
        swarm_command_t o; assert(swarm_decode_command(buf, n, &o));
        assert(o.seq == 0x1234 && o.ttl_s == 30 && o.op == SWARM_CMD_DEVICE_RENAME && o.name_len == 3 && o.name[2] == 'b');
        swarm_command_t p = { .seq = 1, .ttl_s = 5, .op = SWARM_CMD_PERMIT_JOIN };
        n = swarm_encode_command(&p, buf, sizeof buf); assert(n == 4 + 2 + 2 + 1 + 9 + 2 + 1);
        swarm_command_ack_t a = { .seq = 0x1234, .op = SWARM_CMD_ACTUATE, .status = SWARM_ACK_DONE, .detail = 0 };
        n = swarm_encode_command_ack(&a, buf, sizeof buf); assert(n == 4 + 5);
        swarm_command_ack_t ao; assert(swarm_decode_command_ack(buf, n, &ao) && ao.status == SWARM_ACK_DONE);
    }
    /* v4: NODE_CONFIG / ACK, PAIR_REQ radio_role */
    {
        swarm_node_config_t c = { .seq = 7, .radio_role = 2 };
        uint8_t buf[16]; size_t n = swarm_encode_node_config(&c, buf, sizeof buf); assert(n == 4 + 3);
        swarm_node_config_t o; assert(swarm_decode_node_config(buf, n, &o) && o.radio_role == 2);
        swarm_node_config_ack_t a = { .seq = 7, .status = SWARM_ACK_ACCEPTED };
        n = swarm_encode_node_config_ack(&a, buf, sizeof buf); assert(n == 4 + 3);
        /* swarm_pair_req_t keeps its pre-existing `nonce` field (still
         * load-bearing in pairing.c's anti-replay matching -- see
         * swarm_frame.h); `radio_role` sits between `len` and `nonce`, so
         * the encoded frame is 4 (header) + 1 (radio_role) + 4 (nonce) = 9,
         * not the header-plus-radio_role-only 5 a nonce-less PAIR_REQ would
         * encode to. */
        swarm_pair_req_t pr = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PAIR_REQ, .radio_role = 2 };
        n = swarm_encode_pair_req(&pr, buf, sizeof buf); assert(n == 9);
        swarm_pair_req_t po; assert(swarm_decode_pair_req(buf, n, &po) && po.radio_role == 2);
    }
    /* v4: POLL -- header only, no body */
    {
        uint8_t pbuf[8];
        size_t n = swarm_encode_poll(pbuf, sizeof pbuf);
        assert(n == 4);
        assert(pbuf[0] == SWARM_PROTO_VERSION && pbuf[1] == SWARM_MSG_POLL);
        assert(pbuf[2] == 0 && pbuf[3] == 0);                 /* len = 0 */
        assert(swarm_frame_type(pbuf, n) == SWARM_MSG_POLL);

        /* A POLL with a non-zero len is rejected -- even a buffer whose own
         * declared len is internally consistent with the bytes supplied
         * (so the generic header check alone would pass it) must still be
         * refused, because POLL's expected_len() demands an exact 4-byte
         * frame. */
        uint8_t bad[5];
        memcpy(bad, pbuf, 4);
        bad[2] = 1; bad[4] = 0xAA;   /* declared len=1, one extra body byte */
        assert(swarm_frame_type(bad, sizeof bad) == -1);
    }

    printf("test_swarm_frame: OK\n");
    return 0;
}
