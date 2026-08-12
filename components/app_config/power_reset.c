/* Rapid power-cycle WiFi reset.
 *
 * The BOOT-hold factory reset needs physical access to the button -- a hub
 * mounted in a case or tucked behind a shelf has no reachable button, but
 * it always has a power plug. Pulling power N times in quick succession
 * clears the WiFi credentials (and nothing else: claim key, pairings,
 * plants and history all survive), dropping the device back into its
 * onboarding portal.
 *
 * Mechanism: a boot counter in NVS. Every TRUE power-on boot increments
 * it; reaching POWER_RESET_CYCLES triggers the reset. A device that stays
 * up longer than POWER_RESET_WINDOW_MS clears the counter (that boot was
 * normal operation, not part of a rapid sequence). Only
 * ESP_RST_POWERON participates: software restarts (OTA, mode changes),
 * panics, brownouts and -- essential for M7 battery nodes, which reset
 * ~96x/day by design -- deep-sleep wakes all BREAK the sequence instead
 * of feeding it.
 */
#include "app_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "power_reset";

/* Set when this boot IS the triggering one. A hub is done at that point
 * (clearing WiFi is the whole reset), but a NODE has no WiFi credentials
 * at all -- its identity is role + pairing in the swarm store, which this
 * component must not depend on. main.c reads this flag after
 * swarm_store_init() and finishes the node half there. */
static bool s_triggered;

bool power_cycle_reset_triggered(void) { return s_triggered; }

#define POWER_RESET_CYCLES     5
#define POWER_RESET_WINDOW_MS  10000
#define NS  "planthub"
#define KEY "pwr_rst"

static uint8_t counter_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, KEY, &v);
    nvs_close(h);
    return v;
}

static void counter_store(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (v == 0) nvs_erase_key(h, KEY);
    else        nvs_set_u8(h, KEY, v);
    nvs_commit(h);
    nvs_close(h);
}

/* esp_timer callback: runs on the esp_timer task (not an ISR) -- a short
 * NVS write is fine here, same as the sampler's timer-driven work. */
static void window_elapsed(void *arg)
{
    (void)arg;
    if (counter_load() != 0) {
        counter_store(0);
        ESP_LOGI(TAG, "up for %ds; power-cycle counter cleared", POWER_RESET_WINDOW_MS / 1000);
    }
}

void power_cycle_reset_start(void)
{
    if (esp_reset_reason() != ESP_RST_POWERON) {
        /* Any non-power reset breaks the sequence. */
        if (counter_load() != 0) counter_store(0);
        return;
    }

    uint8_t n = counter_load() + 1;
    if (n >= POWER_RESET_CYCLES) {
        counter_store(0);
        s_triggered = true;
        esp_err_t err = app_config_clear_wifi();
        ESP_LOGW(TAG, "%d rapid power cycles: WiFi credentials cleared (%s) -- "
                      "claim and plant data untouched; onboarding portal next",
                 n, esp_err_to_name(err));
        return;
    }
    counter_store(n);
    ESP_LOGI(TAG, "power-on %u/%d within the rapid-cycle window", n, POWER_RESET_CYCLES);

    const esp_timer_create_args_t targs = { .callback = window_elapsed, .name = "pwr_rst" };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)POWER_RESET_WINDOW_MS * 1000);
    }
}
