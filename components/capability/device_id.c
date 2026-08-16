#include "capability.h"
#include <string.h>
#include <stdio.h>

device_id_t device_id_from_mac(device_kind_t kind, const uint8_t mac[6]) {
    device_id_t id;
    id.kind = (uint8_t)kind;
    memcpy(id.addr, mac, 6);
    id.addr[6] = 0;
    id.addr[7] = 0;
    return id;
}

bool device_id_equal(const device_id_t *a, const device_id_t *b) {
    if (!a || !b)
        return a == b;
    return a->kind == b->kind && memcmp(a->addr, b->addr, sizeof a->addr) == 0;
}

static const char *kind_prefix(uint8_t kind) {
    switch ((device_kind_t)kind) {
        case DEV_KIND_BLE:    return "ble";
        case DEV_KIND_ESPNOW: return "espnow";
        case DEV_KIND_ZIGBEE: return "zb";
    }
    return NULL;
}

static int kind_addr_len(uint8_t kind) {
    switch ((device_kind_t)kind) {
        case DEV_KIND_BLE:
        case DEV_KIND_ESPNOW: return 6;
        case DEV_KIND_ZIGBEE: return 8;
    }
    return -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *device_id_format(const device_id_t *id, char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return buf;
    buf[0] = '\0';
    if (!id)
        return buf;

    const char *prefix = kind_prefix(id->kind);
    int addrlen = kind_addr_len(id->kind);
    if (!prefix || addrlen < 0)
        return buf;

    /* worst case: "espnow:" + 16 hex chars + NUL = 24 */
    char tmp[32];
    size_t off = (size_t)snprintf(tmp, sizeof tmp, "%s:", prefix);
    for (int i = 0; i < addrlen && off < sizeof tmp; i++)
        off += (size_t)snprintf(tmp + off, sizeof(tmp) - off, "%02X", id->addr[i]);

    size_t len = strlen(tmp);
    if (len >= buflen)
        len = buflen - 1;
    memcpy(buf, tmp, len);
    buf[len] = '\0';
    return buf;
}

/* Legacy V1 colon-separated MAC form: AA:BB:CC:DD:EE:FF (17 chars, 6 groups). */
static bool is_colon_mac(const char *s) {
    if (strlen(s) != 17)
        return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':')
                return false;
        } else if (hexval(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool parse_hex_bytes(const char *s, size_t nbytes, uint8_t *out) {
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool device_id_parse(const char *s, device_id_t *out) {
    if (!s || !out || s[0] == '\0')
        return false;

    if (is_colon_mac(s)) {
        uint8_t mac[6];
        for (int i = 0; i < 6; i++) {
            int hi = hexval(s[i * 3]);
            int lo = hexval(s[i * 3 + 1]);
            if (hi < 0 || lo < 0)
                return false;
            mac[i] = (uint8_t)((hi << 4) | lo);
        }
        *out = device_id_from_mac(DEV_KIND_BLE, mac);
        return true;
    }

    const char *colon = strchr(s, ':');
    if (!colon)
        return false;
    size_t prefix_len = (size_t)(colon - s);
    const char *hexpart = colon + 1;

    device_kind_t kind;
    int addrlen;
    if (prefix_len == 3 && strncmp(s, "ble", 3) == 0) {
        kind = DEV_KIND_BLE; addrlen = 6;
    } else if (prefix_len == 6 && strncmp(s, "espnow", 6) == 0) {
        kind = DEV_KIND_ESPNOW; addrlen = 6;
    } else if (prefix_len == 2 && strncmp(s, "zb", 2) == 0) {
        kind = DEV_KIND_ZIGBEE; addrlen = 8;
    } else {
        return false;
    }

    size_t hexlen = strlen(hexpart);
    if (hexlen != (size_t)addrlen * 2)
        return false;

    uint8_t raw[8] = {0};
    if (!parse_hex_bytes(hexpart, (size_t)addrlen, raw))
        return false;

    device_id_t id;
    id.kind = (uint8_t)kind;
    memset(id.addr, 0, sizeof id.addr);
    memcpy(id.addr, raw, (size_t)addrlen);
    *out = id;
    return true;
}
