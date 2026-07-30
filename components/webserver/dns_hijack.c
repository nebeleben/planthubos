#include "dns_hijack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "dns_hijack";
#define DNS_PORT 53
#define AP_IP 0xC0A80401  /* 192.168.4.1 */

static TaskHandle_t s_task;
static int s_sock = -1;

/* Walk the QNAME labels starting at offset 12, then skip QTYPE(2)+QCLASS(2).
 * Returns the offset just past QCLASS (end of the question section) and
 * fills *qtype, or -1 if the packet is truncated/malformed. */
static int parse_question(const uint8_t *buf, int len, uint16_t *qtype)
{
    int off = 12;
    while (off < len) {
        uint8_t label_len = buf[off];
        if (label_len == 0) { off += 1; break; }
        if ((label_len & 0xC0) != 0) return -1; /* compression not expected in a query */
        off += 1 + label_len;
    }
    if (off + 4 > len) return -1;
    *qtype = (buf[off] << 8) | buf[off + 1];
    return off + 4;
}

/* Minimal DNS: answer A/ANY queries with one A record -> AP_IP; everything
 * else gets a header-only, no-answer response. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    s_sock = sock;
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    ESP_LOGI(TAG, "DNS hijack listening on :53");

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&src, &slen);
        if (len < 12) continue;

        uint16_t qtype = 0;
        int qend = parse_question(buf, len, &qtype);

        buf[2] = 0x81; buf[3] = 0x80; /* response, no error */

        if (qend < 0 || (qtype != 1 /* A */ && qtype != 255 /* ANY */)) {
            /* Header-only response: drop question and answer sections. */
            buf[4] = buf[5] = 0;             /* QDCOUNT = 0 */
            buf[6] = buf[7] = 0;             /* ANCOUNT = 0 */
            buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NS/AR = 0 */
            sendto(sock, buf, 12, 0, (struct sockaddr *)&src, slen);
            continue;
        }

        buf[4] = 0x00; buf[5] = 0x01;      /* QDCOUNT = 1 */
        buf[6] = 0x00; buf[7] = 0x01;      /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NS/AR = 0 */

        uint8_t answer[] = {
            0xC0, 0x0C,             /* name: pointer to query name */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* RDLENGTH 4 */
            (AP_IP >> 24) & 0xFF, (AP_IP >> 16) & 0xFF, (AP_IP >> 8) & 0xFF, AP_IP & 0xFF,
        };
        memcpy(buf + qend, answer, sizeof(answer));
        sendto(sock, buf, qend + sizeof(answer), 0, (struct sockaddr *)&src, slen);
    }
}

void dns_hijack_start(void)
{
    if (s_task) return; /* already running: guard against double-start */
    xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, &s_task);
}

void dns_hijack_stop(void)
{
    if (!s_task) return;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    vTaskDelete(s_task);
    s_task = NULL;
    ESP_LOGI(TAG, "DNS hijack stopped");
}
