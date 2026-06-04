#!/bin/bash
if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi

set -e
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-i686.cmake ..
make
cd ..
./mkiso.sh
