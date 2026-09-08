/* memcpy of a packed struct is correct here because both ends of an ESP-NOW
 * link are the same little-endian architecture compiled from this repo, and
 * the version byte guards against future on-wire changes. */
#include "swarm_frame.h"
#include <string.h>

/* OTA_CHUNK is the one variable-length frame (see swarm_frame.h): its
 * encoded size is the fixed header below plus however many data bytes the
 * sender actually declared, never padded to SWARM_OTA_CHUNK_DATA. */
#define SWARM_OTA_CHUNK_HDR    (offsetof(swarm_ota_chunk_t, data))
#define SWARM_OTA_CHUNK_MAXLEN (SWARM_OTA_CHUNK_HDR + SWARM_OTA_CHUNK_DATA)

/* Frames arrive straight off the radio from anyone within range, so every
 * decoder validates version, type and exact length before reading a field. */
static size_t expected_len(int type)
{
    switch (type) {
    case SWARM_MSG_PAIR_REQ:   return sizeof(swarm_pair_req_t);
    case SWARM_MSG_PAIR_ACK:   return sizeof(swarm_pair_ack_t);
    case SWARM_MSG_READING:    return sizeof(swarm_reading_t);
    case SWARM_MSG_PING:       return sizeof(swarm_ping_t);
    case SWARM_MSG_PONG:       return sizeof(swarm_pong_t);
    case SWARM_MSG_FORGET:     return sizeof(swarm_forget_t);
    case SWARM_MSG_CHECKIN:    return sizeof(swarm_checkin_t);
    case SWARM_MSG_CHECKIN_ACK: return sizeof(swarm_checkin_ack_t);
    case SWARM_MSG_OTA_BEGIN:  return sizeof(swarm_ota_begin_t);
    case SWARM_MSG_OTA_STATUS: return sizeof(swarm_ota_status_t);
    case SWARM_MSG_OTA_ABORT:  return sizeof(swarm_ota_abort_t);
    case SWARM_MSG_POLL:       return SWARM_HDR_LEN;   /* header only, no body */
    default:                   return 0;
    }
}

int swarm_frame_type(const uint8_t *buf, size_t len)
{
    if (!buf || len < SWARM_HDR_LEN) return -1;
    if (buf[0] != SWARM_PROTO_VERSION) return -1;
    int type = buf[1];
    uint16_t declared = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if ((size_t)declared + SWARM_HDR_LEN != len) return -1;
    if (type == SWARM_MSG_OTA_CHUNK) {
        /* Range, not exact match -- this is the one frame whose true
         * length depends on a field inside the buffer. The exact check
         * (declared len == actual buffer length, and len <=
         * SWARM_OTA_CHUNK_DATA) is swarm_decode_ota_chunk()'s job, not
         * this function's -- see the header comment on swarm_ota_chunk_t. */
        if (len < SWARM_OTA_CHUNK_HDR || len > SWARM_OTA_CHUNK_MAXLEN) return -1;
        return type;
    }
    if (type >= SWARM_MSG_DEVICE_ANNOUNCE && type <= SWARM_MSG_NODE_CONFIG_ACK) {
        /* Variable-length v4 frames: the decoder validates its counted
         * arrays against len (Task 2); here only the header is checked. */
        return type;
    }
    size_t want = expected_len(type);
    if (want == 0 || len != want) return -1;
    return type;
}

static bool decode_into(const uint8_t *buf, size_t len, int type, void *out, size_t sz)
{
    if (swarm_frame_type(buf, len) != type) return false;
    memcpy(out, buf, sz);
    return true;
}

bool swarm_decode_pair_req(const uint8_t *buf, size_t len, swarm_pair_req_t *out)
{ return decode_into(buf, len, SWARM_MSG_PAIR_REQ, out, sizeof(*out)); }

bool swarm_decode_pair_ack(const uint8_t *buf, size_t len, swarm_pair_ack_t *out)
{ return decode_into(buf, len, SWARM_MSG_PAIR_ACK, out, sizeof(*out)); }

