#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bindkey_core.h"

int main(void)
{
    /* --- hash uses the full 15-char budget, not a truncated 8-char one --- */
    {
        char nvs_key[BINDKEY_NVS_KEY_BUF];
        bindkey_nvs_key_for("ble:AABBCCDDEEFF", nvs_key);
        assert(strlen(nvs_key) == BINDKEY_NVS_KEY_BUF - 1);   /* 15 hex chars */
        for (size_t i = 0; nvs_key[i]; i++) {
            char c = nvs_key[i];
            assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        /* deterministic: same dev_id always hashes to the same key name */
        char again[BINDKEY_NVS_KEY_BUF];
        bindkey_nvs_key_for("ble:AABBCCDDEEFF", again);
        assert(strcmp(nvs_key, again) == 0);
        /* different dev_id -> (almost certainly) a different key name */
        char other[BINDKEY_NVS_KEY_BUF];
        bindkey_nvs_key_for("ble:112233445566", other);
        assert(strcmp(nvs_key, other) != 0);
    }

    /* --- set/get round trip returns the right key --- */
    {
        const uint8_t key_a[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        bindkey_blob_t blob;
        bindkey_blob_build("ble:AABBCCDDEEFF", key_a, &blob);
        assert(strcmp(blob.dev_id, "ble:AABBCCDDEEFF") == 0);
        assert(memcmp(blob.key, key_a, 16) == 0);

        uint8_t out[16];
        memset(out, 0xAA, sizeof out);
        assert(bindkey_blob_verify("ble:AABBCCDDEEFF", &blob, out) == true);
        assert(memcmp(out, key_a, 16) == 0);
    }

    /* --- a blob whose stored dev-id does not match the requested one is
     * rejected as "no key" (this is the exact check bindkey.c's
     * read_verified_blob() runs against whatever NVS handed back) --- */
    {
        const uint8_t key_a[16] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                     0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
        bindkey_blob_t blob_for_a;
        bindkey_blob_build("ble:AAAAAAAAAAAA", key_a, &blob_for_a);

        uint8_t out[16];
        memset(out, 0x00, sizeof out);
        assert(bindkey_blob_verify("ble:BBBBBBBBBBBB", &blob_for_a, out) == false);
        /* key_out must be untouched on a mismatch -- never a partial/wrong key */
        for (int i = 0; i < 16; i++) assert(out[i] == 0x00);

        /* the matching id still succeeds against the same blob */
        assert(bindkey_blob_verify("ble:AAAAAAAAAAAA", &blob_for_a, out) == true);
        assert(memcmp(out, key_a, 16) == 0);
    }

    /* --- a hashed-name collision cannot silently yield another device's
     * key: simulate two device ids that hashed to the SAME NVS slot (the
     * scenario bindkey.c's read_verified_blob()/bindkey_set() guard
     * against) by writing device A's blob and then asking the verify
     * helper -- the actual boundary that decides whether a key is handed
     * back -- whether it belongs to device B. It must refuse, exactly as
     * if B had no key at all, never handing back A's key material. --- */
    {
        const uint8_t key_a[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                     0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };
        bindkey_blob_t slot;   /* whatever NVS returns for the shared hashed name */
        bindkey_blob_build("ble:0000000000AA", key_a, &slot);

        uint8_t out_for_b[16];
        memset(out_for_b, 0x42, sizeof out_for_b);
        bool b_got_a_key = bindkey_blob_verify("ble:0000000000BB", &slot, out_for_b);
        assert(b_got_a_key == false);
        assert(memcmp(out_for_b, key_a, 16) != 0);   /* B must never see A's key bytes */

        /* device A itself still reads its own key back fine from that
         * same slot -- the collision only ever costs the LOSER (whichever
         * id didn't actually write the slot), never hands out wrong data. */
        uint8_t out_for_a[16];
        assert(bindkey_blob_verify("ble:0000000000AA", &slot, out_for_a) == true);
        assert(memcmp(out_for_a, key_a, 16) == 0);
    }

    /* --- the longest string device_id_format() actually ever produces --
     * a Zigbee id, "zb:" (3 chars) + 16 hex chars (its 8-byte address) = 19
     * chars -- round-trips without truncation corrupting the comparison.
     * (An espnow id is only "espnow:" (7) + 12 hex (its 6-byte address) =
     * 19 chars too; BLE is shorter still at 16. All comfortably under
     * BINDKEY_DEVID_MAX - 1 = 23, which the next case checks separately.) */
    {
        const char *zb_id = "zb:AABBCCDDEEFF1122";  /* 3 + 16 = 19 chars */
        assert(strlen(zb_id) == 19);
        const uint8_t key_b[16] = { 9 };
        bindkey_blob_t blob;
        bindkey_blob_build(zb_id, key_b, &blob);
        assert(strcmp(blob.dev_id, zb_id) == 0);
        uint8_t out[16];
        assert(bindkey_blob_verify(zb_id, &blob, out) == true);
    }

    /* --- BINDKEY_DEVID_MAX's own literal ceiling (23 chars + NUL) --- */
    {
        char longest[BINDKEY_DEVID_MAX];
        memset(longest, 'a', BINDKEY_DEVID_MAX - 1);
        longest[BINDKEY_DEVID_MAX - 1] = '\0';
        const uint8_t key_c[16] = { 7 };
        bindkey_blob_t blob;
        bindkey_blob_build(longest, key_c, &blob);
        assert(strcmp(blob.dev_id, longest) == 0);
        uint8_t out[16];
        assert(bindkey_blob_verify(longest, &blob, out) == true);
    }

    printf("test_bindkey_core: OK\n");
    return 0;
}
