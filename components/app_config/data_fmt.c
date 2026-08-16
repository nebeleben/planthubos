#include "data_fmt.h"

/* data_fmt_decide() is pure (no I/O, no ESP-IDF types) so it stays host
 * testable -- see data_fmt.h's file comment for why this file only pulls in
 * ESP-IDF headers inside the ESP_PLATFORM block below. ESP_PLATFORM is
 * defined globally by every idf.py/CMake component build; tests/host/run.sh
 * compiles this file with a plain `cc`, which never defines it, so only
 * data_fmt_decide() (and nothing NVS/filesystem-shaped) gets built there. */

data_fmt_action_t data_fmt_decide(bool present, uint8_t stored)
{
    if (!present || stored < DATA_FMT_CURRENT) return DATA_FMT_WIPE;
    if (stored > DATA_FMT_CURRENT) return DATA_FMT_FUTURE;
    return DATA_FMT_OK;
}

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"        /* storage_drop() */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define NS          "planthub"
#define KEY_FMT     "data_fmt"
#define KEY_NOTICE  "fmt_notice"

/* Upper bound for the P<id>_raw.bin/P<id>_hr.bin sweep below -- must match
 * plants_table.h's PLANTS_MAX. Duplicated here (rather than #include
 * "plants_table.h") to avoid a component dependency cycle: the "plants"
 * component's CMakeLists already REQUIRES app_config (plants.c calls
 * app_config_*() for sensor names), and ESP-IDF's component graph rejects a
 * requires cycle -- app_config depending back on "plants" just for this one
 * macro would create exactly that. tests/host/test_data_fmt.c carries a
 * _Static_assert against the real PLANTS_MAX so any future drift between
 * the two fails the host test suite immediately instead of silently under-
 * wiping higher-numbered plant ids on a real device. */
#define WIPE_PLANT_ID_MAX 16

static const char *TAG = "data_fmt";
static bool s_notice_pending = false;
static bool s_data_safe = true;

/* Best-effort remove(): ENOENT is the expected/idempotent case (already
 * gone, either never existed or a prior crashed wipe already got it), so it
 * is not logged. Any other errno is logged but never fatal -- a stray file
 * left behind on a weird filesystem error is a cosmetic issue (it will
 * simply be ignored by V2 code, which only ever opens P<id>_*.bin by the
 * plant ids it actually knows about), not a data-safety one. Returns true
 * iff a file was actually unlinked, purely for the summary log's count. */
static bool remove_if_present(const char *path)
{
    if (remove(path) == 0) return true;
    if (errno != ENOENT) {
        ESP_LOGW(TAG, "data_fmt: failed to remove %s: %s", path, strerror(errno));
    }
    return false;
}

