#include "bindkey_core.h"
#include <stdio.h>
#include <string.h>

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

void bindkey_nvs_key_for(const char *dev_id, char out[BINDKEY_NVS_KEY_BUF])
{
    /* 15 hex digits = 60 bits; the top 4 bits of the 64-bit hash are simply
     * dropped rather than folded in -- NVS only ever sees a 15-char string,
     * so there is no room for them regardless. */
    uint64_t h = fnv1a64(dev_id) & ((1ULL << 60) - 1);
    snprintf(out, BINDKEY_NVS_KEY_BUF, "%015llx", (unsigned long long)h);
}

void bindkey_blob_build(const char *dev_id, const uint8_t key[BINDKEY_LEN], bindkey_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    memcpy(blob->key, key, BINDKEY_LEN);
    strncpy(blob->dev_id, dev_id, BINDKEY_DEVID_MAX - 1);
    blob->dev_id[BINDKEY_DEVID_MAX - 1] = '\0';
}

bool bindkey_blob_verify(const char *dev_id, const bindkey_blob_t *blob, uint8_t key_out[BINDKEY_LEN])
{
    /* blob->dev_id is always NUL-terminated by bindkey_blob_build() (the
     * only writer), but this is also the boundary that reads back whatever
     * NVS actually gave us -- bounded, not strcmp(), on principle. */
    if (strncmp(blob->dev_id, dev_id, BINDKEY_DEVID_MAX) != 0) return false;
    if (key_out) memcpy(key_out, blob->key, BINDKEY_LEN);
    return true;
}
