#include "bthome.h"
#include "capability.h"
#include <string.h>

#ifndef BTHOME_NO_MBEDTLS_CCM
#include "mbedtls/ccm.h"
#endif

/* BTHome v2 Device Information byte (bthome.io/format):
 *   bit 0     encryption (0 = clear, 1 = AES-CCM encrypted payload follows)
 *   bit 1     reserved
 *   bit 2     trigger-based device flag (irregular vs. regular intervals)
 *   bits 3-4  reserved
 *   bits 5-7  BTHome version (010 = v2) -- not checked: this decoder speaks
 *             the v2 object stream regardless of the advertised version bits,
 *             same tolerance mibeacon.c shows its own frame-control byte. */
#define BTHOME_INFO_ENCRYPTED 0x01

/* AES-CCM tag ("MIC") length BTHome v2 always uses, and the counter field's
 * width -- both fixed by the spec, not carried in the payload. */
#define BTHOME_MIC_LEN     4
#define BTHOME_CTR_LEN     4
#define BTHOME_NONCE_LEN   13   /* 6 (MAC) + 2 (UUID) + 1 (device info) + 4 (counter) */

/* Largest plaintext object stream this decoder will ever need to buffer.
 * A BLE advertisement's payload is at most 31 bytes total (adv_queue.h's
 * ADV_PAYLOAD_MAX); one AD structure's own length+type+UUID overhead (1+1+2)
 * leaves at most 27 bytes for everything after the UUID even in the
 * degenerate case where BTHome is the advert's only AD structure. Sized
 * here, not to ADV_PAYLOAD_MAX, to keep this a small stack buffer -- see
 * BTHOME_ERR_FORMAT's use below for what happens if a payload (a corrupt one;
 * no real advert can exceed 27) claims more. */
#define BTHOME_PLAIN_MAX 27

typedef enum { BT_U8, BT_I8, BT_U16, BT_I16, BT_U24, BT_I24 } bthome_dtype_t;

typedef struct {
    uint8_t         obj_id;
    uint8_t         cap_id;   /* CAP_NONE (0xFF) when known but not emitted --
                                * still needed here purely so its byte width
                                * is known and the object can be skipped
                                * without losing sync on the next id. */
    bthome_dtype_t  dtype;
    float           factor;
} bthome_objdef_t;

/* Object-id table -- BTHome v2's ids are a closed, spec-fixed enumeration
 * (bthome.io/format), so a static const table maps id -> (size, factor) for
 * every id we choose to recognise; entries not in this table are, by
 * construction, either outside that closed set or a real BTHome type this
 * product's 8-capability model (capability.h) has no slot for. Flash
 * (.rodata), not RAM -- see the Task 3 report for the exact byte count.
 *
 * Covers every capability.h id BTHome can plausibly carry (temperature,
 * humidity, illuminance, battery, pressure, soil moisture) plus a handful of
 * common non-plant measurement ids (packet id, mass, dewpoint, count,
 * energy, power, voltage, PM2.5/PM10, CO2, TVOC) purely so a real multi-
 * sensor BTHome device's OTHER fields are skipped cleanly (known size, no
 * capability) instead of derailing the parse of the fields we do want.
 * Ids genuinely outside this table stop the parse (see bthome_decode()'s
 * parse_objects()) rather than guess a width. */
static const bthome_objdef_t OBJ_TABLE[] = {
    { 0x00, CAP_NONE,              BT_U8,  1.0f     },  /* packet id (dedup) */
    { 0x01, CAP_BATTERY_LEVEL,     BT_U8,  1.0f     },
    { 0x02, CAP_AIR_TEMPERATURE,   BT_I16, 0.01f    },
    { 0x03, CAP_AIR_HUMIDITY,      BT_U16, 0.01f    },
    { 0x04, CAP_AIR_PRESSURE,      BT_U24, 0.01f    },
    { 0x05, CAP_LIGHT_ILLUMINANCE, BT_U24, 0.01f    },
    { 0x06, CAP_NONE,              BT_U16, 0.01f    },  /* mass, kg */
    { 0x07, CAP_NONE,              BT_U16, 0.01f    },  /* mass, lb */
    { 0x08, CAP_NONE,              BT_I16, 0.01f    },  /* dewpoint */
    { 0x09, CAP_NONE,              BT_U8,  1.0f     },  /* count */
    { 0x0A, CAP_NONE,              BT_U24, 0.001f   },  /* energy */
    { 0x0B, CAP_NONE,              BT_U24, 0.01f    },  /* power */
    { 0x0C, CAP_NONE,              BT_U16, 0.001f   },  /* voltage */
    { 0x0D, CAP_NONE,              BT_U16, 1.0f     },  /* PM2.5 */
    { 0x0E, CAP_NONE,              BT_U16, 1.0f     },  /* PM10 */
    { 0x12, CAP_NONE,              BT_U16, 1.0f     },  /* CO2 */
    { 0x13, CAP_NONE,              BT_U16, 1.0f     },  /* TVOC */
    { 0x14, CAP_SOIL_MOISTURE,     BT_U16, 0.01f    },  /* moisture, hi-res */
    { 0x2E, CAP_AIR_HUMIDITY,      BT_U8,  1.0f     },  /* humidity, lo-res */
    { 0x2F, CAP_SOIL_MOISTURE,     BT_U8,  1.0f     },  /* moisture, lo-res */
};
#define OBJ_TABLE_COUNT (sizeof(OBJ_TABLE) / sizeof(OBJ_TABLE[0]))

