#include "internal/d3d12_device.h"
#include "internal/d3d12_diagnostics.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include <cstring>

// Thanks to the Microsoft PIX team, "WinPixEventRuntime", ABI Usage, for the
// optional command-list exports. The external PIX process owns GPU capture.
// https://devblogs.microsoft.com/pix/winpixeventruntime/
// Thanks to Microsoft, "Use DRED to diagnose GPU faults", for pre-device settings
// and post-removal breadcrumb and allocation reports:
// https://learn.microsoft.com/windows/win32/direct3d12/use-dred
namespace ludus::graphics::rhi::backend::d3d12
{
namespace
{
constexpr logging::LogCategory LOG_RHI{"RHI"};
constexpr usize MAX_DRED_NODES_PER_LIST = 16;
constexpr usize MAX_DRED_NAME_UNITS = 128;
constexpr usize MAX_UTF8_BYTES_PER_UTF16_UNIT = 3;
using DiagnosticName = StaticArray<char, MAX_DRED_NAME_UNITS * MAX_UTF8_BYTES_PER_UTF16_UNIT + 1>;

using PixBeginEvent = void(WINAPI*)(ID3D12GraphicsCommandList*, UINT64, PCSTR);
using PixEndEvent = void(WINAPI*)(ID3D12GraphicsCommandList*);
constexpr uint64 PIX_EVENT_COLOR = 0xff65a8e0;
HMODULE gPixRuntimeOrNull = nullptr;
PixBeginEvent gPixBeginEventOrNull = nullptr;
PixEndEvent gPixEndEventOrNull = nullptr;
bool hasLoggedRemoval = false;

template <typename Procedure>
Procedure LoadPixProcedureOrNull(const char* name) noexcept
{
    const FARPROC exportOrNull = GetProcAddress(gPixRuntimeOrNull, name);
    static_assert(sizeof(Procedure) == sizeof(FARPROC));
    Procedure procedureOrNull = nullptr;
    std::memcpy(&procedureOrNull, &exportOrNull, sizeof(procedureOrNull));
    return procedureOrNull;
}

DiagnosticName CopyDiagnosticName(const char* narrowOrNull, const wchar_t* wideOrNull) noexcept
{
    DiagnosticName name{};
    if (wideOrNull != nullptr)
    {
        int32 units = 0;
        while (units < static_cast<int32>(MAX_DRED_NAME_UNITS) && wideOrNull[units] != L'\0')
        {
            ++units;
        }
        if (units != 0)
        {
            // Flags zero replaces an incomplete UTF-16 sequence at the truncation boundary.
            static_cast<void>(WideCharToMultiByte(CP_UTF8,
                                                  0,
                                                  wideOrNull,
                                                  units,
                                                  name.GetData(),
                                                  static_cast<int32>(name.GetSize() - 1),
                                                  nullptr,
                                                  nullptr));
        }
    }
    else if (narrowOrNull != nullptr)
    {
        for (usize index = 0; index < MAX_DRED_NAME_UNITS && narrowOrNull[index] != '\0'; ++index)
        {
            name[index] = narrowOrNull[index];
        }
    }
    return name;
}

void LogBreadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE* nodeOrNull) noexcept
{
    usize count = 0;
    while (nodeOrNull != nullptr && count < MAX_DRED_NODES_PER_LIST)
    {
        const DiagnosticName queue =
            CopyDiagnosticName(nodeOrNull->pCommandQueueDebugNameA, nodeOrNull->pCommandQueueDebugNameW);
        const DiagnosticName list =
            CopyDiagnosticName(nodeOrNull->pCommandListDebugNameA, nodeOrNull->pCommandListDebugNameW);
        if (nodeOrNull->pLastBreadcrumbValue != nullptr)
        {
            const uint32 completed = *nodeOrNull->pLastBreadcrumbValue;
            LUDUS_LOG_ERROR(LOG_RHI,
                            "DRED breadcrumb queue '{}' list '{}' completed {}/{} operations",
                            queue.GetData(),
                            list.GetData(),
                            completed,
                            nodeOrNull->BreadcrumbCount);
            if (completed < nodeOrNull->BreadcrumbCount && nodeOrNull->pCommandHistory != nullptr)
            {
                LUDUS_LOG_ERROR(LOG_RHI,
                                "DRED first uncompleted operation enum {}",
                                static_cast<uint32>(nodeOrNull->pCommandHistory[completed]));
            }
        }
        else
        {
            LUDUS_LOG_ERROR(LOG_RHI,
                            "DRED breadcrumb queue '{}' list '{}' completion unavailable",
                            queue.GetData(),
                            list.GetData());
        }
        nodeOrNull = nodeOrNull->pNext;
        ++count;
    }
    LUDUS_LOG_ERROR(LOG_RHI, "DRED breadcrumb nodes logged {} (truncated: {})", count, nodeOrNull != nullptr);
}

void LogAllocations(const char* category, const D3D12_DRED_ALLOCATION_NODE* nodeOrNull) noexcept
{
    usize count = 0;
    while (nodeOrNull != nullptr && count < MAX_DRED_NODES_PER_LIST)
    {
        const DiagnosticName name = CopyDiagnosticName(nodeOrNull->ObjectNameA, nodeOrNull->ObjectNameW);
        LUDUS_LOG_ERROR(LOG_RHI,
                        "DRED {} allocation '{}' type {}",
                        category,
                        name.GetData(),
                        static_cast<uint32>(nodeOrNull->AllocationType));
        nodeOrNull = nodeOrNull->pNext;
        ++count;
    }
    LUDUS_LOG_ERROR(LOG_RHI, "DRED {} allocations logged {} (truncated: {})", category, count, nodeOrNull != nullptr);
}
} // namespace