bool swarm_decode_reading(const uint8_t *buf, size_t len, swarm_reading_t *out)
{ return decode_into(buf, len, SWARM_MSG_READING, out, sizeof(*out)); }

bool swarm_decode_ping(const uint8_t *buf, size_t len, swarm_ping_t *out)
{ return decode_into(buf, len, SWARM_MSG_PING, out, sizeof(*out)); }

bool swarm_decode_pong(const uint8_t *buf, size_t len, swarm_pong_t *out)
{ return decode_into(buf, len, SWARM_MSG_PONG, out, sizeof(*out)); }

bool swarm_decode_forget(const uint8_t *buf, size_t len, swarm_forget_t *out)
{ return decode_into(buf, len, SWARM_MSG_FORGET, out, sizeof(*out)); }

bool swarm_decode_checkin(const uint8_t *buf, size_t len, swarm_checkin_t *out)
{ return decode_into(buf, len, SWARM_MSG_CHECKIN, out, sizeof(*out)); }

bool swarm_decode_checkin_ack(const uint8_t *buf, size_t len, swarm_checkin_ack_t *out)
{ return decode_into(buf, len, SWARM_MSG_CHECKIN_ACK, out, sizeof(*out)); }

bool swarm_decode_ota_begin(const uint8_t *buf, size_t len, swarm_ota_begin_t *out)
{ return decode_into(buf, len, SWARM_MSG_OTA_BEGIN, out, sizeof(*out)); }

bool swarm_decode_ota_status(const uint8_t *buf, size_t len, swarm_ota_status_t *out)
{ return decode_into(buf, len, SWARM_MSG_OTA_STATUS, out, sizeof(*out)); }

bool swarm_decode_ota_abort(const uint8_t *buf, size_t len, swarm_ota_abort_t *out)
{ return decode_into(buf, len, SWARM_MSG_OTA_ABORT, out, sizeof(*out)); }

/* The highest-risk decoder in the codebase -- see the swarm_ota_chunk_t
 * comment in swarm_frame.h. swarm_frame_type() has already narrowed `len`
 * to the plausible RANGE [hdr, hdr+SWARM_OTA_CHUNK_DATA]; this function
 * does the exact check that range cannot: read the buffer's own `len`
 * field and verify it (a) is within the data cap, and (b) exactly accounts
 * for every byte of the buffer that was actually supplied -- not merely
 * "no more than". Only once both hold is it safe to copy `declared` bytes
 * out of `data`. */
bool swarm_decode_ota_chunk(const uint8_t *buf, size_t len, swarm_ota_chunk_t *out)
{
    if (swarm_frame_type(buf, len) != SWARM_MSG_OTA_CHUNK) return false;

    uint16_t declared;
    memcpy(&declared, buf + offsetof(swarm_ota_chunk_t, len), sizeof(declared));

    if (declared > SWARM_OTA_CHUNK_DATA) return false;         /* cap */
    if (len != SWARM_OTA_CHUNK_HDR + declared) return false;   /* exact, not >= */

    memset(out, 0, sizeof(*out));
    memcpy(out, buf, SWARM_OTA_CHUNK_HDR);        /* version, type, offset, len */
    memcpy(out->data, buf + SWARM_OTA_CHUNK_HDR, declared);
    return true;
}

static size_t encode_from(const void *in, size_t sz, uint8_t *out, size_t cap)
{
    if (!in || !out || cap < sz || sz < SWARM_HDR_LEN) return 0;
    memcpy(out, in, sz);
    uint16_t body = (uint16_t)(sz - SWARM_HDR_LEN);
    out[2] = (uint8_t)(body & 0xff);
    out[3] = (uint8_t)(body >> 8);
    return sz;
}

