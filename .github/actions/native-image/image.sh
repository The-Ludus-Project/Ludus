#!/usr/bin/env bash
set -euo pipefail

recipe=.github/actions/native-image/Dockerfile
inputs=("$recipe" .github/actions/native-image/use-tools.sh .github/actions/native-setup/install-packages.sh
        config/tool_versions.json config/apt/apt.llvm.org.asc scripts/python/llvm_apt_source.py
        config/shader_toolchain.json scripts/shader-probe)
recipe_key() {
    # Include Git paths, contents and executable bits. Build exactly these files.
    git ls-tree -r "$1" -- "${inputs[@]}" | sha256sum | cut -d' ' -f1
}
build_image() {
    git archive HEAD -- "${inputs[@]}" | docker build --pull -t "$tag" -f "$recipe" -
}
repository="ghcr.io/$(printf '%s' "$GITHUB_REPOSITORY" | tr '[:upper:]' '[:lower:]')-native-ci"
key=$(recipe_key HEAD)
tag="$repository:$key"

if [[ "$1" == verify ]]; then
    build_image
    # Configure an Editor checkout with the same tools the job will use. Do this
    # before merge even when a changed recipe has no published image yet.
    git archive HEAD | docker run --rm -i -e CI=true "$tag" bash -ec '
        mkdir /tmp/ludus && cd /tmp/ludus && tar -xf -
        WITH_EDITOR=true bash /opt/ludus-ci/use-tools.sh
        ./init.sh --cli --no-system-install --locked --with-tests --preset-only linux-clang-development --with-editor --configure-editor-only
    '
    exit 0
fi

# Isolate registry credentials from later steps and the Docker build context.
export DOCKER_CONFIG
DOCKER_CONFIG=$(mktemp -d)
trap 'rm -rf "$DOCKER_CONFIG"' EXIT
lookup_image() {
    local digest
    digest=$(docker buildx imagetools inspect "$tag" --format '{{.Manifest.Digest}}') || return 1
    [[ "$digest" =~ ^sha256:[a-f0-9]{64}$ ]] || return 1
    printf 'image=%s@%s\n' "$repository" "$digest" >> "$GITHUB_OUTPUT"
}

case "$1" in
    resolve)
        if [[ $(recipe_key "$BASE_SHA") != "$key" ]]; then
            echo 'recipe_changed=true' >> "$GITHUB_OUTPUT"
        fi
        if ! docker login ghcr.io -u "$GITHUB_ACTOR" --password-stdin <<< "$GH_TOKEN" || ! lookup_image; then
            echo 'No accessible image for these inputs. Using hosted native setup.'
        fi
        ;;
    publish)
        docker login ghcr.io -u "$GITHUB_ACTOR" --password-stdin <<< "$GH_TOKEN"
        if [[ "${REFRESH_IMAGE:-false}" != true ]] && lookup_image; then
            exit 0
        fi
        build_image
        docker push "$tag"
        lookup_image
        ;;
    *) echo "Unknown native image operation: $1" >&2; exit 2 ;;
esac
