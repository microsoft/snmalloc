#!/bin/bash

cd build
if [ $SELF_HOST = false ]; then
    ctest -j 4 --output-on-failure -C $BUILD_TYPE
else
    sudo cp libsnmallocshim.so libsnmallocshim-16mib.so libsnmallocshim-oe.so /usr/local/lib/
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim.so ninja
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim-16mib.so ninja
    ninja clean
    LD_PRELOAD=/usr/local/lib/libsnmallocshim-oe.so ninja
fi