size_t swarm_encode_pair_req(const swarm_pair_req_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_pair_ack(const swarm_pair_ack_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_reading(const swarm_reading_t *in, uint8_t *out, size_t cap)
{
    size_t n = encode_from(in, sizeof(*in), out, cap);
    /* _pad is reserved; always zero it on the wire regardless of what the
     * caller left in the struct (decoders intentionally ignore this byte). */
    if (n) out[offsetof(swarm_reading_t, _pad)] = 0;
    return n;
}

size_t swarm_encode_ping(const swarm_ping_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_pong(const swarm_pong_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_forget(const swarm_forget_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_checkin(const swarm_checkin_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_checkin_ack(const swarm_checkin_ack_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_ota_begin(const swarm_ota_begin_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_ota_status(const swarm_ota_status_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

size_t swarm_encode_ota_abort(const swarm_ota_abort_t *in, uint8_t *out, size_t cap)
{ return encode_from(in, sizeof(*in), out, cap); }

/* Mirrors the decoder's contract: refuses to build an on-wire frame whose
 * `len` already violates the cap, and only ever writes header + len bytes
 * -- never the full SWARM_OTA_CHUNK_DATA -- so a short final chunk is
 * genuinely not padded on the wire. */
size_t swarm_encode_ota_chunk(const swarm_ota_chunk_t *in, uint8_t *out, size_t cap)
{
    if (!in || !out) return 0;
    if (in->len > SWARM_OTA_CHUNK_DATA) return 0;
    size_t n = SWARM_OTA_CHUNK_HDR + in->len;
    if (cap < n) return 0;
    memcpy(out, in, SWARM_OTA_CHUNK_HDR);
    memcpy(out + SWARM_OTA_CHUNK_HDR, in->data, in->len);
    /* v4 header: bytes after the 4-byte header, LE -- written directly by
     * byte offset since this frame's own `hdr_len` field only exists to
     * hold these same two bytes in the struct layout (see swarm_frame.h);
     * SWARM_OTA_CHUNK_HDR already accounts for it via offsetof(data). */
    out[2] = (uint8_t)((n - SWARM_HDR_LEN) & 0xff);
    out[3] = (uint8_t)((n - SWARM_HDR_LEN) >> 8);
    return n;
}

/* --- M7 zigbee bridge frames: variable-length, field-serialised (see the
 * comment above swarm_device_announce_t in swarm_frame.h). A small cursor
 * writer/reader pair does the bounds checking once; each encoder/decoder
 * below is just a fixed sequence of field calls in struct-declaration
 * order. */

typedef struct { uint8_t *p; size_t cap; size_t n; bool ok; } wr_t;
static void w8(wr_t *w, uint8_t v)   { if (w->n + 1 > w->cap) { w->ok = false; return; } w->p[w->n++] = v; }
static void w16(wr_t *w, uint16_t v) { w8(w, (uint8_t)v); w8(w, (uint8_t)(v >> 8)); }
static void w32(wr_t *w, uint32_t v) { w16(w, (uint16_t)v); w16(w, (uint16_t)(v >> 16)); }
static void wbytes(wr_t *w, const void *s, size_t n) { if (w->n + n > w->cap) { w->ok = false; return; } memcpy(w->p + w->n, s, n); w->n += n; }
static void whdr(wr_t *w, uint8_t type) { w8(w, SWARM_PROTO_VERSION); w8(w, type); w16(w, 0); }
static size_t wfinish(wr_t *w) { if (!w->ok || w->n < SWARM_HDR_LEN) return 0; uint16_t b = (uint16_t)(w->n - SWARM_HDR_LEN); w->p[2] = (uint8_t)b; w->p[3] = (uint8_t)(b >> 8); return w->n; }

typedef struct { const uint8_t *p; size_t len; size_t i; bool ok; } rd_t;
static uint8_t  r8(rd_t *r)  { if (r->i + 1 > r->len) { r->ok = false; return 0; } return r->p[r->i++]; }
static uint16_t r16(rd_t *r) { uint16_t lo = r8(r); uint16_t hi = r8(r); return (uint16_t)(lo | (hi << 8)); }
static uint32_t r32(rd_t *r) { uint32_t lo = r16(r); uint32_t hi = r16(r); return lo | (hi << 16); }
static void rbytes(rd_t *r, void *d, size_t n) { if (r->i + n > r->len) { r->ok = false; return; } memcpy(d, r->p + r->i, n); r->i += n; }
static bool rbegin(rd_t *r, const uint8_t *buf, size_t len, int type) {
    if (swarm_frame_type(buf, len) != type) return false;
    r->p = buf; r->len = len; r->i = SWARM_HDR_LEN; r->ok = true; return true;
}
static bool rend(const rd_t *r) { return r->ok && r->i == r->len; }   /* no trailing bytes */

static void waddr(wr_t *w, const swarm_dev_addr_t *a) { w8(w, a->kind); wbytes(w, a->addr, SWARM_ADDR_LEN); }
static void raddr(rd_t *r, swarm_dev_addr_t *a) { a->kind = r8(r); rbytes(r, a->addr, SWARM_ADDR_LEN); }

size_t swarm_encode_device_announce(const swarm_device_announce_t *in, uint8_t *out, size_t cap)
{
    if (!in || in->name_len > SWARM_DEV_NAME_MAX || in->cap_count > SWARM_DEV_MAX_CAPS ||
        in->action_count > SWARM_DEV_MAX_ACTIONS) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_DEVICE_ANNOUNCE);
    waddr(&w, &in->dev);
    w8(&w, in->endpoint); w8(&w, in->interviewed);
    w8(&w, in->name_len); wbytes(&w, in->name, in->name_len);
    w8(&w, in->cap_count);
    for (uint8_t i = 0; i < in->cap_count; i++) { w8(&w, in->cap_ids[i]); w16(&w, in->cap_clusters[i]); }
    w8(&w, in->action_count);
    for (uint8_t i = 0; i < in->action_count; i++) w8(&w, in->action_ids[i]);
    return wfinish(&w);
}

bool swarm_decode_device_announce(const uint8_t *buf, size_t len, swarm_device_announce_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_DEVICE_ANNOUNCE)) return false;
    memset(out, 0, sizeof(*out));
    raddr(&r, &out->dev);
    out->endpoint = r8(&r); out->interviewed = r8(&r);
    out->name_len = r8(&r);
    if (out->name_len > SWARM_DEV_NAME_MAX) return false;
    rbytes(&r, out->name, out->name_len);
    out->cap_count = r8(&r);
    if (out->cap_count > SWARM_DEV_MAX_CAPS) return false;
    for (uint8_t i = 0; i < out->cap_count; i++) { out->cap_ids[i] = r8(&r); out->cap_clusters[i] = r16(&r); }
    out->action_count = r8(&r);
    if (out->action_count > SWARM_DEV_MAX_ACTIONS) return false;
    for (uint8_t i = 0; i < out->action_count; i++) out->action_ids[i] = r8(&r);
    return rend(&r);
}

size_t swarm_encode_device_gone(const swarm_device_gone_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_DEVICE_GONE);
    waddr(&w, &in->dev);
    return wfinish(&w);
}

bool swarm_decode_device_gone(const uint8_t *buf, size_t len, swarm_device_gone_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_DEVICE_GONE)) return false;
    memset(out, 0, sizeof(*out));
    raddr(&r, &out->dev);
    return rend(&r);
}

size_t swarm_encode_measurement(const swarm_measurement_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_MEASUREMENT);
    waddr(&w, &in->dev);
    w8(&w, in->cap_id);
    uint32_t vbits; memcpy(&vbits, &in->value, sizeof(vbits));
    w32(&w, vbits);
    w32(&w, in->age_s);
    return wfinish(&w);
}

bool swarm_decode_measurement(const uint8_t *buf, size_t len, swarm_measurement_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_MEASUREMENT)) return false;
    memset(out, 0, sizeof(*out));
    raddr(&r, &out->dev);
    out->cap_id = r8(&r);
    uint32_t vbits = r32(&r);
    memcpy(&out->value, &vbits, sizeof(out->value));
    out->age_s = r32(&r);
    return rend(&r);
}

