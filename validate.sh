#!/bin/bash

set -e

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

valgrind --leak-check=full --track-origins=yes ./build/audio-bridge-test $@
