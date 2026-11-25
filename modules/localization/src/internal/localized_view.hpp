#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/foundation/strings/shared_string.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/localization/locale_publication.hpp>

namespace ludus::localization::detail
{
using CatalogLabel = foundation::StaticString<kMaxIdentifierBytes>;

struct DomainCatalog final
{
    CatalogLabel Name;
    Catalog Package;
};

// Filled only by the publication owner while loading. No slot changes once a
// view can retain the generation. Cooked packages already own fallback bytes.
struct CatalogGeneration final
{
    CatalogLabel Locale;
    LocaleRequest Request;
    uint64 ReleaseRevision{};
    uint64 PreferenceRevision{};
    foundation::core::FixedArray<DomainCatalog, MAX_LOCALE_DOMAINS> Catalogs;
    usize MissingCatalogCount{};

    [[nodiscard]] const Catalog* FindCatalogOrNull(std::string_view name) const noexcept;
};

struct LocalizedEntry final
{
    foundation::SharedString Formatted;
    ResolvedText Resolved;
};

struct LocalizedView final
{
    foundation::SharedPtr<const CatalogGeneration> Generation;
    uint64 Revision{};
    foundation::core::StaticArray<LocalizedEntry, MAX_VISIBLE_MESSAGES> Entries;
    usize MessageCount{};
};

[[nodiscard]] Status PrepareLocalizedView(const foundation::SharedPtr<const CatalogGeneration>& generation,
                                          uint64 viewRevision,
                                          foundation::BorrowedView<const VisibleMessage> messages,
                                          const foundation::AllocationDomain& domain,
                                          foundation::SharedPtr<const LocalizedView>& outView) noexcept;
} // namespace ludus::localization::detail