size_t swarm_encode_coord_status(const swarm_coord_status_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_COORD_STATUS);
    w8(&w, in->radio_role); w8(&w, in->formed); w8(&w, in->channel);
    w16(&w, in->pan_id); w8(&w, in->permit_s); w8(&w, in->device_count);
    return wfinish(&w);
}

bool swarm_decode_coord_status(const uint8_t *buf, size_t len, swarm_coord_status_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_COORD_STATUS)) return false;
    memset(out, 0, sizeof(*out));
    out->radio_role = r8(&r); out->formed = r8(&r); out->channel = r8(&r);
    out->pan_id = r16(&r); out->permit_s = r8(&r); out->device_count = r8(&r);
    return rend(&r);
}

size_t swarm_encode_command(const swarm_command_t *in, uint8_t *out, size_t cap)
{
    if (!in || in->name_len > SWARM_DEV_NAME_MAX) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_COMMAND);
    w16(&w, in->seq); w16(&w, in->ttl_s); w8(&w, in->op);
    waddr(&w, &in->dev);
    w16(&w, in->arg);
    w8(&w, in->name_len); wbytes(&w, in->name, in->name_len);
    return wfinish(&w);
}

bool swarm_decode_command(const uint8_t *buf, size_t len, swarm_command_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_COMMAND)) return false;
    memset(out, 0, sizeof(*out));
    out->seq = r16(&r); out->ttl_s = r16(&r); out->op = r8(&r);
    raddr(&r, &out->dev);
    out->arg = r16(&r);
    out->name_len = r8(&r);
    if (out->name_len > SWARM_DEV_NAME_MAX) return false;
    rbytes(&r, out->name, out->name_len);
    if (out->op == 0 || out->op > SWARM_CMD_RESYNC) return false;
    return rend(&r);
}

