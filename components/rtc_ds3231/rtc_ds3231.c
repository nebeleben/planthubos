/* DS3231 device layer: I2C transport (new i2c_master driver) + the
 * train-on-authoritative-time event wiring. All real work is gated on
 * CONFIG_PLANTHUB_RTC_DS3231; the #else stubs keep callers unconditional. */
#include "rtc_ds3231.h"

#if CONFIG_PLANTHUB_RTC_DS3231

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "timekeeper.h"

#define TAG            "rtc_ds3231"
#define RTC_SDA_GPIO   6            /* matches docs/esplayout/h2-link-wiring.md */
#define RTC_SCL_GPIO   7
#define RTC_I2C_PORT   I2C_NUM_0
#define RTC_ADDR       0x68
#define RTC_HZ         100000
#define RTC_TIMEOUT_MS 100
#define RTC_REG_TIME   0x00        /* Seconds..Year = 0x00..0x06 */
#define RTC_REG_STATUS 0x0F        /* bit7 = OSF (oscillator stop flag) */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;   /* NULL => no DS3231 present */

static bool read_regs(uint8_t start, uint8_t *out, size_t n)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &start, 1, out, n, RTC_TIMEOUT_MS) == ESP_OK;
}

bool rtc_ds3231_read_epoch(uint32_t *epoch_out)
{
    if (!s_dev || !epoch_out) return false;
    uint8_t t[7], st;
    if (!read_regs(RTC_REG_TIME, t, sizeof t)) return false;
    if (!read_regs(RTC_REG_STATUS, &st, 1)) return false;
    bool osf = (st & 0x80) != 0;
    return rtc_ds3231_decode(t, osf, epoch_out);
}

bool rtc_ds3231_write_epoch(uint32_t epoch)
{
    if (!s_dev) return false;
    uint8_t buf[8];
    buf[0] = RTC_REG_TIME;
    rtc_ds3231_encode(epoch, buf + 1);
    if (i2c_master_transmit(s_dev, buf, sizeof buf, RTC_TIMEOUT_MS) != ESP_OK) return false;

    /* Clear OSF so a trained chip reads as valid on the next boot. */
    uint8_t st;
    if (read_regs(RTC_REG_STATUS, &st, 1) && (st & 0x80)) {
        uint8_t w[2] = { RTC_REG_STATUS, (uint8_t)(st & ~0x80) };
        i2c_master_transmit(s_dev, w, sizeof w, RTC_TIMEOUT_MS);
    }
    return true;
}

/* Fires on the default event loop when SNTP or a browser time-set learns an
 * authoritative epoch -- train the chip so it stays accurate and seeds the
 * right time on the next boot. The boot-seed path sets timekeeper directly
 * and never posts this event, so there is no feedback loop. */
static void on_epoch_learned(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (rtc_ds3231_write_epoch(*(uint32_t *)data))
        ESP_LOGI(TAG, "trained DS3231 to epoch %lu", (unsigned long)*(uint32_t *)data);
}

esp_err_t rtc_ds3231_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = RTC_I2C_PORT,
        .sda_io_num = RTC_SDA_GPIO,
        .scl_io_num = RTC_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* the module has its own too */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    err = i2c_master_probe(s_bus, RTC_ADDR, RTC_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no DS3231 on the bus (%s); falling back to SNTP", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = RTC_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) { i2c_del_master_bus(s_bus); s_bus = NULL; return err; }

    esp_event_handler_register(PLANTHUB_TIME_EVENT, TIME_EVENT_EPOCH_LEARNED, on_epoch_learned, NULL);
    ESP_LOGI(TAG, "DS3231 present on I2C%d (sda=%d scl=%d)", RTC_I2C_PORT, RTC_SDA_GPIO, RTC_SCL_GPIO);
    return ESP_OK;
}

#else  /* !CONFIG_PLANTHUB_RTC_DS3231 */

esp_err_t rtc_ds3231_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool rtc_ds3231_read_epoch(uint32_t *epoch_out) { (void)epoch_out; return false; }
bool rtc_ds3231_write_epoch(uint32_t epoch) { (void)epoch; return false; }

#endif
