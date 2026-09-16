#include "zb_cmd_rx.h"

bool zb_cmd_rx_is_knob_sig(uint8_t cmd) {
    switch (cmd) {
        case 0x00: case 0x01: case 0x02:      /* Off / On / Toggle */
        case 0x40: case 0x41: case 0x42:      /* *WithEffect / *RecallGlobalScene / *WithTimedOff */
            return false;
        default:
            return true;                       /* 0x03/0x04/0xfc/0xfd/... : the knob's overload */
    }
}

zbcmd_kind_t zb_cmd_rx_knob_decode(uint8_t cmd, int16_t *out) {
    switch (cmd) {
        case 0x00: *out = 0;  return ZBCMD_PRESS;
        case 0x02: *out = 2;  return ZBCMD_PRESS;
        case 0x03: *out = 1;  return ZBCMD_ROTATE;
        case 0x04: *out = -1; return ZBCMD_ROTATE;
        default:              return ZBCMD_IGNORE;  /* 0xfc/0xfd companions, 0x01, etc. */
    }
}

void zb_rot_acc_add(zb_rot_acc_t *a, int16_t delta) { a->net += delta; }
int16_t zb_rot_acc_take(zb_rot_acc_t *a) { int16_t n = a->net; a->net = 0; return n; }
