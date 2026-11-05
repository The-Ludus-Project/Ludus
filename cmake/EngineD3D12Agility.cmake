include(FetchContent)
include(LudusD3D12Agility)

# Microsoft DirectX team, "DirectX 12 Agility SDK Downloads":
# https://devblogs.microsoft.com/directx/directx12agility/
# Keep the NuGet release and the loader's numeric SDK version paired.
set(LUDUS_D3D12_AGILITY_PACKAGE_VERSION "1.620.1")
set(LUDUS_D3D12_AGILITY_SDK_VERSION 620)
set(LUDUS_D3D12_AGILITY_ARCHIVE_SHA256 "a8a494c3e5f687d9be0a465ec8248da70781ab689d888db8806b600422722be6")
set(agility_package "microsoft.direct3d.d3d12")
FetchContent_Declare(ludus_d3d12_agility
    URL "https://api.nuget.org/v3-flatcontainer/${agility_package}/${LUDUS_D3D12_AGILITY_PACKAGE_VERSION}/${agility_package}.${LUDUS_D3D12_AGILITY_PACKAGE_VERSION}.nupkg"
    # CMake 3.29 recognizes the NuGet archive as ZIP by the local extension.
    DOWNLOAD_NAME "${agility_package}.${LUDUS_D3D12_AGILITY_PACKAGE_VERSION}.zip"
    URL_HASH "SHA256=${LUDUS_D3D12_AGILITY_ARCHIVE_SHA256}"
    TLS_VERIFY TRUE
)
FetchContent_MakeAvailable(ludus_d3d12_agility)
unset(agility_package)

set(LUDUS_D3D12_AGILITY_BINARIES "${ludus_d3d12_agility_SOURCE_DIR}/build/native/bin/x64")
set(LUDUS_D3D12_AGILITY_LICENSE "${ludus_d3d12_agility_SOURCE_DIR}/LICENSE.txt")
