/* memcpy of a packed struct is correct here because both ends of an ESP-NOW
 * link are the same little-endian architecture compiled from this repo, and
 * the version byte guards against future on-wire changes. */
#include "swarm_frame.h"
#include <string.h>

/* Frames arrive straight off the radio from anyone within range, so every
 * decoder validates version, type and exact length before reading a field. */
static size_t expected_len(int type)
{
    switch (type) {
    case SWARM_MSG_PAIR_REQ: return sizeof(swarm_pair_req_t);
    case SWARM_MSG_PAIR_ACK: return sizeof(swarm_pair_ack_t);
    case SWARM_MSG_READING:  return sizeof(swarm_reading_t);
    case SWARM_MSG_PING:
    case SWARM_MSG_PONG:     return 2;
    default:                 return 0;
    }
}

int swarm_frame_type(const uint8_t *buf, size_t len)
{
    if (!buf || len < 2) return -1;
    if (buf[0] != SWARM_PROTO_VERSION) return -1;
    size_t want = expected_len(buf[1]);
    if (want == 0 || len != want) return -1;
    return (int)buf[1];
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
