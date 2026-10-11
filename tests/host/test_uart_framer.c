/* swarm measurement frames over a UART byte stream. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "swarm_frame.h"
#include "uart_framer.h"

static swarm_measurement_t mk(uint8_t cap, float v, uint16_t cl) {
    swarm_measurement_t m = {0}; m.dev.kind = 2;
    for (int i = 0; i < SWARM_ADDR_LEN; i++) m.dev.addr[i] = (uint8_t)(0x10 + i);
    m.cap_id = cap; m.endpoint = 1; m.value = v; m.age_s = 7; m.source_cluster = cl; return m;
}
static size_t frame_meas(const swarm_measurement_t *m, uint8_t *out, size_t cap) {
    uint8_t pl[256]; size_t n = swarm_encode_measurement(m, pl, sizeof pl); assert(n > 0);
    return uf_encode(pl, n, out, cap);
}
/* feed a byte buffer; collect decoded measurements */
static int feed(uf_rx_t *rx, const uint8_t *b, size_t n, swarm_measurement_t *out, int max) {
    int k = 0;
    for (size_t i = 0; i < n; i++)
        if (uf_rx_byte(rx, b[i]) && k < max) { assert(swarm_decode_measurement(rx->buf, rx->len, &out[k])); k++; }
    return k;
}
static void expect(const swarm_measurement_t *a, uint8_t cap, float v, uint16_t cl) {
    assert(a->cap_id == cap); assert(fabsf(a->value - v) < 1e-4f); assert(a->source_cluster == cl);
    assert(a->dev.kind == 2 && a->dev.addr[0] == 0x10 && a->endpoint == 1 && a->age_s == 7);
}

int main(void) {
    uint8_t w[2048]; swarm_measurement_t got[8]; uf_rx_t rx; size_t n;

    /* 1. round-trip, byte-at-a-time */
    uf_rx_init(&rx); swarm_measurement_t m1 = mk(11, 60.5f, 0x0B04);
    n = frame_meas(&m1, w, sizeof w); assert(n > 0);
    assert(feed(&rx, w, n, got, 8) == 1); expect(&got[0], 11, 60.5f, 0x0B04);
    printf("1 round-trip: ok (%zu wire bytes)\n", n);

    /* 2. three frames back-to-back */
    uf_rx_init(&rx); size_t t = 0;
    swarm_measurement_t ms[3] = { mk(1, 25.0f, 0x0402), mk(5, 59.0f, 0x0405), mk(12, 229.0f, 0x0B04) };
    for (int i = 0; i < 3; i++) t += frame_meas(&ms[i], w + t, sizeof w - t);
    assert(feed(&rx, w, t, got, 8) == 3);
    expect(&got[0], 1, 25.0f, 0x0402); expect(&got[1], 5, 59.0f, 0x0405); expect(&got[2], 12, 229.0f, 0x0B04);
    printf("2 back-to-back x3: ok\n");

    /* 3. corruption: flip a payload byte in frame A, frame B must still decode */
    uf_rx_init(&rx); t = 0;
    size_t a = frame_meas(&ms[0], w, sizeof w); w[a / 2] ^= 0xFF; t = a;
    t += frame_meas(&ms[2], w + t, sizeof w - t);
    int k = feed(&rx, w, t, got, 8); assert(k == 1); expect(&got[0], 12, 229.0f, 0x0B04); assert(rx.drops == 1);
    printf("3 corrupted frame dropped (CRC), next decodes: ok (drops=%u)\n", rx.drops);

    /* 4. truncation: half of frame A, then two good frames */
    uf_rx_init(&rx); t = 0;
    a = frame_meas(&ms[0], w, sizeof w); t = a / 2;                   /* cut A short */
    t += frame_meas(&ms[1], w + t, sizeof w - t);
    t += frame_meas(&ms[2], w + t, sizeof w - t);
    k = feed(&rx, w, t, got, 8);
    /* A's declared LEN swallows B's bytes as "payload", CRC then fails -> B is lost, C decodes. */
    assert(k >= 1); expect(&got[k - 1], 12, 229.0f, 0x0B04);
    printf("4 truncation: recovered, %d of 2 following frames decoded (one may be eaten by the bogus LEN)\n", k);

    /* 5. mid-stream join: garbage (no sync pair) then a good frame */
    uf_rx_init(&rx); t = 0;
    for (int i = 0; i < 40; i++) w[t++] = (uint8_t)(0x31 + (i * 7) % 60);   /* printable junk, never A5 5A */
    t += frame_meas(&ms[1], w + t, sizeof w - t);
    assert(feed(&rx, w, t, got, 8) == 1); expect(&got[0], 5, 59.0f, 0x0405);
    printf("5 mid-stream join after garbage: ok\n");

    /* 6. noise burst INCLUDING a stray sync pair between frames */
    uf_rx_init(&rx); t = 0;
    t += frame_meas(&ms[0], w + t, sizeof w - t);
    uint8_t junk[] = { 0x00, 0xA5, 0x5A, 0x03, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0xEF };  /* fake sync + bogus len 3 + bad crc */
    memcpy(w + t, junk, sizeof junk); t += sizeof junk;
    t += frame_meas(&ms[2], w + t, sizeof w - t);
    k = feed(&rx, w, t, got, 8);
    assert(k == 2); expect(&got[0], 1, 25.0f, 0x0402); expect(&got[1], 12, 229.0f, 0x0B04);
    printf("6 noise burst w/ fake sync between frames: both real frames decoded, junk dropped (drops=%u)\n", rx.drops);

    /* 7. a v5 (old-proto) swarm frame inside a valid UART frame is rejected by the SWARM layer, not the framer */
    uf_rx_init(&rx); uint8_t pl[256]; size_t pn = swarm_encode_measurement(&m1, pl, sizeof pl); pl[0] = 5;
    n = uf_encode(pl, pn, w, sizeof w); int fr = 0; bool any = false;
    for (size_t i = 0; i < n; i++) if (uf_rx_byte(&rx, w[i])) { fr++; any = swarm_decode_measurement(rx.buf, rx.len, &got[0]); }
    assert(fr == 1 && !any);
    printf("7 framer passes a CRC-valid frame; swarm version guard rejects proto v5: ok\n");

    printf("\nALL UART-FRAMER TESTS PASS\n");
    return 0;
}
