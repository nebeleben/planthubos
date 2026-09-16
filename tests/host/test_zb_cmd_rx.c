#include <assert.h>
#include <stdio.h>
#include "zb_cmd_rx.h"

int main(void) {
    /* signature: standard On/Off ids are NOT a knob signature */
    assert(!zb_cmd_rx_is_knob_sig(0x00));
    assert(!zb_cmd_rx_is_knob_sig(0x01));
    assert(!zb_cmd_rx_is_knob_sig(0x02));
    assert(!zb_cmd_rx_is_knob_sig(0x40));
    /* non-standard ids ARE the signature */
    assert(zb_cmd_rx_is_knob_sig(0x03));
    assert(zb_cmd_rx_is_knob_sig(0x04));
    assert(zb_cmd_rx_is_knob_sig(0xfc));
    assert(zb_cmd_rx_is_knob_sig(0xfd));

    int16_t v = 123;
    assert(zb_cmd_rx_knob_decode(0x00, &v) == ZBCMD_PRESS && v == 0);
    assert(zb_cmd_rx_knob_decode(0x02, &v) == ZBCMD_PRESS && v == 2);
    assert(zb_cmd_rx_knob_decode(0x03, &v) == ZBCMD_ROTATE && v == 1);
    assert(zb_cmd_rx_knob_decode(0x04, &v) == ZBCMD_ROTATE && v == -1);
    assert(zb_cmd_rx_knob_decode(0xfc, &v) == ZBCMD_IGNORE);
    assert(zb_cmd_rx_knob_decode(0x01, &v) == ZBCMD_IGNORE);

    /* accumulator: a CW turn of 6 detents nets +6; mixed nets the sum */
    zb_rot_acc_t a = {0};
    for (int i = 0; i < 6; i++) zb_rot_acc_add(&a, +1);
    assert(zb_rot_acc_take(&a) == 6);
    assert(zb_rot_acc_take(&a) == 0);            /* take resets */
    zb_rot_acc_add(&a, +1); zb_rot_acc_add(&a, +1); zb_rot_acc_add(&a, -1);
    assert(zb_rot_acc_take(&a) == 1);
    printf("test_zb_cmd_rx: OK\n");
    return 0;
}
