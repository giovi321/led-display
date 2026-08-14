#!/bin/bash
# Build helper for development on a Windows host with the toolchain in WSL.
# Run INSIDE WSL:  bash /mnt/z/git/led-display/Firmware/tools/wsl_build.sh
#
# Mirrors the Firmware tree from the Windows checkout into ~/led-display/src
# (network-drive builds are slow and fragile), then runs the ESP-IDF build
# with the build dir and sdkconfig kept OUTSIDE the mirrored tree so a
# re-sync never nukes them.
#
#   SRC=...   override the source checkout (default /mnt/z/git/led-display/Firmware)
#   TESTS=1   also build+run the host unit tests
set -e

SRC="${SRC:-/mnt/z/git/led-display/Firmware}"
WORK="$HOME/led-display"
DST="$WORK/src"

rm -rf "$DST"
mkdir -p "$DST"
(cd "$SRC" && tar cf - \
    --exclude=./build --exclude=./managed_components \
    --exclude=./sdkconfig --exclude=./sdkconfig.old \
    --exclude=./dependencies.lock .) | tar xf - -C "$DST"

if [ "${TESTS:-0}" = "1" ]; then
    (cd "$DST/test/host" && sh ./run_tests.sh)
fi

export IDF_TARGET=esp32s3
# shellcheck disable=SC1090
source "$HOME/esp-idf/export.sh" >/dev/null

cd "$DST"
idf.py -B "$WORK/build" -DSDKCONFIG="$WORK/sdkconfig" build
