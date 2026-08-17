#!/bin/sh
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
$CC -Wall -Wextra -Werror -I../../components/app_config/include \
    test_creds_validate.c ../../components/app_config/creds_validate.c -o test_creds_validate
./test_creds_validate

$CC -Wall -Wextra -Werror -I../../components/wifi_manager/include \
    test_wifi_fsm.c ../../components/wifi_manager/wifi_fsm.c -o test_wifi_fsm
./test_wifi_fsm

$CC -Wall -Wextra -Werror -I../../components/mibeacon/include \
    test_mibeacon.c ../../components/mibeacon/mibeacon.c -o test_mibeacon
./test_mibeacon

$CC -Wall -Wextra -Werror -I../../components/capability/include -I../../components/data_core/include \
    test_registry.c ../../components/data_core/registry.c ../../components/capability/capability.c \
    ../../components/capability/device_id.c -o test_registry -lm
./test_registry

$CC -Wall -Wextra -Werror -I../../components/timekeeper/include \
    test_boottab.c ../../components/timekeeper/boottab.c -o test_boottab
./test_boottab

$CC -Wall -Wextra -Werror -I../../components/storage/include -I../../components/capability/include \
    test_storage.c ../../components/storage/storage.c -o test_storage
./test_storage

$CC -Wall -Wextra -Werror -I../../components/storage/include -I../../components/capability/include \
    test_hourly_agg.c ../../components/storage/hourly_agg.c -o test_hourly_agg
./test_hourly_agg

$CC -Wall -Wextra -Werror -I../../components/storage/include -I../../components/capability/include \
    test_history_cols.c ../../components/storage/storage.c -o test_history_cols
./test_history_cols

$CC -Wall -Wextra -Werror -I../../components/claiming/include \
    test_authtok.c ../../components/claiming/authtok.c -o test_authtok
./test_authtok

$CC -Wall -Wextra -Werror -I../../components/swarm/include \
    test_swarm_frame.c ../../components/swarm/swarm_frame.c -o test_swarm_frame
./test_swarm_frame

$CC -Wall -Wextra -Werror -I../../components/swarm/include \
    test_swarm_buf.c ../../components/swarm/swarm_buf.c -o test_swarm_buf
./test_swarm_buf

$CC -Wall -Wextra -Werror -I../../components/integrations/include -I../../components/capability/include \
    test_lineproto.c ../../components/integrations/lineproto.c ../../components/capability/capability.c \
    -o test_lineproto -lm
./test_lineproto

$CC -Wall -Wextra -Werror -I../../components/integrations/include -I../../components/capability/include \
    test_mqtt_json.c ../../components/integrations/mqtt_json.c ../../components/capability/capability.c \
    -o test_mqtt_json -lm
./test_mqtt_json

$CC -Wall -Wextra -Werror -I../../components/ble_collector/include \
    test_battery_sched.c ../../components/ble_collector/battery_sched.c -o test_battery_sched
./test_battery_sched

$CC -Wall -Wextra -Werror -I../../components/swarm/include \
    test_batt_cycle.c ../../components/swarm/batt_cycle.c -o test_batt_cycle
./test_batt_cycle

$CC -Wall -Wextra -Werror -I../../components/plants/include -I../../components/capability/include \
    test_plants_table.c ../../components/plants/plants_table.c \
    ../../components/capability/device_id.c -o test_plants_table
./test_plants_table

$CC -Wall -Wextra -Werror -I../../components/plants/include -I../../components/capability/include \
    test_plants_migrate.c ../../components/plants/plants_migrate.c ../../components/plants/plants_table.c -o test_plants_migrate
./test_plants_migrate

$CC -Wall -Wextra -Werror -I../../components/psvm/include \
    test_psvm.c ../../components/psvm/psvm.c -o test_psvm -lm
./test_psvm

$CC -Wall -Wextra -Werror -I../../components/gatt/include \
    test_gatt_fsm.c ../../components/gatt/gatt_fsm.c -o test_gatt_fsm
./test_gatt_fsm

$CC -Wall -Wextra -Werror -I../../components/rules/include \
    test_rules_fsm.c ../../components/rules/rules_fsm.c -o test_rules_fsm
./test_rules_fsm

$CC -Wall -Wextra -Werror -I../../components/event_log/include \
    test_event_ring.c ../../components/event_log/event_ring.c -o test_event_ring
./test_event_ring

$CC -Wall -Wextra -Werror -I../../components/capability/include \
    test_capability.c ../../components/capability/capability.c -o test_capability -lm
./test_capability

$CC -Wall -Wextra -Werror -I../../components/capability/include \
    test_device_id.c ../../components/capability/device_id.c -o test_device_id
./test_device_id

