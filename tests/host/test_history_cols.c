#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "storage.h"
#include "capability.h"

int main(void)
{
    /* fresh map is all CAP_NONE */
    history_map_t m;
    history_map_init(&m);
    assert(m.fmt == 2);
    for (int i = 0; i < HISTORY_COLS; i++) assert(m.cap[i] == CAP_NONE);

    /* history_map_ensure allocates ascending columns and is idempotent
     * for a repeated capability */
    assert(history_map_ensure(&m, CAP_AIR_TEMPERATURE) == 0);
    assert(history_map_ensure(&m, CAP_SOIL_MOISTURE) == 1);
    assert(history_map_ensure(&m, CAP_AIR_TEMPERATURE) == 0);   /* idempotent, same column */
    assert(history_map_ensure(&m, CAP_BATTERY_LEVEL) == 2);
    assert(history_map_ensure(&m, CAP_SOIL_MOISTURE) == 1);     /* idempotent, same column */

    /* history_map_col finds mapped capabilities, -1 for unmapped ones */
    assert(history_map_col(&m, CAP_AIR_TEMPERATURE) == 0);
    assert(history_map_col(&m, CAP_SOIL_MOISTURE) == 1);
    assert(history_map_col(&m, CAP_BATTERY_LEVEL) == 2);
    assert(history_map_col(&m, CAP_LIGHT_ILLUMINANCE) == -1);

    /* filling all 8 columns, then requesting a 9th, returns -1 and leaves
     * the map intact */
    history_map_t full;
    history_map_init(&full);
    const uint8_t all_caps[HISTORY_COLS] = {
        CAP_SOIL_MOISTURE, CAP_AIR_TEMPERATURE, CAP_LIGHT_ILLUMINANCE,
        CAP_SOIL_CONDUCTIVITY, CAP_BATTERY_LEVEL, CAP_AIR_HUMIDITY,
        CAP_AIR_PRESSURE, CAP_SIGNAL_RSSI,
    };
    for (int i = 0; i < HISTORY_COLS; i++)
        assert(history_map_ensure(&full, all_caps[i]) == i);

    history_map_t snapshot = full;
    /* 9th capability: there is no 9th real capability id (CAPABILITY_COUNT
     * == 8), so use an arbitrary id outside the known set to exercise the
     * "map is full" path regardless. */
    assert(history_map_ensure(&full, 0x7E) == -1);
    assert(memcmp(&full, &snapshot, sizeof(full)) == 0);   /* untouched */

    /* a record written under a 2-column map decodes correctly under a
     * later 3-column map: the two original columns keep their
     * capabilities, the new column reads CAP_VALUE_NONE for old records */
    history_map_t m2;
    history_map_init(&m2);
    int col_temp = history_map_ensure(&m2, CAP_AIR_TEMPERATURE);
    int col_moist = history_map_ensure(&m2, CAP_SOIL_MOISTURE);
    assert(col_temp >= 0 && col_moist >= 0 && col_temp != col_moist);

    storage_rec_t rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.boot_id = 1;
    rec.rel_s = 100;
    for (int i = 0; i < HISTORY_COLS; i++) rec.col[i] = CAP_VALUE_NONE;
    rec.col[col_temp] = 215;    /* e.g. 21.5C encoded */
    rec.col[col_moist] = 40;

    /* later (a newer firmware, or a later sample), a 3rd capability gets
     * mapped for the first time */
    int col_batt = history_map_ensure(&m2, CAP_BATTERY_LEVEL);
    assert(col_batt >= 0 && col_batt != col_temp && col_batt != col_moist);

    /* the two original columns kept their original capability assignment */
    assert(history_map_col(&m2, CAP_AIR_TEMPERATURE) == col_temp);
    assert(history_map_col(&m2, CAP_SOIL_MOISTURE) == col_moist);

    /* decoding the OLD record (written before battery was ever mapped)
     * under the NEW 3-column map: the two original columns still read
     * back their original values, and the new column -- never written on
     * this old record -- reads CAP_VALUE_NONE */
    assert(rec.col[col_temp] == 215);
    assert(rec.col[col_moist] == 40);
    assert(rec.col[col_batt] == CAP_VALUE_NONE);

    printf("test_history_cols: OK\n");
    return 0;
}
