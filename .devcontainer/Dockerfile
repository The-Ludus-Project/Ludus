# syntax=docker/dockerfile:1
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# The optional CA mount supports HTTPS package mirrors behind a trusted proxy.
# It is used only during the build and never copied into the image.
# Ubuntu 24.04 does not package the pinned LLVM, so the committed apt.llvm.org
# key and the pinned suite are registered before installing it (build context: repo root).
RUN --mount=type=secret,id=proxy_ca \
    --mount=type=bind,source=config,target=/tmp/ludus/config \
    --mount=type=bind,source=scripts/python/llvm_apt_source.py,target=/tmp/ludus/scripts/python/llvm_apt_source.py \
    set -eu; \
    if [ -f /run/secrets/proxy_ca ]; then \
        set -- -o Acquire::https::CaInfo=/run/secrets/proxy_ca; \
    fi; \
    apt-get "$@" update && \
    DEBIAN_FRONTEND=noninteractive apt-get "$@" install -y --no-install-recommends \
        build-essential ca-certificates git pkg-config python3 python3-venv && \
    python3 /tmp/ludus/scripts/python/llvm_apt_source.py --root /tmp/ludus && \
    apt-get "$@" update && \
    DEBIAN_FRONTEND=noninteractive apt-get "$@" install -y --no-install-recommends \
        clang-23 clang-tools-23 lld-23 clang-format-23 clang-tidy-23 \
        libclang-rt-23-dev && \
    rm -rf /var/lib/apt/lists/*

USER vscode
