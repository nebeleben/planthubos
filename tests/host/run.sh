#!/bin/sh
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
$CC -Wall -Wextra -Werror -I../../components/app_config/include \
    test_creds_validate.c ../../components/app_config/creds_validate.c -o test_creds_validate
./test_creds_validate
