#include "uart_framer.h"
#include <string.h>
static uint16_t crc16(const uint8_t *p, size_t n, uint16_t c) {
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}
size_t uf_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
    if (len > UF_MAX_PAYLOAD || cap < len + 6) return 0;
    out[0] = UF_SYNC0; out[1] = UF_SYNC1; out[2] = (uint8_t)len; out[3] = (uint8_t)(len >> 8);
    memcpy(out + 4, payload, len);
    uint16_t c = crc16(out + 2, 2, 0xFFFF); c = crc16(payload, len, c);
    out[4 + len] = (uint8_t)c; out[5 + len] = (uint8_t)(c >> 8);
    return len + 6;
}
void uf_rx_init(uf_rx_t *rx) { memset(rx, 0, sizeof *rx); }
static void reset(uf_rx_t *rx) { rx->st = 0; rx->len = rx->got = 0; }
bool uf_rx_byte(uf_rx_t *rx, uint8_t b) {
    switch (rx->st) {
    case 0: if (b == UF_SYNC0) rx->st = 1; return false;
    case 1: if (b == UF_SYNC1) rx->st = 2; else if (b != UF_SYNC0) rx->st = 0; return false;
    case 2: rx->lenb[0] = b; rx->st = 3; return false;
    case 3: rx->lenb[1] = b; rx->len = rx->lenb[0] | ((size_t)rx->lenb[1] << 8);
            if (rx->len == 0 || rx->len > UF_MAX_PAYLOAD) { rx->drops++; reset(rx); return false; }
            rx->got = 0; rx->st = 4; return false;
    case 4: rx->buf[rx->got++] = b; if (rx->got == rx->len) rx->st = 5; return false;
    case 5: rx->crcb[0] = b; rx->st = 6; return false;
    case 6: { rx->crcb[1] = b;
            uint16_t want = rx->crcb[0] | ((uint16_t)rx->crcb[1] << 8);
            uint16_t c = crc16(rx->lenb, 2, 0xFFFF); c = crc16(rx->buf, rx->len, c);
            size_t len = rx->len; reset(rx); rx->len = len;   /* keep len readable for the caller */
            if (c != want) { rx->drops++; rx->len = 0; return false; }
            return true; }
    }
    reset(rx); return false;
}
