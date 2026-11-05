cmake_minimum_required(VERSION 3.29)

# Multiple executables in one build may share a runtime output directory.
file(LOCK "${lock_file}" GUARD PROCESS TIMEOUT 60)
file(MAKE_DIRECTORY "${destination}")
file(COPY_FILE "${binaries}/D3D12Core.dll" "${destination}/D3D12Core.dll" ONLY_IF_DIFFERENT)
file(COPY_FILE "${binaries}/d3d12SDKLayers.dll" "${destination}/d3d12SDKLayers.dll" ONLY_IF_DIFFERENT)
file(COPY_FILE "${license}" "${destination}/LICENSE.txt" ONLY_IF_DIFFERENT)
