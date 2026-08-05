#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWARM_PROTO_VERSION 3
#define SWARM_LMK_LEN 16

typedef enum {
    SWARM_MSG_PAIR_REQ   = 1,
    SWARM_MSG_PAIR_ACK   = 2,
    SWARM_MSG_READING    = 3,
    SWARM_MSG_PING       = 4,
    SWARM_MSG_PONG       = 5,
    SWARM_MSG_FORGET     = 6,
    SWARM_MSG_OTA_BEGIN  = 7,
    SWARM_MSG_OTA_CHUNK  = 8,
    SWARM_MSG_OTA_STATUS = 9,
    SWARM_MSG_OTA_ABORT  = 10,
    SWARM_MSG_CHECKIN    = 11,
    SWARM_MSG_CHECKIN_ACK = 12,
} swarm_msg_t;

/* Checkin commands (CHECKIN_ACK.command) */
enum {
    SWARM_CHECKIN_CMD_NONE       = 0,
    SWARM_CHECKIN_CMD_SET_MODE   = 1,   /* arg = SWARM_PM_* */
    SWARM_CHECKIN_CMD_STAY_AWAKE = 2,   /* arg = 0 */
};

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
    char     country[3]; /* hub's effective regulatory domain: intended shape is
                             2 chars + NUL, e.g. "CH", or "01" for world-safe --
                             PlanV1 3.3 country inheritance. IMPORTANT: IDF's
                             wifi_country_t.cc (what pairing.c's hub_task() reads
                             this from via esp_wifi_get_country()) is NOT
                             NUL-terminated -- its third octet is the 802.11d
                             "environment" character ('O'/'I'/'X'/' '). hub_task()
                             forces country[2]='\0' after copying specifically so
                             THIS wire field is always a proper C string; do not
                             assume every producer/consumer of this struct gets
                             that for free, and never remove that forced NUL. A
                             receiver still must not trust it blindly (see
                             pairing.c node-side PAIR_ACK handling, which validates
                             country[0]/[1] as alphanumeric and re-forces the NUL
                             itself before use). The hub is associated and may
                             legitimately use 802.11d for its OWN radio, but this
                             field only ever carries whatever it reads back, never
                             a value the node is allowed to derive itself: the node
                             must never enable 802.11d (it never associates, and
                             doing so disabled its transmitter outright -- see
                             espnow_link.c). */
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

/* Hub -> broadcast, plaintext -- same reasoning as PAIR_ACK/PONG above: a
 * forgotten node is no longer an encrypted ESP-NOW peer on the hub's side
 * (the hub deletes the peer entry when it forgets), so a unicast send has
 * no peer to address any more.
 *
 * target_mac (M5c; the original M5a/M5b shape had no target and every node
 * paired to the hub reacted to any FORGET, unpairing all of them instead of
 * just the one an operator actually forgot -- a real defect, not an
 * accepted trade, since it silently strands every OTHER node too). A node
 * now acts on this frame only when BOTH hold: the sender MAC equals its own
 * stored hub MAC (unchanged from before -- rejects a FORGET from anyone
 * else in radio range), AND target_mac equals its own STA MAC (new -- so a
 * node paired to the same hub but NOT the one being forgotten silently
 * ignores it). Broadcast is still the only viable transport (see above),
 * so every paired node still receives every FORGET; target_mac is what
 * makes only the intended one act on it. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t type;
    uint8_t target_mac[6];
} swarm_forget_t;

/* Node -> hub, unicast, encrypted. Battery nodes report their current power mode
 * (SWARM_PM_* from power_modes.h, Task 3), plus a monotonic wake counter for diagnostics. */
typedef struct __attribute__((packed)) {
    uint8_t  version;      /* SWARM_PROTO_VERSION */
    uint8_t  type;         /* SWARM_MSG_CHECKIN */
    uint8_t  power_mode;   /* SWARM_PM_*: the node's CURRENT (reported) mode */
    uint32_t wake_counter; /* monotonic per NVS, diagnostic only */
} swarm_checkin_t;         /* 7 bytes */

