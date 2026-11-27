#!/usr/bin/env bash
# Set aside a hosted runner's APT sources that no Ludus job installs from.
# Usage: use-required-sources.sh
#
# APT fails an index update when any configured source fails. Batch 45920191091
# lost every job after the hosted image's packages.microsoft.com source answered
# 403 Forbidden, although Ludus installs only Ubuntu archive packages and the
# pinned apt.llvm.org suite. On a GitHub-hosted runner, which is discarded after
# the job, this moves every other source file out of sources.list.d before the
# first update. Self-hosted runners and other machines are left unchanged.
set -euo pipefail

if [[ "${RUNNER_ENVIRONMENT:-}" != github-hosted ]]; then
  exit 0
fi
source_parts="${LUDUS_APT_SOURCE_PARTS:-/etc/apt/sources.list.d}"
set_aside="${LUDUS_APT_SET_ASIDE_SOURCES:-/etc/apt/ludus-unused-sources}"
required_host='^([[:alnum:]-]+\.)*ubuntu\.com$|^apt\.llvm\.org$'
shopt -s nullglob
for source in "$source_parts"/*.list "$source_parts"/*.sources; do
  is_required=true
  hosts=$(grep -v '^[[:space:]]*#' "$source" | grep -oE '[[:alpha:]+]+://[^/[:space:]]+' | sed -E 's|^[^:]+://||; s|:[0-9]+$||' || true)
  for host in $hosts; do
    if [[ ! "$host" =~ $required_host ]]; then
      is_required=false
    fi
  done
  if [[ "$is_required" == false ]]; then
    sudo mkdir -p "$set_aside"
    sudo mv "$source" "$set_aside/"
    echo "Set aside unused APT source ${source##*/} (${hosts//$'\n'/ })."
  fi
done
