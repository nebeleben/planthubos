/* uart_link.h -- the H2 side of the wired C6<->H2 Zigbee transport.
 *
 * Registers the zigbee component's egress observers (device / status / tuya /
 * measurement) and forwards each as a framed swarm_* frame over UART1; and
 * runs an RX task that decodes the C6's command/permit/identify/resync frames
 * and drives them into the coordinator (zigbee_permit_join / zigbee_read_identity
 * / zb_cmd_local_dispatch / zb_store announce replay). The payload inside the
 * framer is a verbatim swarm_* frame, so both chips reuse swarm_encode_* /
 * swarm_decode_* with no divergence. Reliability is lossless-wire: CRC + resync
 * in the framer, a boot status+replay to resync device lists, and a command-ack
 * timeout owned by the zb_cmd engine. */
#pragma once
#include "esp_err.h"

/* Installs UART1, registers the four egress observers + the command-result
 * callback, starts the RX task, and sends an initial coord-status + device
 * replay. Call once, after zigbee_start(). */
esp_err_t uart_link_start(void);
