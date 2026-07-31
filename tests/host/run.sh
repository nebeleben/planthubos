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

$CC -Wall -Wextra -Werror -I../../components/mibeacon/include -I../../components/data_core/include \
    test_registry.c ../../components/data_core/registry.c ../../components/mibeacon/mibeacon.c -o test_registry
./test_registry

$CC -Wall -Wextra -Werror -I../../components/timekeeper/include \
    test_boottab.c ../../components/timekeeper/boottab.c -o test_boottab
./test_boottab

$CC -Wall -Wextra -Werror -I../../components/storage/include \
    test_storage.c ../../components/storage/storage.c -o test_storage
./test_storage

$CC -Wall -Wextra -Werror -I../../components/storage/include \
    test_hourly_agg.c ../../components/storage/hourly_agg.c -o test_hourly_agg
./test_hourly_agg

$CC -Wall -Wextra -Werror -I../../components/claiming/include \
    test_authtok.c ../../components/claiming/authtok.c -o test_authtok
./test_authtok
