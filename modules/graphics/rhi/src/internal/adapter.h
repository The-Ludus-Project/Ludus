#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/adapter.h>

#include <string_view>

// Private R14a adapter catalog hooks. Each native backend lists its adapters,
// describes the adapter of its live session and honours the requested identity
// during selection; device.cpp checks the request against the catalog before
// startup and the selected adapter after Ready.
namespace ludus::graphics::rhi::backend
{
/// Enumerate the backend's adapters into outCatalog; preserve it unless Ready.
[[nodiscard]] AdapterStatus EnumerateAdapters(AdapterCatalog& outCatalog) noexcept;
/// Describe the adapter of the live session; false (outAdapter unchanged) without one.
[[nodiscard]] bool GetSelectedAdapter(AdapterInfo& outAdapter) noexcept;
} // namespace ludus::graphics::rhi::backend

namespace ludus::graphics::rhi::internal
{
/// Copy the identity requested by the next CreateDevice; automatic clears the request.
void SetAdapterRequest(const AdapterIdentity& adapter) noexcept;
/// Requested identity, or null when the automatic choice applies.
[[nodiscard]] const AdapterIdentity* GetRequestedAdapterOrNull() noexcept;
/// Store a reported UTF-8 name, shortened at a code point boundary to the name capacity; a name
/// with an ill-formed sequence keeps only the well-formed bytes before it.
void AssignAdapterName(std::string_view reported, AdapterName& outName) noexcept;
/// Count one reported adapter and return the catalog slot for it, or null once full.
[[nodiscard]] AdapterInfo* ReserveCatalogEntryOrNull(AdapterCatalog& inoutCatalog) noexcept;
} // namespace ludus::graphics::rhi::internal
