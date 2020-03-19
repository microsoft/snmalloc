#!/bin/bash

set -ex

export CC=/usr/bin/clang-9
export CXX=/usr/bin/clang++-9

mkdir build
cd build
cmake -GNinja -DSNMALLOC_CI_BUILD=ON -DSNMALLOC_QEMU_WORKAROUND=ON ..
ninja