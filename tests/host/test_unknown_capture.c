#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "unknown_capture.h"

static const uint8_t MAC_A[6] = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t MAC_B[6] = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x02};

static void mac_n(uint8_t out[6], uint8_t n)
{
    memset(out, 0, 6);
    out[5] = n;
}

int main(void)
{
    printf("unknown_capture: sizeof(unknown_sample_t)=%zu sizeof(unknown_dev_t)=%zu "
           "table=%zu B (UNKNOWN_DEVICES=%d, UNKNOWN_SAMPLES=%d)\n",
           sizeof(unknown_sample_t), sizeof(unknown_dev_t),
           sizeof(unknown_dev_t) * UNKNOWN_DEVICES, UNKNOWN_DEVICES, UNKNOWN_SAMPLES);

    unknown_capture_init();

    unknown_dev_t out[UNKNOWN_DEVICES];
    assert(unknown_capture_list(out, UNKNOWN_DEVICES) == 0);

    /* 1. first advert creates a device with one sample */
    uint8_t p1[4] = {0x01, 0x02, 0x03, 0x04};
    unknown_capture_add(MAC_A, p1, sizeof(p1), -40, 100);
    size_t n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == 1);
    assert(out[0].in_use);
    assert(memcmp(out[0].mac, MAC_A, 6) == 0);
    assert(out[0].last_seen_s == 100);
    assert(out[0].n == 1);
    assert(out[0].s[0].len == sizeof(p1));
    assert(memcmp(out[0].s[0].payload, p1, sizeof(p1)) == 0);
    assert(out[0].s[0].rssi == -40);
    assert(out[0].s[0].ts == 100);

    /* 2. second advert from the same device rotates into the second slot */
    uint8_t p2[3] = {0x11, 0x12, 0x13};
    unknown_capture_add(MAC_A, p2, sizeof(p2), -41, 101);
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == 1);   /* still one device */
    assert(out[0].n == 2);
    assert(out[0].last_seen_s == 101);
    /* first sample unchanged, in slot 0 (oldest) */
    assert(out[0].s[0].len == sizeof(p1));
    assert(memcmp(out[0].s[0].payload, p1, sizeof(p1)) == 0);
    /* new sample landed in slot 1 (newest) */
    assert(out[0].s[1].len == sizeof(p2));
    assert(memcmp(out[0].s[1].payload, p2, sizeof(p2)) == 0);
    assert(out[0].s[1].rssi == -41);
    assert(out[0].s[1].ts == 101);

    /* 3. a third advert replaces the oldest sample, not the device */
    uint8_t p3[5] = {0x21, 0x22, 0x23, 0x24, 0x25};
    unknown_capture_add(MAC_A, p3, sizeof(p3), -42, 102);
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == 1);                    /* still exactly one device */
    assert(out[0].n == UNKNOWN_SAMPLES); /* sample count stays capped */
    /* slot 0 now holds what was the newest sample (p2), the true oldest (p1) is gone */
    assert(out[0].s[0].len == sizeof(p2));
    assert(memcmp(out[0].s[0].payload, p2, sizeof(p2)) == 0);
    /* slot 1 holds the newest sample (p3) */
    assert(out[0].s[1].len == sizeof(p3));
    assert(memcmp(out[0].s[1].payload, p3, sizeof(p3)) == 0);
    assert(out[0].s[1].ts == 102);

    /* a second, distinct device is tracked independently */
    uint8_t pb[2] = {0x55, 0x66};
    unknown_capture_add(MAC_B, pb, sizeof(pb), -50, 200);
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == 2);

    /* 5. forget removes a device and frees its slot */
    unknown_capture_forget(MAC_B);
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == 1);
    assert(memcmp(out[0].mac, MAC_A, 6) == 0);
    /* forgetting a MAC that was never tracked (or already forgotten) is a no-op */
    unknown_capture_forget(MAC_B);
    assert(unknown_capture_list(out, UNKNOWN_DEVICES) == 1);

    /* 4. a ninth distinct device evicts the least-recently-seen */
    unknown_capture_init();
    uint8_t mac[6];
    for (int i = 0; i < UNKNOWN_DEVICES; i++) {
        mac_n(mac, (uint8_t)i);
        uint8_t payload[1] = {(uint8_t)i};
        /* last_seen_s ascending with device index -- device 0 is the
         * least-recently-seen once all 8 slots are full. */
        unknown_capture_add(mac, payload, sizeof(payload), -30, (uint32_t)(1000 + i));
    }
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == UNKNOWN_DEVICES);

    mac_n(mac, 8);   /* the 9th distinct device */
    uint8_t payload9[1] = {0x09};
    unknown_capture_add(mac, payload9, sizeof(payload9), -30, 2000);
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == UNKNOWN_DEVICES);   /* still capped at UNKNOWN_DEVICES */

    /* device 0 (least-recently-seen) is gone; every other original device
     * plus the new one are present */
    bool saw_device0 = false, saw_device9 = false;
    int found_count = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t want0[6]; mac_n(want0, 0);
        uint8_t want9[6]; mac_n(want9, 8);
        if (memcmp(out[i].mac, want0, 6) == 0) saw_device0 = true;
        if (memcmp(out[i].mac, want9, 6) == 0) { saw_device9 = true; found_count++; }
        for (int j = 1; j < UNKNOWN_DEVICES; j++) {
            uint8_t wantj[6]; mac_n(wantj, (uint8_t)j);
            if (memcmp(out[i].mac, wantj, 6) == 0) found_count++;
        }
    }
    assert(!saw_device0);
    assert(saw_device9);
    assert(found_count == UNKNOWN_DEVICES);   /* devices 1..8 all present */

    /* 6. list() returns only in-use entries, honours `max`, and a fresh
     * init() clears everything */
    unknown_capture_init();
    assert(unknown_capture_list(out, UNKNOWN_DEVICES) == 0);
    for (int i = 0; i < UNKNOWN_DEVICES; i++) {
        mac_n(mac, (uint8_t)i);
        uint8_t payload[1] = {(uint8_t)i};
        unknown_capture_add(mac, payload, sizeof(payload), -30, (uint32_t)(3000 + i));
    }
    assert(unknown_capture_list(out, UNKNOWN_DEVICES) == UNKNOWN_DEVICES);
    unknown_capture_forget(mac);   /* mac still holds device 7's MAC from the loop above */
    n = unknown_capture_list(out, UNKNOWN_DEVICES);
    assert(n == UNKNOWN_DEVICES - 1);
    for (size_t i = 0; i < n; i++) assert(out[i].in_use);
    /* `max` is honoured even when more devices are tracked */
    assert(unknown_capture_list(out, 2) == 2);

    /* len is clamped to ADV_PAYLOAD_MAX defensively */
    unknown_capture_init();
    uint8_t big[ADV_PAYLOAD_MAX + 10];
    memset(big, 0x7A, sizeof(big));
    unknown_capture_add(MAC_A, big, sizeof(big), -20, 1);
    assert(unknown_capture_list(out, 1) == 1);
    assert(out[0].s[0].len == ADV_PAYLOAD_MAX);

    printf("test_unknown_capture: OK\n");
    return 0;
}
