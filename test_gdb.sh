#!/bin/sh
BUILD_DIR=build-debian
export SAMOSA_COMPILED_GATEWAY="gdb -ex run -ex \"bt\" -ex \"quit\" --args $PWD/build-debian/samosa-gateway"
export SAMOSA_COMPILED_JOBSD="$PWD/build-debian/samosa-jobsd"
export SAMOSA_FAKE_BACKEND="$PWD/build-debian/test_fake_openai_backend"
export SAMOSA_FS="$PWD/build-debian/samosa-fs"
sh tests/test_compiled_gateway.sh >/tmp/out.log 2>&1
cat /tmp/out.log
