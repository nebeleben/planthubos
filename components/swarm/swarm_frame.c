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
    case SWARM_MSG_OTA_BEGIN:  return sizeof(swarm_ota_begin_t);
    case SWARM_MSG_OTA_STATUS: return sizeof(swarm_ota_status_t);
    case SWARM_MSG_OTA_ABORT:  return sizeof(swarm_ota_abort_t);
    default:                   return 0;
    }
}

int swarm_frame_type(const uint8_t *buf, size_t len)
{
    if (!buf || len < 2) return -1;
    if (buf[0] != SWARM_PROTO_VERSION) return -1;
    int type = buf[1];
    if (type == SWARM_MSG_OTA_CHUNK) {
        /* Range, not exact match -- this is the one frame whose true
         * length depends on a field inside the buffer. The exact check
         * (declared len == actual buffer length, and len <=
         * SWARM_OTA_CHUNK_DATA) is swarm_decode_ota_chunk()'s job, not
         * this function's -- see the header comment on swarm_ota_chunk_t. */
        if (len < SWARM_OTA_CHUNK_HDR || len > SWARM_OTA_CHUNK_MAXLEN) return -1;
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
    if (!out || cap < sz) return 0;
    memcpy(out, in, sz);
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
    return n;
}
