#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 24.04 does not package the pinned LLVM; use its pinned apt.llvm.org suite.
python3 scripts/python/llvm_apt_source.py
packages=(gdb clang-23 llvm-23 lld-23 clang-format-23 clang-tidy-23 libclang-rt-23-dev python3-venv ccache libwayland-dev wayland-protocols libwayland-bin)
if [[ "$WITH_EDITOR" == true ]]; then
  # Preview is included in editor setup; offscreen rendering tests
  # still need a Vulkan loader and Mesa's software device.
  packages+=(qt6-base-dev qt6-declarative-dev qt6-wayland libvulkan1 mesa-vulkan-drivers)
fi
# Bound each network attempt, including mirrors that keep sending data
# slowly enough to avoid APT's inactivity timeout.
apt_options=(-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::Retries=3)
APT_ATTEMPTS=3
fetch_with_apt() {
  local limit="$1"
  shift
  local attempt
  local status=0
  for ((attempt = 1; attempt <= APT_ATTEMPTS; ++attempt)); do
    status=0
    sudo timeout --signal=TERM --kill-after=30s "$limit" apt-get "${apt_options[@]}" "$@" || status=$?
    if [[ "$status" -eq 0 ]]; then
      return 0
    fi
    echo "::warning::apt-get $1 attempt $attempt of $APT_ATTEMPTS failed with exit status $status"
  done
  return "$status"
}
fetch_with_apt 5m update -qq
fetch_with_apt 10m install --download-only -y -qq "${packages[@]}"
# The download deadline must never interrupt dpkg mid-transaction.
# Installation uses the cached packages and the overall CI job timeout.
sudo apt-get install --no-download -y -qq "${packages[@]}"
