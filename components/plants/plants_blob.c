#include "plants_blob.h"
#include <string.h>

void plants_blob_pack(const plants_table_t *in, plants_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->format = PLANTS_BLOB_FORMAT;
    out->next_id = in->next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        out->p[i].id = in->p[i].id;
        out->p[i].in_use = in->p[i].in_use ? 1 : 0;
        memcpy(out->p[i].mac, in->p[i].mac, 6);
        out->p[i].mac_valid = in->p[i].mac_valid ? 1 : 0;
        memcpy(out->p[i].name, in->p[i].name, sizeof(out->p[i].name));
        for (int c = 0; c < CAPABILITY_COUNT; c++) {
            out->p[i].cap_bound[c] = in->p[i].cap_bound[c] ? 1 : 0;
            out->p[i].cap_dev[c] = in->p[i].cap_dev[c];
        }
        for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
            out->p[i].act_bound[s] = in->p[i].act_bound[s] ? 1 : 0;
            out->p[i].act_id[s] = in->p[i].act_id[s];
            out->p[i].act_dev[s] = in->p[i].act_dev[s];
        }
    }
}

void plants_blob_unpack(const plants_blob_t *blob, plants_table_t *out)
{
    out->next_id = blob->next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        out->p[i].in_use = blob->p[i].in_use != 0;
        out->p[i].id = blob->p[i].id;
        out->p[i].mac_valid = blob->p[i].mac_valid != 0;
        memcpy(out->p[i].mac, blob->p[i].mac, 6);
        memcpy(out->p[i].name, blob->p[i].name, sizeof(out->p[i].name));
        out->p[i].name[PLANT_NAME_LEN] = '\0';  /* defensive: guard against a corrupt non-terminated blob */
        for (int c = 0; c < CAPABILITY_COUNT; c++) {
            out->p[i].cap_bound[c] = blob->p[i].cap_bound[c] != 0;
            out->p[i].cap_dev[c] = blob->p[i].cap_dev[c];
        }
        for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
            out->p[i].act_bound[s] = blob->p[i].act_bound[s] != 0;
            out->p[i].act_id[s] = blob->p[i].act_id[s];
            out->p[i].act_dev[s] = blob->p[i].act_dev[s];
        }
    }
}

void plants_blob_migrate_v2(const plants_blob_v2_t *in, plants_table_t *out)
{
    out->next_id = in->next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        out->p[i].in_use = in->p[i].in_use != 0;
        out->p[i].id = in->p[i].id;
        out->p[i].mac_valid = in->p[i].mac_valid != 0;
        memcpy(out->p[i].mac, in->p[i].mac, 6);
        memcpy(out->p[i].name, in->p[i].name, sizeof(out->p[i].name));
        out->p[i].name[PLANT_NAME_LEN] = '\0';  /* defensive, same as plants_blob_unpack() */

        /* The eight capability bindings a format-2 blob actually carries
         * survive verbatim. */
        for (int c = 0; c < 8; c++) {
            out->p[i].cap_bound[c] = in->p[i].cap_bound[c] != 0;
            out->p[i].cap_dev[c] = in->p[i].cap_dev[c];
        }
        /* switch.state (cap id 8) did not exist in format 2: starts
         * unbound, exactly like every other never-bound capability. */
        out->p[i].cap_bound[CAP_SWITCH_STATE] = false;
        memset(&out->p[i].cap_dev[CAP_SWITCH_STATE], 0, sizeof(device_id_t));

        /* Action slots did not exist in format 2 either: both start
         * unbound with ACTION_NONE, same as a freshly created plant. */
        for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
            out->p[i].act_bound[s] = false;
            out->p[i].act_id[s] = ACTION_NONE;
            memset(&out->p[i].act_dev[s], 0, sizeof(device_id_t));
        }
    }
}

bool plants_blob_load(const uint8_t *bytes, size_t len, plants_table_t *out)
{
    if (!bytes || !out) {
        return false;
    }
    if (len == sizeof(plants_blob_t)) {
        const plants_blob_t *blob = (const plants_blob_t *)(const void *)bytes;
        if (blob->format != PLANTS_BLOB_FORMAT) {
            return false;
        }
        plants_blob_unpack(blob, out);
        return true;
    }
    if (len == sizeof(plants_blob_v2_t)) {
        const plants_blob_v2_t *v2 = (const plants_blob_v2_t *)(const void *)bytes;
        if (v2->format != PLANTS_BLOB_FORMAT_V2) {
            return false;
        }
        plants_blob_migrate_v2(v2, out);
        return true;
    }
    return false;
}
