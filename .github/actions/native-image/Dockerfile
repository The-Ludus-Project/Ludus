FROM ubuntu:24.04
LABEL org.opencontainers.image.source="https://github.com/The-Ludus-Project/Ludus"

# Tools normally supplied by the hosted VM, including zstd for Actions caches.
RUN apt-get -o Acquire::Retries=3 update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        build-essential ca-certificates curl file git pkg-config python3 sudo unzip xz-utils zstd && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/ludus-ci
COPY .github/actions/native-image/use-tools.sh use-tools.sh
COPY config/tool_versions.json config/tool_versions.json
COPY config/apt/apt.llvm.org.asc config/apt/apt.llvm.org.asc
COPY scripts/python/llvm_apt_source.py scripts/python/llvm_apt_source.py
COPY .github/actions/native-setup/install-packages.sh install-packages.sh
RUN DEBIAN_FRONTEND=noninteractive WITH_EDITOR=true bash install-packages.sh && \
    rm -rf /var/lib/apt/lists/* && \
    python3 -m venv venv && \
    python3 -c 'import json; print("\n".join(f"{k}=={v}" for k,v in json.load(open("config/tool_versions.json"))["managed"].items()))' > requirements.txt && \
    venv/bin/pip install --no-cache-dir -r requirements.txt

# Keep only archives. The normal shader bootstrap verifies hashes before extraction.
COPY scripts/shader-probe scripts/shader-probe
COPY config/shader_toolchain.json config/shader_toolchain.json
RUN ./scripts/shader-probe bootstrap && mkdir shader-archives && \
    mv out/shader-tools/slang.tar.gz out/shader-tools/spirv-tools.deb shader-archives/ && rm -rf out
