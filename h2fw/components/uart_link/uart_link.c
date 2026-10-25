#include "uart_link.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "swarm_frame.h"
#include "uart_framer.h"
#include "zigbee.h"
#include "zb_store.h"
#include "data_core.h"
#include "capability.h"
#include "actor.h"
#include "action.h"

#define TAG "uart_link"

#define UL_PORT       UART_NUM_1
#define UL_TX_GPIO    10
#define UL_RX_GPIO    11
#define UL_BAUD       921600
#define UL_BUF        1024
#define UL_CMD_TTL_S  30u

static SemaphoreHandle_t s_tx_mtx;

/* Single in-flight actuate, correlated back to its UART seq when the zb_cmd
 * engine reports the outcome. v1 tolerates one outstanding actuate. */
static struct { bool used; uint16_t seq; int dev_idx; uint8_t action; } s_inflight;

/* ---- TX ---------------------------------------------------------------- */

static void ul_send(const uint8_t *payload, size_t n)
{
    uint8_t wire[UF_MAX_PAYLOAD + 6];
    size_t w = uf_encode(payload, n, wire, sizeof wire);
    if (!w) { ESP_LOGW(TAG, "uf_encode failed (payload %u)", (unsigned)n); return; }
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    uart_write_bytes(UL_PORT, (const char *)wire, w);
    xSemaphoreGive(s_tx_mtx);
}

static void build_announce(const zb_device_t *dev, swarm_device_announce_t *a)
{
    memset(a, 0, sizeof *a);
    a->dev.kind = DEV_KIND_ZIGBEE;
    memcpy(a->dev.addr, dev->eui64, 8);
    a->interviewed = dev->interviewed;
    a->name_len = (uint8_t)strnlen(dev->name, SWARM_DEV_NAME_MAX);
    memcpy(a->name, dev->name, a->name_len);
    a->cap_count = dev->cap_count > SWARM_DEV_MAX_CAPS ? SWARM_DEV_MAX_CAPS : dev->cap_count;
    for (uint8_t i = 0; i < a->cap_count; i++) {
        a->cap_ids[i] = dev->caps[i];
        a->cap_clusters[i] = dev->cap_clusters[i];
        a->cap_endpoints[i] = dev->cap_endpoints[i];
    }
    a->action_count = dev->action_count > SWARM_DEV_MAX_ACTIONS ? SWARM_DEV_MAX_ACTIONS : dev->action_count;
    for (uint8_t i = 0; i < a->action_count; i++) {
        a->action_ids[i] = dev->actions[i];
        a->action_endpoints[i] = dev->action_endpoints[i];
    }
    a->manuf_len = (uint8_t)strnlen(dev->manufacturer, SWARM_DEV_STR_MAX);
    memcpy(a->manufacturer, dev->manufacturer, a->manuf_len);
    a->model_len = (uint8_t)strnlen(dev->model, SWARM_DEV_STR_MAX);
    memcpy(a->model, dev->model, a->model_len);
}

static void send_announce(const zb_device_t *dev)
{
    swarm_device_announce_t a;
    build_announce(dev, &a);
    uint8_t pl[256];
    size_t n = swarm_encode_device_announce(&a, pl, sizeof pl);
    if (n) ul_send(pl, n);
}

static void send_coord_status(void)
{
    uint8_t channel = 0; uint16_t pan_id = 0; bool formed = false;
    bool started = zigbee_net_info(&channel, &pan_id, &formed);
    swarm_coord_status_t s = {0};
    s.radio_role = 0;   /* a local wired coordinator has no swarm radio role */
    s.formed = started && formed;
    s.channel = channel;
    s.pan_id = pan_id;
    s.permit_s = zigbee_permit_join_remaining();
    s.device_count = (uint8_t)zigbee_device_count();
    uint8_t pl[64];
    size_t n = swarm_encode_coord_status(&s, pl, sizeof pl);
    if (n) ul_send(pl, n);
}

