#include "internal/adapter.h"

#include <ludus/foundation/base/core.h>

namespace ludus::graphics::rhi
{
namespace
{
using foundation::uint8;
using foundation::usize;

/// UTF-8 continuation bytes have the bit pattern 10xxxxxx.
constexpr uint8 UTF8_CONTINUATION_MASK = 0xC0;
constexpr uint8 UTF8_CONTINUATION_BITS = 0x80;

bool isAdapterRequested = false;
AdapterIdentity gAdapterRequest;
} // namespace

AdapterStatus GetAdapterCatalog(AdapterCatalog& outCatalog) noexcept
{
    AdapterCatalog catalog;
    const AdapterStatus status = backend::EnumerateAdapters(catalog);
    if (status == AdapterStatus::Ready)
    {
        outCatalog = catalog;
    }
    return status;
}

namespace internal
{
void SetAdapterRequest(const AdapterIdentity& adapter) noexcept
{
    isAdapterRequested = adapter.IsAutomatic() == false;
    gAdapterRequest = isAdapterRequested == true ? adapter : AdapterIdentity{};
}

const AdapterIdentity* GetRequestedAdapterOrNull() noexcept
{
    return isAdapterRequested == true ? &gAdapterRequest : nullptr;
}

void AssignAdapterName(std::string_view reported, AdapterName& outName) noexcept
{
    usize size = reported.size() < MAX_ADAPTER_NAME_BYTES ? reported.size() : MAX_ADAPTER_NAME_BYTES;
    if (size < reported.size())
    {
        // Step back over continuation bytes so the kept prefix ends on a code point boundary.
        while (size > 0 && (static_cast<uint8>(reported[size]) & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_BITS)
        {
            --size;
        }
    }
    const foundation::StringStatus status = outName.TryAssign(reported.substr(0, size));
    LUDUS_ASSERT(status == foundation::StringStatus::Ok);
    static_cast<void>(status);
}

AdapterInfo* ReserveCatalogEntryOrNull(AdapterCatalog& inoutCatalog) noexcept
{
    ++inoutCatalog.ReportedCount;
    if (inoutCatalog.Count == MAX_CATALOG_ADAPTERS)
    {
        return nullptr;
    }
    AdapterInfo* entry = &inoutCatalog.Adapters[inoutCatalog.Count];
    ++inoutCatalog.Count;
    *entry = {};
    entry->Identity.Api = inoutCatalog.Api;
    return entry;
}
} // namespace internal
} // namespace ludus::graphics::rhi