$CC -Wall -Wextra -Werror -I../../components/app_config/include -I../../components/plants/include \
    -I../../components/capability/include \
    test_data_fmt.c ../../components/app_config/data_fmt.c -o test_data_fmt
./test_data_fmt

$CC -Wall -Wextra -Werror -I../../components/ble_collector/include \
    test_adv_queue.c ../../components/ble_collector/adv_queue.c -o test_adv_queue
./test_adv_queue

$CC -Wall -Wextra -Werror -I../../components/wrappers/include \
    test_wrapper_index.c ../../components/wrappers/wrapper_index.c -o test_wrapper_index
./test_wrapper_index

$CC -Wall -Wextra -Werror -I../../components/wrappers/include \
    test_wrapper_arena.c ../../components/wrappers/wrapper_arena.c -o test_wrapper_arena
./test_wrapper_arena

$CC -Wall -Wextra -Werror -I../../components/wrappers/include \
    test_unknown_capture.c ../../components/wrappers/unknown_capture.c -o test_unknown_capture
./test_unknown_capture

# test_bthome links mbedtls's AES-CCM for the one encrypted vector the M3
# Task 3 brief asks for; the plain-cc host harness has no ESP-IDF mbedtls
# component to draw on, so this looks for a host mbedtls (Homebrew's, if
# present) and links it directly by full path (not -lmbedcrypto) to avoid
# depending on the dylib search path at run time. When no host mbedtls is
# found -- or it exists but won't actually link/run (e.g. this repo's own
# dev machine has only an Intel-bottled Homebrew under Rosetta at
# /usr/local, x86_64-only, while plain `cc` here targets arm64 natively; a
# probe-compile below tries native first, then -arch x86_64 via Rosetta,
# before giving up) -- the encrypted-vector case is compiled out
# (BTHOME_NO_MBEDTLS_CCM) and test_bthome.c prints an explicit skip notice
# instead of silently passing without ever running it. The device (ESP-IDF)
# build always links the real mbedtls component and always exercises that
# path (bthome.c has no such fallback there).
BTHOME_CCM_FLAGS="-DBTHOME_NO_MBEDTLS_CCM=1"
if command -v brew >/dev/null 2>&1 && MBEDTLS_PREFIX=$(brew --prefix mbedtls 2>/dev/null) \
   && [ -f "$MBEDTLS_PREFIX/include/mbedtls/ccm.h" ] && [ -f "$MBEDTLS_PREFIX/lib/libmbedcrypto.a" ]; then
    PROBE_BIN=$(mktemp /tmp/bthome_mbedtls_probe.XXXXXX)
    PROBE_OK=0
    for TRY_ARCH in "" "-arch x86_64"; do
        if $CC $TRY_ARCH -I"$MBEDTLS_PREFIX/include" -o "$PROBE_BIN" -x c - -x none "$MBEDTLS_PREFIX/lib/libmbedcrypto.a" \
             > /dev/null 2>&1 <<'EOF' && "$PROBE_BIN" > /dev/null 2>&1
#include "mbedtls/ccm.h"
int main(void) { mbedtls_ccm_context c; mbedtls_ccm_init(&c); mbedtls_ccm_free(&c); return 0; }
EOF
        then
            BTHOME_CCM_FLAGS="$TRY_ARCH -I$MBEDTLS_PREFIX/include $MBEDTLS_PREFIX/lib/libmbedcrypto.a"
            PROBE_OK=1
            break
        fi
    done
    rm -f "$PROBE_BIN"
    if [ "$PROBE_OK" -ne 1 ]; then
        echo "NOTE: host mbedtls found but would not link/run -- test_bthome will skip its encrypted-vector case (device build covers it)" >&2
    fi
else
    echo "NOTE: host mbedtls not found -- test_bthome will skip its encrypted-vector case (device build covers it)" >&2
fi
$CC -Wall -Wextra -Werror -I../../components/bthome/include -I../../components/capability/include \
    test_bthome.c ../../components/bthome/bthome.c ../../components/capability/capability.c \
    $BTHOME_CCM_FLAGS -o test_bthome -lm
./test_bthome

# The pure (no-NVS) half of bindkey.c's logic -- the dev_id -> NVS-key hash
# and the collision-safety verify check -- split into bindkey_core.c
# specifically so it can be exercised here without ESP-IDF's nvs_flash.
$CC -Wall -Wextra -Werror -I../../components/bthome/include \
    test_bindkey_core.c ../../components/bthome/bindkey_core.c -o test_bindkey_core
./test_bindkey_core

# Guards cfg.max_uri_handlers against the number of routes actually
# registered. Pure text check over the webserver sources -- see the file's own
# top comment for the boot loop it exists to prevent.
$CC -Wall -Wextra -Werror test_uri_handler_budget.c -o test_uri_handler_budget
./test_uri_handler_budget
