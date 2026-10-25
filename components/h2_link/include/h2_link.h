/* h2_link.h -- the C6 side of the wired C6<->H2 Zigbee transport.
 *
 * Owns UART1, decodes the H2 coordinator's announce/measurement/tuya/status
 * frames and feeds them through the SHARED swarm_ingest_* bodies (so the
 * local-radio path and the ESP-NOW bridge path cannot diverge), and sends
 * command/permit/resync frames back. The H2 is a LOCAL radio, not a swarm
 * node: its devices land in the data_core registry (and so in /api/v1/devices)
 * but it never appears in /api/v1/nodes. Enabled by CONFIG_PLANTHUB_H2_LINK_ENABLED,
 * which the two-chip-appliance build sets together with CONFIG_PLANTHUB_ZB_ENABLED=n.
 *
 * h2_link_start() MUST run after swarm_start_main() so it wins the
 * DEV_KIND_ZIGBEE actor dispatch hook (routing actuations over the UART). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "swarm_frame.h"   /* swarm_coord_status_t */

/* Installs UART1, starts the RX task, claims the Zigbee actuate dispatch hook,
 * and sends a boot RESYNC asking the H2 to replay its device list. */
esp_err_t h2_link_start(void);

/* Opens the coordinator's permit-join window (sends a PERMIT_JOIN command to
 * the H2). Returns ESP_OK once queued. */
esp_err_t h2_link_permit(void);

/* The H2 sends COORD_STATUS every 2 s (uart_link's status_tx_task); after
 * this many seconds without one, the coordinator counts as not responding
 * (H2 unpowered, crashed, rebooting, or the UART link broken). Five missed
 * reports, so a single dropped frame never flaps it. */
#define H2_LINK_STALE_S 10

/* Copies the H2's last reported coordinator status into *out and its age in
 * seconds into *age_s (either may be NULL); returns false if none has
 * arrived yet this boot. For GET /api/v1/zigbee -- a caller compares *age_s
 * against H2_LINK_STALE_S rather than trusting a status that may be old. */
bool h2_link_coord_status(swarm_coord_status_t *out, uint32_t *age_s);

/* True when a COORD_STATUS has arrived within the last H2_LINK_STALE_S
 * seconds, i.e. the H2 is up and the link is carrying frames. */
bool h2_link_alive(void);

/* Removes / renames an H2-owned device (sends the matching command). */
esp_err_t h2_link_remove(const uint8_t eui64[8]);
esp_err_t h2_link_rename(const uint8_t eui64[8], const char *name);
