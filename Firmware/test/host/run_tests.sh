#!/bin/sh
# Host-side unit tests for the pure firmware modules (mapping.c, valuefmt.c).
# No ESP-IDF needed: plain C, plain gcc, plain assert.
#   cd Firmware/test/host && ./run_tests.sh
set -e

CC="${CC:-gcc}"
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -I../../main"

$CC $CFLAGS -o test_mapping  test_mapping.c  ../../main/mapping.c
$CC $CFLAGS -o test_valuefmt test_valuefmt.c ../../main/valuefmt.c

./test_mapping
./test_valuefmt
echo "ALL HOST TESTS PASSED"
