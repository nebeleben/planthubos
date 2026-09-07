#pragma once
/* swarm_power_mode.h -- the pure (no-IDF) half of the node power-mode enum,
 * split out of swarm_store.h so swarm_rules.c (and its host test) can
 * compile with plain cc: swarm_store.h pulls in esp_err.h, which isn't
 * available outside the ESP-IDF toolchain. swarm_store.h includes this
 * file for the same definitions rather than duplicating them. */

/* Power mode a battery node runs under (M7). ALWAYS_ON never sleeps and is
 * the default for both a brand-new own-device mode and an unknown node's
 * desired mode -- a node/hub that has never heard otherwise behaves exactly
 * like a pre-M7 device. BATTERY_15/60 are the two checkin-interval presets;
 * see batt_cycle.h's batt_period_s() for the actual seconds each maps to. */
typedef enum {
    SWARM_PM_ALWAYS_ON  = 0,
    SWARM_PM_BATTERY_15 = 1,
    SWARM_PM_BATTERY_60 = 2,
} swarm_power_mode_t;
#define SWARM_PM_VALID(m) ((m) <= SWARM_PM_BATTERY_60)
