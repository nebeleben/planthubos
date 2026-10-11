/* uart_framer.h -- a byte-stream framer around a swarm frame payload.
 * Wire: SYNC0 SYNC1 LEN_LO LEN_HI PAYLOAD[LEN] CRC_LO CRC_HI
 * CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over LEN(2)+PAYLOAD.
 * Pure: no ESP-IDF, so tests/host/test_uart_framer.c exercises it directly. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define UF_SYNC0 0xA5
#define UF_SYNC1 0x5A
#define UF_MAX_PAYLOAD 512
size_t uf_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap);
typedef struct { int st; size_t len, got; uint8_t buf[UF_MAX_PAYLOAD]; uint8_t lenb[2], crcb[2]; unsigned drops; } uf_rx_t;
void uf_rx_init(uf_rx_t *rx);
/* Feed one byte. Returns true when a complete CRC-valid payload is in rx->buf (rx->len bytes).
 * Any error (bad len, bad CRC) silently resyncs: scans for the next SYNC. */
bool uf_rx_byte(uf_rx_t *rx, uint8_t b);
