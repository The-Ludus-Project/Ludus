#!/usr/bin/env bash
set -euo pipefail

# Keep the venv at the absolute path recorded in its executable shebangs.
image_dir=$(dirname "$0")
mkdir -p out/host-tools
ln -s "$image_dir/venv" out/host-tools/venv
if [[ "$WITH_EDITOR" == true ]]; then
    mkdir -p out/shader-tools
    cp "$image_dir"/shader-archives/* out/shader-tools/
fi
