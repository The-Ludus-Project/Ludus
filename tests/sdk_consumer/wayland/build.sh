#!/usr/bin/env bash
# Run from the checkout after scripts/install-sdk linux-clang-development.
set -euo pipefail
destination=$1
mkdir -p "$destination/source"
cp tests/sdk_consumer/wayland/CMakeLists.txt "$destination/source/"
cp -R apps/world_demo "$destination/source/"
tools="$PWD/out/host-tools/venv/bin"
"$tools/cmake" -S "$destination/source" -B "$destination/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="$tools/ninja" \
    -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
    -DCMAKE_PREFIX_PATH="$PWD/out/install/linux-clang-development" \
    -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc" \
    -DLUDUS_SPIRV_VALIDATOR="$PWD/out/shader-tools/spirv-tools/usr/bin/spirv-val" \
    -DLUDUS_SPIRV_CROSS="$PWD/out/shader-tools/spirv-cross/bin/spirv-cross"
"$tools/cmake" --build "$destination/build" --target ludus_world_demo
