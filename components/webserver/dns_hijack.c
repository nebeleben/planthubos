#include "dns_hijack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "dns_hijack";
#define DNS_PORT 53
#define AP_IP 0xC0A80401  /* 192.168.4.1 */

static TaskHandle_t s_task;
static int s_sock = -1;

/* dns_hijack_start() can race itself: the boot catch-up call in
 * webserver_start() (main task) and the WIFI_EVENT_AP_START handler
 * (default event-loop task) can both observe s_task == NULL and each spawn
 * a dns_task, leaking a task + bound socket. Make the guard atomic. */
static atomic_flag s_starting = ATOMIC_FLAG_INIT;

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
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed (errno=%d); aborting hijack task", DNS_PORT, errno);
        close(sock);
        s_sock = -1;
        s_task = NULL;
        atomic_flag_clear(&s_starting);
        vTaskDelete(NULL);
        return;
    }
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
    /* atomic_flag_test_and_set() is the atomic check-and-set: only the
     * first of two racing callers (boot catch-up vs. WIFI_EVENT_AP_START
     * handler) gets past this and creates the task. */
    if (atomic_flag_test_and_set(&s_starting)) return; /* already running/starting */
    xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, &s_task);
}

void dns_hijack_stop(void)
{
    /* Only ever called from the event-loop task after WIFI_EVENT_AP_STOP,
     * which cannot be in flight concurrently with the boot catch-up path,
     * so this doesn't need its own atomic guard. */
    if (!s_task) return;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    vTaskDelete(s_task);
    s_task = NULL;
    atomic_flag_clear(&s_starting);
    ESP_LOGI(TAG, "DNS hijack stopped");
}