static const bthome_objdef_t *find_objdef(uint8_t obj_id)
{
    for (size_t i = 0; i < OBJ_TABLE_COUNT; i++)
        if (OBJ_TABLE[i].obj_id == obj_id) return &OBJ_TABLE[i];
    return NULL;
}

static size_t dtype_size(bthome_dtype_t t)
{
    switch (t) {
    case BT_U8: case BT_I8:   return 1;
    case BT_U16: case BT_I16: return 2;
    case BT_U24: case BT_I24: return 3;
    }
    return 1;
}

static float decode_value(const bthome_objdef_t *def, const uint8_t *p)
{
    switch (def->dtype) {
    case BT_U8:  return (float)p[0] * def->factor;
    case BT_I8:  return (float)(int8_t)p[0] * def->factor;
    case BT_U16: return (float)((uint32_t)p[0] | ((uint32_t)p[1] << 8)) * def->factor;
    case BT_I16: return (float)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) * def->factor;
    case BT_U24: return (float)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) * def->factor;
    case BT_I24: {
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        if (u & 0x800000u) u |= 0xFF000000u;   /* sign-extend 24 -> 32 */
        return (float)(int32_t)u * def->factor;
    }
    }
    return 0.0f;
}

/* Sequential object-stream parse, shared by the plain and (post-decrypt)
 * encrypted paths. Bounds-checked on every step: reading object id N+1 never
 * happens until object N's full declared width has been confirmed to fit in
 * [p, p+len). An id outside OBJ_TABLE stops the parse (its width is
 * genuinely unknown -- guessing would risk misreading arbitrary trailing
 * bytes as further object ids) but is not an error: whatever was already
 * decoded is kept and BTHOME_OK is returned, matching "unknown ids are
 * skipped, not fatal" for the ids this table cannot even size. */
static bthome_err_t parse_objects(const uint8_t *p, size_t len, bthome_emit_t *out, size_t *n_out)
{
    size_t off = 0;
    while (off < len) {
        uint8_t id = p[off];
        const bthome_objdef_t *def = find_objdef(id);
        if (!def) break;
        size_t sz = dtype_size(def->dtype);
        if (off + 1 + sz > len) return BTHOME_ERR_TRUNCATED;
        if (def->cap_id != CAP_NONE && *n_out < BTHOME_MAX_EMITS) {
            out[*n_out].cap_id = def->cap_id;
            out[*n_out].value = decode_value(def, p + off + 1);
            (*n_out)++;
        }
        off += 1 + sz;
    }
    return BTHOME_OK;
}

bthome_err_t bthome_decode(const uint8_t *data, size_t len, const uint8_t key[16],
                           const uint8_t mac[6], bthome_emit_t *out, size_t *n_out)
{
    *n_out = 0;
    if (len < 1) return BTHOME_ERR_TRUNCATED;

    uint8_t info = data[0];
    bool encrypted = (info & BTHOME_INFO_ENCRYPTED) != 0;

    if (!encrypted) {
        return parse_objects(data + 1, len - 1, out, n_out);
    }

    if (!key) return BTHOME_ERR_ENCRYPTED_NO_KEY;

    /* Encrypted layout (bthome.io/encryption), everything after the device
     * info byte: ciphertext (the plain object stream, encrypted) || counter
     * (4 B, little-endian) || MIC (4 B). */
    if (len < 1 + BTHOME_CTR_LEN + BTHOME_MIC_LEN) return BTHOME_ERR_TRUNCATED;
    size_t ct_len = len - 1 - BTHOME_CTR_LEN - BTHOME_MIC_LEN;
    if (ct_len > BTHOME_PLAIN_MAX) return BTHOME_ERR_FORMAT;  /* not a real advert */

    const uint8_t *ct      = data + 1;
    const uint8_t *ctr_le  = ct + ct_len;
    const uint8_t *mic     = ctr_le + BTHOME_CTR_LEN;

    /* Nonce = MAC (6 B, human/display order -- see bthome.h's mac[] contract)
     * || BTHome UUID (2 B, transmitted-order = little-endian of 0xFCD2, i.e.
     * 0xD2 0xFC) || device info byte (1 B) || counter (4 B, little-endian,
     * the SAME bytes as ctr_le above -- the counter is both authenticated
     * data via the nonce and carried in clear in the payload). Verified
     * against bthome.io's own published nonce example
     * (5448e68f80a5 d2fc 41 33221100), not invented -- see the Task 3
     * report. */
    uint8_t nonce[BTHOME_NONCE_LEN];
    memcpy(nonce, mac, 6);
    nonce[6] = (uint8_t)(BTHOME_SVC_UUID & 0xFF);
    nonce[7] = (uint8_t)((BTHOME_SVC_UUID >> 8) & 0xFF);
    nonce[8] = info;
    memcpy(nonce + 9, ctr_le, BTHOME_CTR_LEN);

#ifdef BTHOME_NO_MBEDTLS_CCM
    (void)ct; (void)mic; (void)nonce;
    return BTHOME_ERR_DECRYPT;   /* host build without mbedtls -- see bthome.h */
#else
    uint8_t plain[BTHOME_PLAIN_MAX];
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        /* No additional authenticated data -- BTHome v2 dropped the v1
         * cipher.update(b"\x11") header (bthome.io/encryption: "In BTHome
         * V2, this header is not used anymore"). */
        rc = mbedtls_ccm_auth_decrypt(&ctx, ct_len, nonce, BTHOME_NONCE_LEN,
                                       NULL, 0, ct, plain, mic, BTHOME_MIC_LEN);
    }
    mbedtls_ccm_free(&ctx);
    if (rc != 0) return BTHOME_ERR_DECRYPT;

    return parse_objects(plain, ct_len, out, n_out);
#endif
}
