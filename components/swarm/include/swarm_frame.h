#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWARM_PROTO_VERSION 2
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
    char     country[3]; /* hub's effective regulatory domain (esp_wifi_get_country(),
                             2 chars + NUL, e.g. "CH", or "01" for world-safe) --
                             PlanV1 3.3 country inheritance. The hub is associated
                             and may legitimately use 802.11d for its OWN radio, but
                             this field only ever carries whatever it reads back, never
                             a value the node is allowed to derive itself: the node
                             must never enable 802.11d (it never associates, and doing
                             so disabled its transmitter outright -- see espnow_link.c). */
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

/* Node -> hub, unicast (the node already holds this hub as an encrypted
 * ESP-NOW peer by the time it ever sends a PING, same as a READING --
 * unlike pairing, which happens before any peer relationship exists). Used
 * by pairing_node_resync_channel() to probe whether a channel actually
 * reaches the hub's ESP-NOW/application layer, not just its radio. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t nonce;   /* echoed in the PONG so the node matches the reply to this probe */
} swarm_ping_t;

/* Hub -> broadcast, plaintext -- same reason as PAIR_ACK (see above): a
 * node mid-resync may not be on the channel its AP association would
 * require for a unicast reply to be deliverable, and is not associated to
 * begin with, so a unicast PONG from the hub is exactly as unreliable as a
 * unicast PAIR_ACK would be. This is the real liveness proof M5a lacked --
 * a delivered PING only proves the hub's radio issued a MAC ack, which can
 * happen even when ESP-NOW itself discards the frame (unknown peer,
 * undecryptable); only a PONG proves the hub's application layer actually
 * processed it. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t nonce;   /* copied verbatim from the PING that triggered this reply */
} swarm_pong_t;

int  swarm_frame_type(const uint8_t *buf, size_t len);
bool swarm_decode_pair_req(const uint8_t *buf, size_t len, swarm_pair_req_t *out);
bool swarm_decode_pair_ack(const uint8_t *buf, size_t len, swarm_pair_ack_t *out);
bool swarm_decode_reading(const uint8_t *buf, size_t len, swarm_reading_t *out);
bool swarm_decode_ping(const uint8_t *buf, size_t len, swarm_ping_t *out);
bool swarm_decode_pong(const uint8_t *buf, size_t len, swarm_pong_t *out);
size_t swarm_encode_pair_req(const swarm_pair_req_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_pair_ack(const swarm_pair_ack_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_reading(const swarm_reading_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ping(const swarm_ping_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_pong(const swarm_pong_t *in, uint8_t *out, size_t cap);
