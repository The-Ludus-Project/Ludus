#pragma once

// Private read access to a validated package version 2 for the formatter. The
// wire layout stays owned by catalog.cpp; these accessors decode its fields.

#include <ludus/foundation/base/types.h>

#include <ludus/localization/catalog.hpp>

#include <string_view>

namespace ludus::localization::detail
{
/// One validated message record of a version 2 catalog.
struct MessageRecord final
{
    std::string_view Value; ///< Static text or dynamic pattern.
    bool IsDynamic{};       ///< Dynamic records declare at least one argument.
    uint32 Provider{};      ///< Index into the provider path.
    uint32 FirstArgument{}; ///< First argument record index.
    uint32 ArgumentCount{}; ///< Declared arguments, at most MAX_ARGUMENTS.
};

/// One validated argument declaration of a version 2 catalog.
struct ArgumentRecord final
{
    std::string_view Name;
    ArgumentType Type{ArgumentType::Text};
    TextOrigin Origin{TextOrigin::Game};
    UnknownPolicy Unknown{UnknownPolicy::Reject};
    uint32 MaxBytes{};
    int64 Minimum{};
    int64 Maximum{};
    uint32 FirstValue{};
    uint32 ValueCount{};
};

/// Decoders over a valid catalog; callers pass indices the catalog validated.
struct CatalogAccess final
{
    [[nodiscard]] static uint32 GetVersion(const Catalog& catalog) noexcept;
    /// Reports whether binding was made by catalog (or a copy of it).
    [[nodiscard]] static bool IsBindingOf(const Catalog& catalog, MessageBinding binding) noexcept;
    [[nodiscard]] static uint32 GetIndex(MessageBinding binding) noexcept;
    [[nodiscard]] static MessageRecord GetMessage(const Catalog& catalog, uint32 index) noexcept;
    [[nodiscard]] static ArgumentRecord GetArgument(const Catalog& catalog, uint32 argument) noexcept;
    [[nodiscard]] static std::string_view GetEnumValue(const Catalog& catalog, uint32 value) noexcept;
    [[nodiscard]] static std::string_view GetProviderLocale(const Catalog& catalog, uint32 provider) noexcept;
    [[nodiscard]] static Origin GetOrigin(const Catalog& catalog, uint32 provider) noexcept;
};
} // namespace ludus::localization::detail
