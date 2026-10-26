#include "h2_link.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "swarm_frame.h"
#include "swarm_ingest.h"
#include "uart_framer.h"
#include "data_core.h"
#include "capability.h"
#include "actor.h"

#define TAG "h2_link"

#define HL_PORT    UART_NUM_1
#define HL_TX_GPIO 10
#define HL_RX_GPIO 11
#define HL_BAUD    921600
/* The H2's boot / RESYNC replay sends a back-to-back burst of announce frames
 * (each up to UF_MAX_PAYLOAD + framing) at 921600 baud, and the RX task runs
 * at a low priority (below httpd) -- 1 KB rides out a burst while it is
 * preempted. (Was trimmed to 512 in the 2026-10 heap-starved period.) */
#define HL_RX_BUF  1024
/* The RX task runs the whole ingest path (data_core, actors, dev_profiles,
 * ESP_LOG float formatting) on this stack. 3072 overflowed in the 2026-10
 * soak (stack protection fault in h2_rx every ~600 s), so the frame buffers
 * live in statics below rather than on it. */
#define HL_RX_STACK 4096

static SemaphoreHandle_t s_tx_mtx;

static SemaphoreHandle_t s_status_mtx;
static swarm_coord_status_t s_last_status;
static bool s_status_valid;
static uint32_t s_status_rx_s;   /* now_s() when s_last_status arrived */

static uint16_t s_seq;
static struct { bool used; uint16_t seq; int dev_idx; uint8_t action; } s_inflight;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* ---- TX ---------------------------------------------------------------- */

static void hl_send(const uint8_t *payload, size_t n)
{
    uint8_t wire[UF_MAX_PAYLOAD + 6];
    size_t w = uf_encode(payload, n, wire, sizeof wire);
    if (!w) { ESP_LOGW(TAG, "uf_encode failed (%u)", (unsigned)n); return; }
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    uart_write_bytes(HL_PORT, (const char *)wire, w);
    xSemaphoreGive(s_tx_mtx);
}

static void send_command(const swarm_command_t *c)
{
    uint8_t pl[sizeof(swarm_command_t) + 8];
    size_t n = swarm_encode_command(c, pl, sizeof pl);
    if (n) hl_send(pl, n);
}

static void send_resync(void)
{
    swarm_command_t c = { .seq = ++s_seq, .op = SWARM_CMD_RESYNC };
    send_command(&c);
}

esp_err_t h2_link_permit(void)
{
    swarm_command_t c = { .seq = ++s_seq, .op = SWARM_CMD_PERMIT_JOIN };
    send_command(&c);
    return ESP_OK;
}

esp_err_t h2_link_remove(const uint8_t eui64[8])
{
    swarm_command_t c = { .seq = ++s_seq, .op = SWARM_CMD_DEVICE_REMOVE, .dev.kind = DEV_KIND_ZIGBEE };
    memcpy(c.dev.addr, eui64, SWARM_ADDR_LEN);
    send_command(&c);
    return ESP_OK;
}

esp_err_t h2_link_rename(const uint8_t eui64[8], const char *name)
{
    swarm_command_t c = { .seq = ++s_seq, .op = SWARM_CMD_DEVICE_RENAME, .dev.kind = DEV_KIND_ZIGBEE };
    memcpy(c.dev.addr, eui64, SWARM_ADDR_LEN);
    size_t len = name ? strlen(name) : 0;
    c.name_len = (uint8_t)(len > SWARM_DEV_NAME_MAX ? SWARM_DEV_NAME_MAX : len);
    memcpy(c.name, name, c.name_len);
    send_command(&c);
    return ESP_OK;
}

bool h2_link_coord_status(swarm_coord_status_t *out, uint32_t *age_s)
{
    bool valid;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    valid = s_status_valid;
    if (valid && out) *out = s_last_status;
    if (valid && age_s) *age_s = now_s() - s_status_rx_s;
    xSemaphoreGive(s_status_mtx);
    return valid;
}

bool h2_link_alive(void)
{
    uint32_t age;
    return h2_link_coord_status(NULL, &age) && age <= H2_LINK_STALE_S;
}

/* Zigbee actuate dispatch hook: an actor command for a DEV_KIND_ZIGBEE device
 * is routed over the UART to the H2 (the local radio), instead of to a local
 * zb_cmd engine (disabled on the two-chip hub) or an ESP-NOW bridge. */
static void h2_link_dispatch(const actor_cmd_t *cmd)
{
    uint8_t key[ACTOR_DEVICE_KEY_LEN];
    if (!actor_device_key(cmd->dev_idx, key)) {
        ESP_LOGW(TAG, "actuate: no device key for dev_idx %d", cmd->dev_idx);
        return;
    }
    swarm_command_t c = { .seq = ++s_seq, .op = SWARM_CMD_ACTUATE, .endpoint = cmd->endpoint };
    c.dev.kind = key[0];
    memcpy(c.dev.addr, key + 1, SWARM_ADDR_LEN);
    c.arg = (uint16_t)((uint16_t)cmd->action_id | ((uint16_t)cmd->param << 8));
    c.ttl_s = (cmd->deadline_s > now_s()) ? (uint16_t)(cmd->deadline_s - now_s()) : 0;
    s_inflight.used = true; s_inflight.seq = c.seq;
    s_inflight.dev_idx = cmd->dev_idx; s_inflight.action = cmd->action_id;
    send_command(&c);
}

/* ---- RX ---------------------------------------------------------------- */

