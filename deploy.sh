#!/bin/bash

set -e

cd $(dirname "${0}")

ssh root@192.168.51.1 "mount / -o remount,rw"

rm -rf build-aarch64

source ~/Source/MOD/mod-plugin-builder/local.env darkglass-anagram
# source ~/Source/MOD/mod-plugin-builder/local.env moddwarf-new

cmake -S . -B build-aarch64 -DCMAKE_BUILD_TYPE=Release
$(which cmake) --build build-aarch64 -j $(nproc)

scp build-aarch64/jack-audio-bridge*.so root@192.168.51.1:/lib/jack/
