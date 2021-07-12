#!/bin/bash

set -eo pipefail

cd build
if [ $SELF_HOST = false ]; then
    ctest -j 4 --output-on-failure -C $BUILD_TYPE
else
    sudo cp libsnmallocshim.so libsnmallocshim-checks.so /usr/local/lib/
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim.so ninja
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim-checks.so ninja
fi