static void dispatch_frame(const uint8_t *buf, size_t len)
{
    int type = swarm_frame_type(buf, len);
    switch (type) {
    case SWARM_MSG_DEVICE_ANNOUNCE: {
        swarm_device_announce_t a;
        if (swarm_decode_device_announce(buf, len, &a))
            swarm_ingest_announce(&a, NULL, now_s());   /* NULL via_mac = local radio */
        break;
    }
    case SWARM_MSG_DEVICE_GONE: {
        swarm_device_gone_t g;
        if (swarm_decode_device_gone(buf, len, &g)) swarm_ingest_device_gone(&g);
        break;
    }
    case SWARM_MSG_MEASUREMENT: {
        swarm_measurement_t m;
        if (swarm_decode_measurement(buf, len, &m)) {
            if (!swarm_ingest_measurement(&m, 0, now_s())) {
                ESP_LOGW(TAG, "measurement for unknown device; asking H2 to resync");
                send_resync();
            }
        }
        break;
    }
    case SWARM_MSG_TUYA_DP: {
        swarm_tuya_dp_t d;
        if (swarm_decode_tuya_dp(buf, len, &d)) swarm_ingest_tuya_dp(&d, now_s());
        break;
    }
    case SWARM_MSG_COORD_STATUS: {
        swarm_coord_status_t s;
        if (swarm_decode_coord_status(buf, len, &s)) {
            xSemaphoreTake(s_status_mtx, portMAX_DELAY);
            s_last_status = s; s_status_valid = true; s_status_rx_s = now_s();
            xSemaphoreGive(s_status_mtx);
        }
        break;
    }
    case SWARM_MSG_COMMAND_ACK: {
        swarm_command_ack_t ack;
        if (swarm_decode_command_ack(buf, len, &ack) && s_inflight.used && s_inflight.seq == ack.seq) {
            ESP_LOGI(TAG, "actuate seq %u -> %s (detail %u)", ack.seq,
                     ack.status == SWARM_ACK_DONE ? "done" : "failed", ack.detail);
            s_inflight.used = false;
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "ignoring inbound frame type %d", type);
        break;
    }
}

/* Static, not on rx_task's stack: the framer alone holds a UF_MAX_PAYLOAD
 * (512 B) frame buffer, and together with the read buffer that was ~790 B of
 * the stack the ingest path needs (see HL_RX_STACK). Only rx_task uses them. */
static uf_rx_t s_rx;
static uint8_t s_rx_chunk[256];

static void rx_task(void *arg)
{
    (void)arg;
    uf_rx_init(&s_rx);
    uint32_t frames = 0;
    bool hwm_logged = false;
    for (;;) {
        int got = uart_read_bytes(HL_PORT, s_rx_chunk, sizeof s_rx_chunk, pdMS_TO_TICKS(100));
        for (int i = 0; i < got; i++) {
            if (uf_rx_byte(&s_rx, s_rx_chunk[i])) {
                dispatch_frame(s_rx.buf, s_rx.len);
                frames++;
            }
        }
        /* One stack high-water line once the ingest path has really been
         * exercised (boot replay + a few minutes of reports), like the
         * sampler's and the rules engine's. */
        if (!hwm_logged && frames >= 200) {
            ESP_LOGI(TAG, "rx task stack high-water mark: %u B unused of %u B (after %lu frames)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)HL_RX_STACK,
                     (unsigned long)frames);
            hwm_logged = true;
        }
    }
}

static void hl_uart_init(void)
{
    uart_config_t c = {
        .baud_rate = HL_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    /* tx_buffer_size 0 = blocking writes straight to the FIFO (no TX ring):
     * frames are tiny and infrequent, and this reclaims ~1 KB of the
     * heap-starved hub's budget (see the browser-unreachable bench finding). */
    ESP_ERROR_CHECK(uart_driver_install(HL_PORT, HL_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HL_PORT, &c));
    ESP_ERROR_CHECK(uart_set_pin(HL_PORT, HL_TX_GPIO, HL_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Idle the RX line HIGH (UART idle level) with an internal pull-up. When
     * the H2 is unpowered or not driving (switch open, or mid-boot), its TX
     * output floats; without this, a floating 921600-baud RX line picks up
     * noise that the UART reads as an endless stream of bytes, and the RX task
     * then spins forever -- which starved the webserver/WiFi and made the UI
     * unreachable (bench, 2026-10-20). A pull-up keeps RX idle (no frames) when
     * nothing drives it. */
    gpio_set_pull_mode(HL_RX_GPIO, GPIO_PULLUP_ONLY);
    uart_set_rx_timeout(HL_PORT, 2);
}

esp_err_t h2_link_start(void)
{
    s_tx_mtx = xSemaphoreCreateMutex();
    s_status_mtx = xSemaphoreCreateMutex();
    hl_uart_init();
    /* Claim the Zigbee actuate route. On the two-chip hub the local zb_cmd
     * engine is a disabled-build stub and there is no ESP-NOW bridge, so
     * overwriting whatever swarm_start_main() registered is correct -- hence
     * the "start after swarm_start_main()" contract (see h2_link.h). */
    actor_set_dispatch_hook(DEV_KIND_ZIGBEE, h2_link_dispatch);
    /* Priority 4: BELOW the httpd server task (tskIDLE+5) so a busy UART --
     * even a noise storm on a floating RX -- can never starve the webserver
     * or WiFi. A few frames/s at this priority drains the 512 B RX ring fine. */
    xTaskCreate(rx_task, "h2_rx", HL_RX_STACK, NULL, 4, NULL);
    send_resync();   /* ask the H2 to replay its device list on our boot */
    ESP_LOGI(TAG, "h2_link up on UART%d (tx=%d rx=%d @ %d)", HL_PORT, HL_TX_GPIO, HL_RX_GPIO, HL_BAUD);
    return ESP_OK;
}
