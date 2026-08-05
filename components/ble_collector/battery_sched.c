#include "battery_sched.h"
#include <string.h>

void batt_sched_seen(batt_entry_t *tab, const uint8_t mac[6],
                      uint8_t addr_type, const uint8_t addr_val[6], uint32_t now_s)
{
    int free_idx = -1;
    for (int i = 0; i < BATT_MAX_SENSORS; i++) {
        if (tab[i].in_use && memcmp(tab[i].mac, mac, 6) == 0) {
            tab[i].addr_type = addr_type;
            memcpy(tab[i].addr_val, addr_val, 6);
            tab[i].last_seen_s = now_s;
            return;
        }
        if (!tab[i].in_use && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) return;   /* table full: drop silently, mirrors registry_update_from() */

    tab[free_idx].in_use = true;
    memcpy(tab[free_idx].mac, mac, 6);
    tab[free_idx].addr_type = addr_type;
    memcpy(tab[free_idx].addr_val, addr_val, 6);
    tab[free_idx].last_seen_s = now_s;
    tab[free_idx].last_ok_s = 0;
    tab[free_idx].last_attempt_s = 0;
}

int batt_sched_pick(const batt_entry_t *tab, uint32_t now_s)
{
    int best_idx = -1;
    uint32_t best_last_ok = 0;

    for (int i = 0; i < BATT_MAX_SENSORS; i++) {
        const batt_entry_t *e = &tab[i];
        if (!e->in_use) continue;

        bool due_poll  = (e->last_ok_s == 0) || (now_s - e->last_ok_s >= BATT_POLL_INTERVAL_S);
        bool due_retry = (e->last_attempt_s == 0) || (now_s - e->last_attempt_s >= BATT_RETRY_INTERVAL_S);
        bool recently_seen = (now_s - e->last_seen_s) <= 300;
        if (!due_poll || !due_retry || !recently_seen) continue;

        if (best_idx < 0 || e->last_ok_s < best_last_ok) {
            best_idx = i;
            best_last_ok = e->last_ok_s;
        }
    }
    return best_idx;
}