size_t swarm_encode_command_ack(const swarm_command_ack_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_COMMAND_ACK);
    w16(&w, in->seq); w8(&w, in->op); w8(&w, in->status); w8(&w, in->detail);
    return wfinish(&w);
}

bool swarm_decode_command_ack(const uint8_t *buf, size_t len, swarm_command_ack_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_COMMAND_ACK)) return false;
    memset(out, 0, sizeof(*out));
    out->seq = r16(&r); out->op = r8(&r); out->status = r8(&r); out->detail = r8(&r);
    return rend(&r);
}

size_t swarm_encode_node_config(const swarm_node_config_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_NODE_CONFIG);
    w16(&w, in->seq); w8(&w, in->radio_role);
    return wfinish(&w);
}

bool swarm_decode_node_config(const uint8_t *buf, size_t len, swarm_node_config_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_NODE_CONFIG)) return false;
    memset(out, 0, sizeof(*out));
    out->seq = r16(&r); out->radio_role = r8(&r);
    return rend(&r);
}

size_t swarm_encode_node_config_ack(const swarm_node_config_ack_t *in, uint8_t *out, size_t cap)
{
    if (!in) return 0;
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_NODE_CONFIG_ACK);
    w16(&w, in->seq); w8(&w, in->status);
    return wfinish(&w);
}

bool swarm_decode_node_config_ack(const uint8_t *buf, size_t len, swarm_node_config_ack_t *out)
{
    rd_t r; if (!out || !rbegin(&r, buf, len, SWARM_MSG_NODE_CONFIG_ACK)) return false;
    memset(out, 0, sizeof(*out));
    out->seq = r16(&r); out->status = r8(&r);
    return rend(&r);
}

/* SWARM_MSG_POLL: header only, no in-memory struct -- unlike every encoder
 * above, there is no `in` to copy fields from, so this just writes the
 * 4-byte v4 header directly with the same wr_t cursor the variable-length
 * M7 frames use. expected_len(SWARM_MSG_POLL) (this file, above) demands
 * an exact 4-byte frame, so a decoded buffer with any trailing byte -- even
 * one whose own `len` field is internally consistent with the buffer it
 * came in -- is rejected by swarm_frame_type() before it ever reaches here. */
size_t swarm_encode_poll(uint8_t *out, size_t cap)
{
    wr_t w = { out, cap, 0, out != NULL };
    whdr(&w, SWARM_MSG_POLL);
    return wfinish(&w);
}