void InitializePixMarkers() noexcept
{
    // Opt-in deployment beside the executable. Exclude the working directory
    // and PATH from DLL search, and leave the renderer usable without the DLL.
    gPixRuntimeOrNull = LoadLibraryExW(L"WinPixEventRuntime.dll",
                                       nullptr,
                                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (gPixRuntimeOrNull != nullptr)
    {
        gPixBeginEventOrNull = LoadPixProcedureOrNull<PixBeginEvent>("PIXBeginEventOnCommandList");
        gPixEndEventOrNull = LoadPixProcedureOrNull<PixEndEvent>("PIXEndEventOnCommandList");
        if (gPixBeginEventOrNull == nullptr || gPixEndEventOrNull == nullptr)
        {
            ReleasePixMarkers();
        }
    }
}

void ReleasePixMarkers() noexcept
{
    gPixBeginEventOrNull = nullptr;
    gPixEndEventOrNull = nullptr;
    if (gPixRuntimeOrNull != nullptr)
    {
        FreeLibrary(gPixRuntimeOrNull);
        gPixRuntimeOrNull = nullptr;
    }
}

void BeginPixEvent(ID3D12GraphicsCommandList& inoutCommands, const char* name) noexcept
{
    if (gPixBeginEventOrNull != nullptr)
    {
        gPixBeginEventOrNull(&inoutCommands, PIX_EVENT_COLOR, name);
    }
}

void EndPixEvent(ID3D12GraphicsCommandList& inoutCommands) noexcept
{
    if (gPixEndEventOrNull != nullptr)
    {
        gPixEndEventOrNull(&inoutCommands);
    }
}

void ConfigureDred() noexcept
{
    hasLoggedRemoval = false;
#if defined(LUDUS_BUILD_DEBUG) || defined(LUDUS_BUILD_DEVELOPMENT)
    ID3D12DeviceRemovedExtendedDataSettings* settingsOrNull = nullptr;
    const HRESULT result =
        D3D12GetDebugInterface(IID_ID3D12DeviceRemovedExtendedDataSettings, GetInterfaceOutput(settingsOrNull));
    if (SUCCEEDED(result) == true && settingsOrNull != nullptr)
    {
        const ComOwner<ID3D12DeviceRemovedExtendedDataSettings> settings(settingsOrNull);
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    else
    {
        LUDUS_LOG_INFO(LOG_RHI, "DRED configuration unavailable (HRESULT {:x})", static_cast<uint32>(result));
    }
#endif
}

void LogDeviceRemoval(ID3D12Device* deviceOrNull) noexcept
{
    if (deviceOrNull == nullptr || hasLoggedRemoval == true)
    {
        return;
    }
    const HRESULT reason = deviceOrNull->GetDeviceRemovedReason();
    if (reason == S_OK)
    {
        return;
    }
    hasLoggedRemoval = true;
    LUDUS_LOG_ERROR(LOG_RHI,
                    "Direct3D 12 device removed (HRESULT {:x}). Recreate the device",
                    static_cast<uint32>(reason));

    ID3D12DeviceRemovedExtendedData* dataOrNull = nullptr;
    const HRESULT result =
        deviceOrNull->QueryInterface(IID_ID3D12DeviceRemovedExtendedData, GetInterfaceOutput(dataOrNull));
    if (FAILED(result) == true || dataOrNull == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "DRED interface unavailable (HRESULT {:x})", static_cast<uint32>(result));
        return;
    }
    const ComOwner<ID3D12DeviceRemovedExtendedData> data(dataOrNull);
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    const HRESULT breadcrumbResult = data->GetAutoBreadcrumbsOutput(&breadcrumbs);
    if (SUCCEEDED(breadcrumbResult) == true)
    {
        LogBreadcrumbs(breadcrumbs.pHeadAutoBreadcrumbNode);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_RHI, "DRED breadcrumbs unavailable (HRESULT {:x})", static_cast<uint32>(breadcrumbResult));
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    const HRESULT pageFaultResult = data->GetPageFaultAllocationOutput(&pageFault);
    if (SUCCEEDED(pageFaultResult) == true)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "DRED page fault GPU virtual address {:x}", pageFault.PageFaultVA);
        LogAllocations("live", pageFault.pHeadExistingAllocationNode);
        LogAllocations("recently freed", pageFault.pHeadRecentFreedAllocationNode);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_RHI,
                        "DRED page-fault data unavailable (HRESULT {:x})",
                        static_cast<uint32>(pageFaultResult));
    }
}
} // namespace ludus::graphics::rhi::backend::d3d12
