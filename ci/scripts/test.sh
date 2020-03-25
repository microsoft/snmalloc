#!/bin/bash

cd build
if [ $SELF_HOST = false ]; then
    ctest -j 4 --output-on-failure -C $BUILD_TYPE
else
    sudo cp libsnmallocshim.so libsnmallocshim-1mib.so /usr/local/lib/
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim.so ninja
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim-1mib.so ninja
fi