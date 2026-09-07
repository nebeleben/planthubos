#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWARM_PROTO_VERSION 4
#define SWARM_HDR_LEN 4      /* version(1) + type(1) + len(2) */
#define SWARM_LMK_LEN 16

/* Protocol v4: every frame on the wire starts with this 4-byte header:
 *   version (1), type (1), len (2, little-endian).
 * len = number of bytes after the 4-byte header, little-endian; filled by
 * the encoder, checked by swarm_frame_type(). Every swarm_*_t struct below
 * carries this header as its first three fields (version, type, len) --
 * swarm_ota_chunk_t is the one exception: it already had a field named
 * `len` for its own data-byte count, so its v4 header-length field is
 * named `hdr_len` instead (see its definition below). */

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
    /* M7 zigbee bridge frame types; bodies land in Task 2, but the enum
     * values must exist now for swarm_frame_type()'s version-check range
     * to compile. */
    SWARM_MSG_DEVICE_ANNOUNCE  = 13,
    SWARM_MSG_DEVICE_GONE      = 14,
    SWARM_MSG_MEASUREMENT      = 15,
    SWARM_MSG_COORD_STATUS     = 16,
    SWARM_MSG_COMMAND          = 17,
    SWARM_MSG_COMMAND_ACK      = 18,
    SWARM_MSG_NODE_CONFIG      = 19,
    SWARM_MSG_NODE_CONFIG_ACK  = 20,
} swarm_msg_t;

/* Checkin commands (CHECKIN_ACK.command) */
enum {
    SWARM_CHECKIN_CMD_NONE       = 0,
    SWARM_CHECKIN_CMD_SET_MODE   = 1,   /* arg = SWARM_PM_* */
    SWARM_CHECKIN_CMD_STAY_AWAKE = 2,   /* arg = 0 */
};

/* Node -> broadcast, plaintext. Sender MAC comes from the ESP-NOW receive
 * metadata, so it is deliberately not repeated in the payload.
 *
 * radio_role (M7): the node's own current radio role (RADIO_ROLE_*, see
 * radio_role.h), reported so the hub can populate reported_radio_role
 * before the node ever sends a CHECKIN/COORD_STATUS. Added after `len`,
 * ahead of `nonce` -- nonce itself is untouched and still load-bearing:
 * pairing.c's pairing_handle_frame() reads req.nonce off every decoded
 * PAIR_REQ and echoes it in PAIR_ACK, which is how a node tells a genuine
 * reply to ITS broadcast apart from another node's concurrent pairing
 * attempt (see the PAIR_ACK comment below). Do not remove it. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
    uint8_t  radio_role;  /* M7: RADIO_ROLE_* the node currently runs */
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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
    uint8_t  version;
    uint8_t  type;
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
    uint8_t  target_mac[6];
} swarm_forget_t;

/* Node -> hub, unicast, encrypted. Battery nodes report their current power mode
 * (SWARM_PM_* from swarm_store.h, Task 3), plus a monotonic wake counter for diagnostics. */
typedef struct __attribute__((packed)) {
    uint8_t  version;      /* SWARM_PROTO_VERSION */
    uint8_t  type;         /* SWARM_MSG_CHECKIN */
    uint16_t len;          /* v4 header: bytes after the 4-byte header, LE */
    uint8_t  power_mode;   /* SWARM_PM_*: the node's CURRENT (reported) mode */
    uint32_t wake_counter; /* monotonic per NVS, diagnostic only */
} swarm_checkin_t;         /* 9 bytes */

/* Hub -> node, unicast, encrypted. Hub's reply to CHECKIN; carries command/arg
 * for the node to execute (e.g., change power mode, stay awake). */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;          /* SWARM_MSG_CHECKIN_ACK */
    uint16_t len;           /* v4 header: bytes after the 4-byte header, LE */
    uint8_t  command;       /* SWARM_CHECKIN_CMD_* */
    uint8_t  arg;
} swarm_checkin_ack_t;     /* 6 bytes */

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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
 * the wire is ONLY header (10 bytes: version+type+hdr_len+offset+len) + len
 * -- a short final chunk is never padded out to SWARM_OTA_CHUNK_DATA.
 *
 * Consequence for the decoder contract: swarm_frame_type() cannot demand an
 * exact length for this type the way it does for every other frame -- it
 * only knows the buffer is *plausibly* an OTA_CHUNK if its length falls in
 * [10, 10+SWARM_OTA_CHUNK_DATA]. The real check -- that the `len` field
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
    uint16_t hdr_len;                   /* v4 header: bytes after the 4-byte
                                            header (offset + len + data), LE.
                                            Distinct from `len` below, which
                                            counts only the data bytes --
                                            this struct already used that
                                            name before v4, so the new
                                            generic header field could not
                                            reuse it. Filled by the encoder,
                                            checked by swarm_frame_type(). */
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
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
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
    uint8_t  version;
    uint8_t  type;
    uint16_t len;         /* v4 header: bytes after the 4-byte header, LE */
    uint8_t  reason;
} swarm_ota_abort_t;

