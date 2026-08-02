#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWARM_PROTO_VERSION 1
#define SWARM_LMK_LEN 16

typedef enum {
    SWARM_MSG_PAIR_REQ = 1,
    SWARM_MSG_PAIR_ACK = 2,
    SWARM_MSG_READING  = 3,
    SWARM_MSG_PING     = 4,
    SWARM_MSG_PONG     = 5,
} swarm_msg_t;

/* Node -> broadcast, plaintext. Sender MAC comes from the ESP-NOW receive
 * metadata, so it is deliberately not repeated in the payload. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t nonce;      /* echoed in the ack so a node ignores stale replies */
} swarm_pair_req_t;

/* Hub -> broadcast, plaintext (see the LMK note in the plan/README).
 * Broadcast, not unicast: an ESP-NOW unicast frame from an AP-associated
 * hub is silently filtered -- and never MAC-acked, i.e. always reports
 * ESP_NOW_SEND_FAIL -- by an unassociated node's radio (confirmed on real
 * hardware; see pairing.c's hub_task()). Broadcasting removes that MAC-ack
 * dependency entirely and is not a security regression: this frame is
 * plaintext by design regardless of addressing, and the node accepts it
 * purely by nonce match against its own just-broadcast PAIR_REQ. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint8_t  channel;    /* the hub's current wifi channel, 1..13 */
    uint8_t  lmk[SWARM_LMK_LEN];
    uint32_t nonce;
} swarm_pair_ack_t;

/* Node -> hub, unicast, encrypted. Absent values use the storage markers. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint8_t  frame_cnt;      /* sensor's MiBeacon counter, for hub-side dedup */
    uint8_t  mac[6];         /* sensor MAC, human order */
    int16_t  temp_dc;
    uint8_t  moisture_pct;
    uint8_t  battery_pct;
    uint32_t lux;
    uint16_t conductivity_us;
    int8_t   rssi;           /* node's RSSI to the sensor (best-node input) */
    uint16_t age_s;          /* seconds since the node heard it */
    uint8_t  _pad;           /* keeps the struct a round 23 bytes; reserved:
                                encoders zero it, decoders ignore it */
} swarm_reading_t;

int  swarm_frame_type(const uint8_t *buf, size_t len);
bool swarm_decode_pair_req(const uint8_t *buf, size_t len, swarm_pair_req_t *out);
bool swarm_decode_pair_ack(const uint8_t *buf, size_t len, swarm_pair_ack_t *out);
bool swarm_decode_reading(const uint8_t *buf, size_t len, swarm_reading_t *out);
size_t swarm_encode_pair_req(const swarm_pair_req_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_pair_ack(const swarm_pair_ack_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_reading(const swarm_reading_t *in, uint8_t *out, size_t cap);