/* Hub -> node, unicast, encrypted. Hub's reply to CHECKIN; carries command/arg
 * for the node to execute (e.g., change power mode, stay awake). */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t type;          /* SWARM_MSG_CHECKIN_ACK */
    uint8_t command;       /* SWARM_CHECKIN_CMD_* */
    uint8_t arg;
} swarm_checkin_ack_t;     /* 4 bytes */

/* Hub -> node, unicast, encrypted (an OTA session only ever targets an
 * already-adopted node, i.e. an existing encrypted peer -- unlike pairing
 * or FORGET, which precede or end that relationship). Announces an
 * incoming firmware push; the hub always sources the image from its own
 * running partition, so fw_version is informational for the node/UI, not
 * something the node can be asked to fetch itself.
 *
 * session_id (fix, M5c hardware round 1): a fresh esp_random() value picked
 * once per node_ota_start() call and carried unchanged through every retry
 * of THIS begin (node_ota.c's send_ota_begin() builds the frame once,
 * outside its retry loop). Echoed verbatim in every OTA_STATUS the node
 * sends for this session (below) and checked by node_ota_handle_status()
 * before crediting a status to the hub's current session -- without it, a
 * status frame from an aborted or superseded session (e.g. a stale
 * broadcast still in flight after the hub already gave up and started a new
 * push) could be credited to a session it has nothing to do with. Also lets
 * the node (node_ota_recv.c's handle_begin()) tell a genuine retransmission
 * of the active session's own BEGIN (same session_id) apart from a
 * different BEGIN arriving mid-session. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t session_id;
    uint32_t total_len;
    uint8_t  sha256[32];
    char     fw_version[16];  /* human-readable; encoder/caller must NUL-terminate */
} swarm_ota_begin_t;

#define SWARM_OTA_CHUNK_DATA 200

/* Hub -> node, unicast, encrypted. THE ONE VARIABLE-LENGTH FRAME IN THIS
 * PROTOCOL: every other frame here is a fixed C struct copied byte for
 * byte, which is exactly what lets swarm_frame_type() reject anything of
 * the wrong length before a single field is read. This frame instead
 * carries `len` <= SWARM_OTA_CHUNK_DATA data bytes, and the encoded size on
 * the wire is ONLY header (8 bytes: version+type+offset+len) + len -- a
 * short final chunk is never padded out to SWARM_OTA_CHUNK_DATA.
 *
 * Consequence for the decoder contract: swarm_frame_type() cannot demand an
 * exact length for this type the way it does for every other frame -- it
 * only knows the buffer is *plausibly* an OTA_CHUNK if its length falls in
 * [8, 8+SWARM_OTA_CHUNK_DATA]. The real check -- that the `len` field
 * embedded in the buffer exactly accounts for the buffer's actual length,
 * and that len itself is <= SWARM_OTA_CHUNK_DATA -- is swarm_decode_ota_chunk()'s
 * job, and it MUST run before any byte of `data` is touched. Get either
 * check backwards (e.g. accepting len <= buffer instead of len == buffer,
 * or trusting len before range-checking it against SWARM_OTA_CHUNK_DATA) and
 * a hostile or buggy sender -- anyone in radio range, since this is
 * encrypted but still attacker-controlled plaintext-after-decrypt -- can
 * make the decoder read data bytes that were never actually sent: a buffer
 * overread. This is the highest-risk decoder in the codebase for exactly
 * that reason. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t offset;
    uint16_t len;                       /* <= SWARM_OTA_CHUNK_DATA */
    uint8_t  data[SWARM_OTA_CHUNK_DATA];
} swarm_ota_chunk_t;