/* M7 zigbee bridge frames (v4, types 13-20). Unlike every frame above,
 * these are variable-length: each carries one or more counted arrays
 * (name, cap list, action list) whose actual length depends on the data,
 * not the struct layout. So none of these are __attribute__((packed)) wire
 * structs copied byte for byte -- they are plain in-memory structs with NO
 * header fields at all; their wire form is produced field-by-field by the
 * matching swarm_encode_device_announce() / swarm_decode_device_announce()
 * style function pair in swarm_frame.c (see its wr_t/rd_t cursor helpers),
 * the same way swarm_ota_chunk_t's `data` payload already has to be handled
 * specially. Keep it that way: do not add __attribute__((packed)) or a
 * version/type/len header to these -- the header only exists on the wire,
 * written by whdr()/wfinish(). */

#define SWARM_ADDR_LEN 8
#define SWARM_DEV_NAME_MAX 24
#define SWARM_DEV_MAX_CAPS 4
#define SWARM_DEV_MAX_ACTIONS 2

/* Command ops (swarm_command_t.op) */
enum { SWARM_CMD_PERMIT_JOIN = 1, SWARM_CMD_DEVICE_REMOVE = 2, SWARM_CMD_DEVICE_RENAME = 3,
       SWARM_CMD_ACTUATE = 4, SWARM_CMD_RESYNC = 5 };
/* Ack status (swarm_command_ack_t.status, swarm_node_config_ack_t.status) */
enum { SWARM_ACK_ACCEPTED = 1, SWARM_ACK_DONE = 2, SWARM_ACK_FAILED = 3 };

/* A zigbee end device's address: its short/group "kind" byte plus its
 * 8-byte IEEE address. Shared by every M7 frame that names a device. */
typedef struct { uint8_t kind; uint8_t addr[SWARM_ADDR_LEN]; } swarm_dev_addr_t;

/* Bridge -> hub. A newly interviewed (or re-announced) zigbee end device:
 * its address, endpoint, interview state, human name, and the capabilities
 * (cap_id + zigbee cluster id pairs) and actions it exposes. */
typedef struct {                       /* in-memory form; wire form is field-serialised */
    swarm_dev_addr_t dev;
    uint8_t  endpoint;
    uint8_t  interviewed;
    uint8_t  name_len;  char name[SWARM_DEV_NAME_MAX];
    uint8_t  cap_count; uint8_t cap_ids[SWARM_DEV_MAX_CAPS]; uint16_t cap_clusters[SWARM_DEV_MAX_CAPS];
    uint8_t  action_count; uint8_t action_ids[SWARM_DEV_MAX_ACTIONS];
} swarm_device_announce_t;

/* Bridge -> hub. A previously announced device has left the network. */
typedef struct { swarm_dev_addr_t dev; } swarm_device_gone_t;

/* Bridge -> hub. One capability reading from one zigbee end device. */
typedef struct { swarm_dev_addr_t dev; uint8_t cap_id; float value; uint32_t age_s; } swarm_measurement_t;

/* Bridge -> hub. The bridge node's zigbee coordinator status (network
 * formed, channel/PAN, permit-join countdown, device count). */
typedef struct { uint8_t radio_role; uint8_t formed; uint8_t channel; uint16_t pan_id; uint8_t permit_s; uint8_t device_count; } swarm_coord_status_t;

/* Hub -> bridge. Directs the bridge's zigbee coordinator to act on a
 * device (permit-join, remove, rename, actuate) or resync its state. */
typedef struct { uint16_t seq; uint16_t ttl_s; uint8_t op; swarm_dev_addr_t dev; uint16_t arg; uint8_t name_len; char name[SWARM_DEV_NAME_MAX]; } swarm_command_t;

/* Bridge -> hub. Reply to a COMMAND, correlated by seq. */
typedef struct { uint16_t seq; uint8_t op; uint8_t status; uint8_t detail; } swarm_command_ack_t;

/* Hub -> node. Directs a node to switch its radio_role (BLE relay <->
 * zigbee bridge) and reboot into it; see radio_role.h. */
typedef struct { uint16_t seq; uint8_t radio_role; } swarm_node_config_t;

/* Node -> hub. Reply to a NODE_CONFIG, correlated by seq. */
typedef struct { uint16_t seq; uint8_t status; } swarm_node_config_ack_t;

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

/* M7 zigbee bridge frames (variable-length, field-serialised -- see the
 * comment above swarm_device_announce_t). */
bool swarm_decode_device_announce(const uint8_t *buf, size_t len, swarm_device_announce_t *out);
bool swarm_decode_device_gone(const uint8_t *buf, size_t len, swarm_device_gone_t *out);
bool swarm_decode_measurement(const uint8_t *buf, size_t len, swarm_measurement_t *out);
bool swarm_decode_coord_status(const uint8_t *buf, size_t len, swarm_coord_status_t *out);
bool swarm_decode_command(const uint8_t *buf, size_t len, swarm_command_t *out);
bool swarm_decode_command_ack(const uint8_t *buf, size_t len, swarm_command_ack_t *out);
bool swarm_decode_node_config(const uint8_t *buf, size_t len, swarm_node_config_t *out);
bool swarm_decode_node_config_ack(const uint8_t *buf, size_t len, swarm_node_config_ack_t *out);
size_t swarm_encode_device_announce(const swarm_device_announce_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_device_gone(const swarm_device_gone_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_measurement(const swarm_measurement_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_coord_status(const swarm_coord_status_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_command(const swarm_command_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_command_ack(const swarm_command_ack_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_node_config(const swarm_node_config_t *in, uint8_t *out, size_t cap);
size_t swarm_encode_node_config_ack(const swarm_node_config_ack_t *in, uint8_t *out, size_t cap);