bool data_fmt_apply(const char *storage_base)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data_fmt: nvs_open failed (%s); cannot check data format this boot", esp_err_to_name(err));
        /* A durably-FUTURE marker from an earlier boot plus a failed
         * nvs_open() THIS boot must not silently read back as "safe": with
         * s_data_safe defaulting true, plants_init()/sampler_start() would
         * run against newer-format on-disk data main.c never actually
         * verified this boot. Fail closed instead -- same as the real
         * DATA_FMT_FUTURE branch below. */
        s_data_safe = false;
        return false;
    }

    uint8_t stored = 0;
    esp_err_t get_err = nvs_get_u8(h, KEY_FMT, &stored);
    bool present = (get_err == ESP_OK);
    if (get_err != ESP_OK && get_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "data_fmt: nvs_get_u8(%s) failed (%s); treating marker as absent", KEY_FMT, esp_err_to_name(get_err));
        present = false;
    }

    /* The post-wipe UI notice is persistent across reboots until the user
     * dismisses it (Task 6's data_fmt_dismiss_notice() erases the key) --
     * refresh the in-RAM cache from NVS on every boot regardless of what
     * this boot's action turns out to be, so a notice latched on an earlier
     * boot still shows even when this boot itself has nothing to wipe. */
    uint8_t notice = 0;
    s_notice_pending = (nvs_get_u8(h, KEY_NOTICE, &notice) == ESP_OK) && notice != 0;

    data_fmt_action_t action = data_fmt_decide(present, stored);

    if (action == DATA_FMT_FUTURE) {
        /* Downgrade: a newer firmware already wrote a data_fmt this old
         * firmware doesn't recognise. Never wipe (that would destroy data a
         * correct/newer firmware could still read) and never write the
         * marker (that would be this firmware falsely claiming it made the
         * data current) -- just refuse to touch on-disk data at all and let
         * main.c skip plants_init()/sampler_start() via data_fmt_safe(). */
        ESP_LOGE(TAG, "data_fmt: stored marker %u is newer than this firmware's %d; refusing to "
                      "touch plant/history data (downgrade?) -- re-flash the matching firmware",
                 stored, DATA_FMT_CURRENT);
        s_data_safe = false;
        nvs_close(h);
        return false;
    }
    s_data_safe = true;

    if (action == DATA_FMT_OK) {
        nvs_close(h);
        return false;
    }

    /* DATA_FMT_WIPE. storage_base is NULL when littlefs failed to mount --
     * nothing to wipe on disk this boot, and writing the marker anyway
     * would permanently skip a wipe that never actually happened once
     * storage does come back. Leave the marker untouched so this retries on
     * a later boot, same log-and-continue treatment main.c gives every
     * other littlefs-dependent init. */
    if (!storage_base) {
        ESP_LOGW(TAG, "data_fmt: wipe needed but storage is unavailable; deferring to a later boot");
        nvs_close(h);
        return false;
    }

    int removed = 0;
    char path[128];
    snprintf(path, sizeof(path), "%s/plants.bin", storage_base);
    if (remove_if_present(path)) removed++;

    for (uint8_t id = 1; id <= WIPE_PLANT_ID_MAX; id++) {
        snprintf(path, sizeof(path), "%s/P%u_raw.bin", storage_base, id);
        if (remove_if_present(path)) removed++;
        snprintf(path, sizeof(path), "%s/P%u_hr.bin", storage_base, id);
        if (remove_if_present(path)) removed++;
        /* Clears storage.c's in-RAM write-cursor cache slot(s) for this id,
         * if any -- normally a no-op this early in boot (nothing has
         * appended yet), but harmless and cheap insurance against a future
         * call-order change leaving a stale cache entry pointed at a file
         * that no longer exists. Does not touch the filesystem itself,
         * hence the explicit remove_if_present() calls above -- see
         * storage.h's storage_drop() doc comment. */
        storage_drop(id);
    }

    /* Crash-safety / idempotency ordering: the marker is the LAST thing
     * written, and only once, after every delete above has already been
     * attempted and the notice flag is already durable. If power is lost
     * anywhere before data_fmt=CURRENT is committed, the stored marker is
     * still < CURRENT (or absent), so data_fmt_decide() returns WIPE again
     * on the next boot -- every step above is idempotent (remove() on an
     * already-gone file is just ENOENT, storage_drop() on an empty cache
     * slot is a no-op) so the retry safely finishes the job. Writing the
     * notice flag BEFORE the marker (rather than after, or in the same
     * nvs_commit) matters too: if a crash lands between the two, the next
     * boot still sees data_fmt < CURRENT and redoes this whole block,
     * re-setting notice=1 (already 1, harmless) before it re-attempts the
     * marker -- so the marker can only ever become durably CURRENT with the
     * notice already durably set first. The reverse order would risk a
     * durable marker=CURRENT with no notice ever set, silently swallowing
     * the one thing the user is supposed to be told about the wipe. */
    err = nvs_set_u8(h, KEY_NOTICE, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data_fmt: failed to persist notice flag (%s); wipe will be redone next boot", esp_err_to_name(err));
        nvs_close(h);
        return true;   /* files are already gone even though the marker isn't durable yet */
    }
    s_notice_pending = true;

    err = nvs_set_u8(h, KEY_FMT, DATA_FMT_CURRENT);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data_fmt: failed to persist data_fmt=%d marker (%s); wipe will be redone next boot",
                 DATA_FMT_CURRENT, esp_err_to_name(err));
        return true;
    }

    ESP_LOGW(TAG, "data_fmt: first V2 boot -- wiped %d V1 file(s) (plants.bin + history rings); "
                  "WiFi credentials, claim key, hub name, role/pairing and rules were preserved",
             removed);
    return true;
}

bool data_fmt_notice_pending(void) { return s_notice_pending; }

void data_fmt_dismiss_notice(void)
{
    s_notice_pending = false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_erase_key(h, KEY_NOTICE);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) nvs_commit(h);
    nvs_close(h);
}

bool data_fmt_safe(void) { return s_data_safe; }

#endif /* ESP_PLATFORM */
