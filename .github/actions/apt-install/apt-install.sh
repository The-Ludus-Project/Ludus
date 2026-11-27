#!/usr/bin/env bash
# Install Ubuntu packages on a hosted runner with bounded, retried downloads.
# Usage: apt-install.sh [--no-install-recommends] package...
#
# A stalled package mirror once held a required check for 37 minutes inside an
# unbounded apt-get install; jobs without a timeout would have waited 6 hours.
# Every network attempt here has a deadline and is retried. Installation then
# uses only the downloaded archives, so a deadline never interrupts dpkg.
# The native setup and image keep the same policy in
# .github/actions/native-setup/install-packages.sh. Both first set aside hosted
# APT sources that no job installs from (use-required-sources.sh).
set -euo pipefail

install_options=(-y -qq)
packages=()
for argument in "$@"; do
  case "$argument" in
    --no-install-recommends) install_options+=("$argument") ;;
    -*) echo "apt-install.sh: unsupported option $argument" >&2; exit 2 ;;
    *) packages+=("$argument") ;;
  esac
done
if [[ ${#packages[@]} -eq 0 ]]; then
  echo "apt-install.sh: name at least one package" >&2
  exit 2
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
bash "$(dirname "${BASH_SOURCE[0]}")/use-required-sources.sh"
fetch_with_apt 5m update -qq
fetch_with_apt 10m install --download-only "${install_options[@]}" "${packages[@]}"
sudo DEBIAN_FRONTEND=noninteractive apt-get install --no-download "${install_options[@]}" "${packages[@]}"
