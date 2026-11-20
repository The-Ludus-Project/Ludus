#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/config/config.hpp>

#include <span>
#include <string_view>

namespace ludus::runtime::configuration
{
/// One engine-owned configuration schema that tools can select by identity.
struct SchemaEntry final
{
    std::string_view Identity; ///< Versioned schema identity written into every bundle, such as `ludus.host.v1`.
    std::string_view Title;    ///< Short display name for tool and Editor selectors.
    std::span<const foundation::config::Descriptor> Settings; ///< The owning module's static descriptors.
};

/// @brief The engine schemas that `ludus-config` and the Editor can validate, host first.
///
/// A fixed table compiled into this library: no registration, discovery or global mutable state. Every span and
/// string is static and outlives any Context built from it.
[[nodiscard]] std::span<const SchemaEntry> GetSchemaCatalog() noexcept;

/// @brief Find a catalog schema by its exact identity.
/// @return The entry, or null when no engine schema has that identity (including a different version).
[[nodiscard]] const SchemaEntry* FindSchemaOrNull(std::string_view identity) noexcept;
} // namespace ludus::runtime::configuration
