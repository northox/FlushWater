#!/usr/bin/env bash
# Build and run the host-side decision tests. No ESP32 required.
#   exit 0 = all good      exit 1 = regression      exit 2 = known bugs present
set -u
cd "$(dirname "$0")"
g++ -std=gnu++17 -O0 -g -I stubs -o /tmp/flushwater_tests test_decide.cpp \
    -Wno-write-strings 2>&1 | head -30
[ -x /tmp/flushwater_tests ] || { echo "BUILD FAILED"; exit 3; }
/tmp/flushwater_tests