/* Replays the full joined-device list (from zb_store, via zigbee_device_list)
 * so the C6 can reconcile after either chip reboots -- idempotent upserts on
 * the C6 side. */
static void replay_devices(void)
{
    /* zb_device_t is ~160 B; a 16-slot array is ~2.5 KB -- far too much for
     * the main task's stack (uart_link_start runs there at boot). Heap it. */
    zb_device_t *devs = malloc(sizeof(zb_device_t) * ZB_STORE_MAX_DEVICES);
    if (!devs) { ESP_LOGW(TAG, "replay: out of memory"); return; }
    int n = zigbee_device_list(devs, ZB_STORE_MAX_DEVICES);
    for (int i = 0; i < n; i++) send_announce(&devs[i]);
    free(devs);
    ESP_LOGI(TAG, "replayed %d device(s) to C6", n);
}

/* ---- Egress observers (fire on the Zigbee stack task; encode + send only) */

static void on_device(const zb_device_t *dev, bool gone)
{
    if (gone) {
        swarm_device_gone_t g = {0};
        g.dev.kind = DEV_KIND_ZIGBEE;
        memcpy(g.dev.addr, dev->eui64, 8);
        uint8_t pl[32];
        size_t n = swarm_encode_device_gone(&g, pl, sizeof pl);
        if (n) ul_send(pl, n);
    } else {
        send_announce(dev);
    }
}

static void on_status(void) { send_coord_status(); }

static void on_tuya(const uint8_t eui64[8], uint8_t dp_id, uint8_t dp_type, int32_t value)
{
    swarm_tuya_dp_t d = {0};
    d.dev.kind = DEV_KIND_ZIGBEE;
    memcpy(d.dev.addr, eui64, 8);
    d.dp_id = dp_id; d.dp_type = dp_type; d.value = value;
    uint8_t pl[64];
    size_t n = swarm_encode_tuya_dp(&d, pl, sizeof pl);
    if (n) ul_send(pl, n);
}

static void on_measurement(const uint8_t eui64[8], uint8_t cap_id, uint8_t endpoint,
                           float value, uint16_t source_cluster)
{
    swarm_measurement_t m = {0};
    m.dev.kind = DEV_KIND_ZIGBEE;
    memcpy(m.dev.addr, eui64, 8);
    m.cap_id = cap_id; m.endpoint = endpoint; m.value = value;
    m.age_s = 0; m.source_cluster = source_cluster;
    uint8_t pl[64];
    size_t n = swarm_encode_measurement(&m, pl, sizeof pl);
    if (n) ul_send(pl, n);
}

/* The zb_cmd engine's outcome callback: correlate to the in-flight actuate's
 * UART seq and send a COMMAND_ACK back to the C6. */
static void on_cmd_result(int dev_idx, uint8_t action_id, uint16_t param, bool ok, uint8_t zcl_status)
{
    (void)param;
    if (!s_inflight.used || s_inflight.dev_idx != dev_idx || s_inflight.action != action_id)
        return;
    swarm_command_ack_t ack = {0};
    ack.seq = s_inflight.seq;
    ack.op = SWARM_CMD_ACTUATE;
    ack.status = ok ? SWARM_ACK_DONE : SWARM_ACK_FAILED;
    ack.detail = zcl_status;
    s_inflight.used = false;
    uint8_t pl[32];
    size_t n = swarm_encode_command_ack(&ack, pl, sizeof pl);
    if (n) ul_send(pl, n);
}

/* ---- RX ---------------------------------------------------------------- */

