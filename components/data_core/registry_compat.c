/* M2-SHIM: see registry_compat.h's doc comment.
 *
 * data_core_snapshot_legacy() itself lives in data_core.c, not here: it
 * must decode directly out of the live s_registry under s_mutex (see that
 * file) rather than first taking a full 2048-byte registry_t copy via
 * data_core_snapshot() -- several of this shim's callers (webserver/sse.c,
 * swarm.c's on_sensor_update()) run on the default event-loop task, which
 * both files' own comments document as having only ~2304 bytes of stack; a
 * second registry_t-sized local on top of that would risk overflowing it.
 * legacy_registry_find() below has no such concern (it only ever scans a
 * buffer the CALLER already owns), so it stays here. */
#include "registry_compat.h"
#include <string.h>

int legacy_registry_find(const legacy_registry_t *r, const uint8_t mac[6])
{
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++)
        if (r->sensors[i].in_use && memcmp(r->sensors[i].mac, mac, 6) == 0) return i;
    return -1;
}
