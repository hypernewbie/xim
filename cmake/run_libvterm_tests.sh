#!/bin/sh
# Run the vendored libvterm test harness in an isolated build-tree copy.

set -eu

SOURCE_DIR="$1"
WORK_DIR="$2"
CC="$3"
LIBTOOL="$4"
CFLAGS="$5"
LDFLAGS="$6"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
cp -a "$SOURCE_DIR"/. "$WORK_DIR"/
cd "$WORK_DIR"

timeout -k 5s 180s make --no-print-directory -f Makefile test \
    "CC=$CC" \
    "LIBTOOL=$LIBTOOL" \
    "CFLAGS=$CFLAGS" \
    "LDFLAGS=$LDFLAGS"