static void handle_command(const swarm_command_t *c)
{
    switch (c->op) {
    case SWARM_CMD_PERMIT_JOIN:
        zigbee_permit_join();
        break;
    case SWARM_CMD_ACTUATE: {
        device_id_t id = { .kind = DEV_KIND_ZIGBEE };
        memcpy(id.addr, c->dev.addr, SWARM_ADDR_LEN);
        int idx = data_core_find_index(&id);
        if (idx < 0) {
            swarm_command_ack_t ack = { .seq = c->seq, .op = SWARM_CMD_ACTUATE,
                                        .status = SWARM_ACK_FAILED, .detail = 0xfe };
            uint8_t pl[32]; size_t n = swarm_encode_command_ack(&ack, pl, sizeof pl);
            if (n) ul_send(pl, n);
            ESP_LOGW(TAG, "ACTUATE for unknown device, nacked");
            break;
        }
        actor_cmd_t ac = {0};
        ac.dev_idx = (int8_t)idx;
        ac.action_id = (uint8_t)(c->arg & 0xff);   /* same packing as the C6 h2_link_dispatch */
        ac.param = (uint16_t)(c->arg >> 8);
        ac.endpoint = c->endpoint;
        ac.source = ACTOR_SRC_REMOTE;
        ac.deadline_s = actor_now_s() + (c->ttl_s ? c->ttl_s : UL_CMD_TTL_S);
        s_inflight.used = true; s_inflight.seq = c->seq; s_inflight.dev_idx = idx; s_inflight.action = ac.action_id;
        zb_cmd_local_dispatch(&ac);   /* sends the ZCL command + arms its own timeout */
        break;
    }
    case SWARM_CMD_DEVICE_REMOVE:
        zigbee_device_remove(c->dev.addr);
        break;
    case SWARM_CMD_DEVICE_RENAME:
        zigbee_device_rename(c->dev.addr, c->name_len ? c->name : "");
        break;
    case SWARM_CMD_RESYNC:
        /* The C6 asks us to re-assert state (its boot, or a reading for a
         * device it has no record of): replay device list + current status. */
        replay_devices();
        send_coord_status();
        break;
    default:
        ESP_LOGW(TAG, "unhandled command op %u", c->op);
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uf_rx_t rx; uf_rx_init(&rx);
    uint8_t buf[256];
    for (;;) {
        int got = uart_read_bytes(UL_PORT, buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < got; i++) {
            if (!uf_rx_byte(&rx, buf[i])) continue;
            int type = swarm_frame_type(rx.buf, rx.len);
            if (type == SWARM_MSG_COMMAND) {
                swarm_command_t c;
                if (swarm_decode_command(rx.buf, rx.len, &c)) handle_command(&c);
            } else {
                ESP_LOGD(TAG, "ignoring inbound frame type %d", type);
            }
        }
    }
}

/* Periodic COORD_STATUS so the C6's view (permit_s countdown, formed,
 * device_count) tracks the H2's real state between the observer-driven
 * updates -- the webUI polls /api/v1/zigbee at this cadence while a permit
 * window is open and ticks the countdown client-side. Doubles as a liveness
 * heartbeat the C6 can later use to detect a coordinator-offline condition. */
#define UL_STATUS_PERIOD_MS 2000
static void status_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UL_STATUS_PERIOD_MS));
        send_coord_status();
    }
}

static void ul_uart_init(void)
{
    uart_config_t c = {
        .baud_rate = UL_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UL_PORT, UL_BUF, UL_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UL_PORT, &c));
    ESP_ERROR_CHECK(uart_set_pin(UL_PORT, UL_TX_GPIO, UL_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_timeout(UL_PORT, 2);   /* ~2-symbol idle = mid-frame resync guard */
    s_tx_mtx = xSemaphoreCreateMutex();
}

esp_err_t uart_link_start(void)
{
    ul_uart_init();
    zigbee_set_device_observer(on_device);
    zigbee_set_status_observer(on_status);
    zigbee_set_tuya_dp_observer(on_tuya);
    zigbee_set_measurement_observer(on_measurement);
    zb_cmd_set_result_cb(on_cmd_result);
    xTaskCreate(rx_task, "ul_rx", 4096, NULL, 6, NULL);
    xTaskCreate(status_tx_task, "ul_status", 3072, NULL, 4, NULL);
    /* Boot assert: tell the C6 our coordinator state and replay known devices. */
    send_coord_status();
    replay_devices();
    return ESP_OK;
}
