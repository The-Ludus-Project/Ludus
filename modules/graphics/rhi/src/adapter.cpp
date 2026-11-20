#include "internal/adapter.h"
#include "internal/owner.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/utf8.hpp>

namespace ludus::graphics::rhi
{
namespace
{
using foundation::usize;

/// Explicit adapter request of one owner slot (R14c); each owner may select its own adapter.
struct AdapterRequest final
{
    bool IsRequested = false;
    AdapterIdentity Identity;
};
foundation::StaticArray<AdapterRequest, MAX_DEVICE_OWNERS> gAdapterRequests{};
/// The adapter request of the bound owner slot.
AdapterRequest& GetAdapterRequest() noexcept
{
    return gAdapterRequests[internal::GetBoundOwnerSlot()];
}
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
    GetAdapterRequest().IsRequested = adapter.IsAutomatic() == false;
    GetAdapterRequest().Identity = GetAdapterRequest().IsRequested == true ? adapter : AdapterIdentity{};
}

const AdapterIdentity* GetRequestedAdapterOrNull() noexcept
{
    return GetAdapterRequest().IsRequested == true ? &GetAdapterRequest().Identity : nullptr;
}

void AssignAdapterName(std::string_view reported, AdapterName& outName) noexcept
{
    foundation::Utf8View name;
    usize errorOffset = 0;
    if (foundation::ValidateUtf8(reported, name, errorOffset) != foundation::StringStatus::Ok)
    {
        // Keep the well-formed prefix; the bytes before the first ill-formed sequence are valid UTF-8.
        const foundation::StringStatus status =
            foundation::ValidateUtf8(reported.substr(0, errorOffset), name, errorOffset);
        LUDUS_ASSERT(status == foundation::StringStatus::Ok);
        static_cast<void>(status);
    }
    bool isTruncated = false;
    outName.AssignTruncatedUtf8(name, isTruncated);
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
