#include "dns_hijack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "dns_hijack";
#define DNS_PORT 53
#define AP_IP 0xC0A80401  /* 192.168.4.1 */

/* Minimal DNS: echo the query back as a response with one A record -> AP_IP. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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

        buf[2] = 0x81; buf[3] = 0x80;      /* response, no error */
        buf[6] = buf[4]; buf[7] = buf[5];  /* ANCOUNT = QDCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NS/AR = 0 */

        uint8_t answer[] = {
            0xC0, 0x0C,             /* name: pointer to query name */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* RDLENGTH 4 */
            (AP_IP >> 24) & 0xFF, (AP_IP >> 16) & 0xFF, (AP_IP >> 8) & 0xFF, AP_IP & 0xFF,
        };
        memcpy(buf + len, answer, sizeof(answer));
        sendto(sock, buf, len + sizeof(answer), 0, (struct sockaddr *)&src, slen);
    }
}

void dns_hijack_start(void)
{
    xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, NULL);
}
