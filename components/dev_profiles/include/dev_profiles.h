#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reusable Zigbee mapping profile keyed by (manufacturer, model). Pure and
 * host-testable: cap targets carry a resolved cap_id (the cJSON layer in
 * dev_profiles_json.c resolves NAME->id via capability_by_name first). The
 * built-in seed is embedded read-only (dev_profiles_builtin.c); user/AI
 * profiles live in a bounded RAM store persisted to LittleFS. */

#define DEV_PROFILE_STR_MAX     32
#define DEV_PROFILE_LABEL_MAX   48
#define DEV_PROFILE_MAX_ENTRIES 16
#define DEV_PROFILE_MAX_USER    32

enum {
    DEV_PROFILE_KIND_DP       = 1,  /* Tuya EF00 datapoint -> cap */
    DEV_PROFILE_KIND_SUPPRESS = 2,  /* drop any measurement from source_cluster */
    DEV_PROFILE_KIND_CLUSTER  = 3,  /* RESERVED for v-next; rejected by validate in v1 */
};

typedef struct {
    uint8_t  kind;           /* DEV_PROFILE_KIND_* */
    uint8_t  dp_id;          /* DP kind only */
    uint8_t  cap_id;         /* DP kind only; resolved from a cap name */
    float    scale;          /* DP kind only */
    float    offset;         /* DP kind only */
    uint16_t source_cluster; /* SUPPRESS kind only */
} dev_profile_entry_t;

typedef struct {
    char    manufacturer[DEV_PROFILE_STR_MAX];
    char    model[DEV_PROFILE_STR_MAX];
    char    label[DEV_PROFILE_LABEL_MAX];   /* optional UI label, may be "" */
    uint8_t entry_count;
    dev_profile_entry_t entries[DEV_PROFILE_MAX_ENTRIES];
} dev_profile_t;

typedef struct {
    uint8_t       count;
    dev_profile_t p[DEV_PROFILE_MAX_USER];
} dev_profile_store_t;

void dev_profile_store_init(dev_profile_store_t *s);
/* Replaces the entry with the same (manufacturer, model) in place, else
 * appends. Returns its index, or -1 when full. */
int  dev_profile_store_upsert(dev_profile_store_t *s, const dev_profile_t *p);
bool dev_profile_store_remove(dev_profile_store_t *s, const char *manuf, const char *model);

/* Resolves a cap NAME to its id. false + *cap_id_out untouched on unknown. */
bool dev_profile_resolve_cap(const char *name, uint8_t *cap_id_out);

/* DP entries need cap_id < CAPABILITY_COUNT + finite scale/offset; suppress
 * entries need source_cluster != 0; match key non-empty; entry_count in range;
 * the reserved CLUSTER kind is rejected in v1. */
bool dev_profile_validate(const dev_profile_t *p);

/* user store first (precedence), then the embedded built-in seed; NULL if
 * neither matches. `user` may be NULL to search the built-ins only. */
const dev_profile_t *dev_profile_match(const char *manuf, const char *model,
                                       const dev_profile_store_t *user);

/* Embedded read-only seed (dev_profiles_builtin.c). */
int                  dev_profile_builtin_count(void);
const dev_profile_t *dev_profile_builtin(int i);

/* --- dev_profiles_json.c: cJSON parse/emit + LittleFS persistence ---
 * (device-mapping-profiles Task 3; declared here rather than in the pure
 * core above ONLY because this header is the one public interface for the
 * whole component -- the cJSON dependency itself stays confined to
 * dev_profiles_json.c's .c file, per this component's own "Pure and
 * host-testable" doc comment up top; dev_profiles.c/dev_profiles_builtin.c/
 * mapping_engine.c never call any of these four and stay cJSON-free.) */

/* Parses the spec section 3 schema ({"match":{...}, "label"?, "entries":[...]})
 * into *out. false (contents of *out are then undefined) on malformed JSON,
 * a missing/non-string match.manufacturer or .model, a missing/non-array
 * entries, an unknown entry "kind", an unresolvable cap NAME
 * (dev_profile_resolve_cap()), or a result that fails dev_profile_validate() --
 * every rejection reason a caller (api_v1.c) should turn into an honest 400
 * rather than storing a half-parsed profile. */
bool dev_profile_from_json(const char *json, dev_profile_t *out);
/* Inverse of dev_profile_from_json() above. Caller-frees (free()) the
 * returned buffer; NULL on allocation failure. */
char *dev_profile_to_json(const dev_profile_t *p);

/* Loads the user/AI profile store from LittleFS (DEV_PROFILE_PATH,
 * dev_profiles_json.c) into *s, always via dev_profile_store_init() first --
 * an absent file, a corrupt/non-array JSON body, or an individual entry
 * that fails dev_profile_from_json() all degrade to "leave that one entry
 * out" (a corrupt STORE reads as empty, a corrupt single ENTRY within an
 * otherwise-good store is just skipped), never a crash or a partially
 * memcpy'd struct. */
void dev_profile_user_load(dev_profile_store_t *s);
/* Serializes *s (every entry via dev_profile_to_json()) and writes it
 * atomically (tmp+rename, same discipline as tuya_dp_map_save()). false on
 * any allocation/write/rename failure -- the previous on-disk file is left
 * untouched either way. */
bool dev_profile_user_save(const dev_profile_store_t *s);
