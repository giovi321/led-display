#!/bin/sh
# Host-side unit tests for the pure firmware modules (mapping.c, valuefmt.c,
# fonts.c, wifi_pick.c, light_curve.c, devname.c).
# No ESP-IDF needed: plain C, plain gcc, plain assert.
#   ./Firmware/test/host/run_tests.sh   (any working directory)
set -e
cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -I../../main"

$CC $CFLAGS -o test_mapping   test_mapping.c   ../../main/mapping.c
$CC $CFLAGS -o test_valuefmt  test_valuefmt.c  ../../main/valuefmt.c
$CC $CFLAGS -o test_fonts     test_fonts.c     ../../main/fonts.c
$CC $CFLAGS -o test_wifi_pick test_wifi_pick.c ../../main/wifi_pick.c
$CC $CFLAGS -o test_devname   test_devname.c   ../../main/devname.c
# light_curve uses powf for the gamma shape, so this one needs libm.
$CC $CFLAGS -o test_light_curve test_light_curve.c ../../main/light_curve.c -lm

./test_mapping
./test_valuefmt
./test_fonts
./test_wifi_pick
./test_devname
./test_light_curve
echo "ALL HOST TESTS PASSED"