/* Node -> BROADCAST, plaintext (fix, M5c hardware round 1; same reasoning as
 * PAIR_ACK/PONG/FORGET above -- PlanV1 8e). Originally unicast/encrypted;
 * the first real hardware OTA showed roughly a third of these going out as
 * a unicast send the node's own radio reported successful, that the hub's
 * application layer never saw -- the exact 802.11-MAC-ack-vs-actual-delivery
 * gap 8e already documents for PAIR_ACK. Broadcasting removes the MAC-ack
 * dependency entirely, and we already know a node's broadcasts reach the
 * hub reliably: PAIR_REQ (also a node broadcast) is how pairing itself
 * works in the first place. DO NOT "optimise" this back to unicast --
 * see PlanV1 8e for the hardware round that lesson already cost once.
 *
 * Necessarily plaintext as a result (ESP-NOW never encrypts broadcast
 * traffic) -- a much smaller trade than PAIR_ACK's, which is the accepted
 * precedent: this frame carries only state/err/next_offset (transfer
 * progress), never key material. session_id (see swarm_ota_begin_t above)
 * plus the hub's own is_paired_node() source-MAC gate (swarm.c's
 * hub_rx_cb()) together mean a spoofed or stray broadcast still cannot be
 * credited to a real session without both a paired node's MAC AND that
 * session's esp_random() session_id.
 *
 * Sent every 64 chunks and at completion so the hub's go-back-N sender
 * knows where to resume after a drop, and at session start/end to report
 * RECEIVING/DONE/FAILED. */
enum {
    OTA_ST_IDLE      = 0,
    OTA_ST_RECEIVING = 1,
    OTA_ST_DONE      = 2,
    OTA_ST_FAILED    = 3,
};

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint32_t session_id;  /* echoed from the OTA_BEGIN that started this session */
    uint8_t  state;
    uint8_t  err;
    uint32_t next_offset; /* the hub clamps this to total_len before trusting it -- see
                            * node_ota_handle_status() -- so a malformed/stale value can
                            * never make the go-back-N sender believe it's past the end. */
} swarm_ota_status_t;

/* Hub -> node, unicast, encrypted. Ends a session early: hub-initiated
 * abort, or the hub giving up after too many stalls/timeouts. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t type;
    uint8_t reason;
} swarm_ota_abort_t;

int  swarm_frame_type(const uint8_t *buf, size_t len);
bool swarm_decode_pair_req(const uint8_t *buf, size_t len, swarm_pair_req_t *out);
bool swarm_decode_pair_ack(const uint8_t *buf, size_t len, swarm_pair_ack_t *out);
bool swarm_decode_reading(const uint8_t *buf, size_t len, swarm_reading_t *out);
bool swarm_decode_ping(const uint8_t *buf, size_t len, swarm_ping_t *out);
bool swarm_decode_pong(const uint8_t *buf, size_t len, swarm_pong_t *out);
bool swarm_decode_forget(const uint8_t *buf, size_t len, swarm_forget_t *out);
bool swarm_decode_checkin(const uint8_t *buf, size_t len, swarm_checkin_t *out);
bool swarm_decode_checkin_ack(const uint8_t *buf, size_t len, swarm_checkin_ack_t *out);
bool swarm_decode_ota_begin(const uint8_t *buf, size_t len, swarm_ota_begin_t *out);
bool swarm_decode_ota_chunk(const uint8_t *buf, size_t len, swarm_ota_chunk_t *out);
bool swarm_decode_ota_status(const uint8_t *buf, size_t len, swarm_ota_status_t *out);
bool swarm_decode_ota_abort(const uint8_t *buf, size_t len, swarm_ota_abort_t *out);
size_t swarm_encode_pair_req(const swarm_pair_req_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_pair_ack(const swarm_pair_ack_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_reading(const swarm_reading_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ping(const swarm_ping_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_pong(const swarm_pong_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_forget(const swarm_forget_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_checkin(const swarm_checkin_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_checkin_ack(const swarm_checkin_ack_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ota_begin(const swarm_ota_begin_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ota_chunk(const swarm_ota_chunk_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ota_status(const swarm_ota_status_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_ota_abort(const swarm_ota_abort_t *in, uint8_t *out, size_t cap);
