#ifndef ZB_CMD_RX_H
#define ZB_CMD_RX_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { ZBCMD_IGNORE = 0, ZBCMD_PRESS, ZBCMD_ROTATE } zbcmd_kind_t;

typedef struct { int16_t net; } zb_rot_acc_t;

bool zb_cmd_rx_is_knob_sig(uint8_t onoff_cmd_id);

zbcmd_kind_t zb_cmd_rx_knob_decode(uint8_t onoff_cmd_id, int16_t *out);

void zb_rot_acc_add(zb_rot_acc_t *a, int16_t delta);

int16_t zb_rot_acc_take(zb_rot_acc_t *a);

#endif
