#include <windows.h>

// Thanks to Microsoft's DirectX team, "Getting Started with the Agility SDK",
// section 2, for the process-executable export contract.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = LUDUS_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* const D3D12SDKPath = ".\\" LUDUS_D3D12_SDK_DIRECTORY "\\";
}
