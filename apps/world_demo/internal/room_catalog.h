#pragma once
// The world demo's checked-in room catalog: each level's document identity maps
// to a nonzero foundation::uint16 room ordinal that enters every scatter placement identity
// (docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter).
// Ordinals are assigned once and never reused, so a retired room keeps its
// placements distinct from every later room.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <string_view>
namespace ludus::world_demo
{
/// One catalog entry.
struct RoomCatalogEntry final
{
    /// Document identity of the level's World record (identity-migration.json for version-1 rooms).
    foundation::identity::AssetId Document{};
    /// The level's settings `id`, which identifies committed version-1 rooms whose records carry no UUID.
    std::string_view LevelId;
    /// Nonzero ordinal, unique in the catalog and never reused.
    foundation::uint16 Room = 0;
};
/// Outcome of CheckRoomCatalog.
struct RoomCatalogCheck final
{
    /// Whether every entry is valid.
    bool IsValid = true;
    /// Index of the first rejected entry; zero when valid.
    foundation::usize Index = 0;
};
/// @brief Check a catalog: nonzero ordinals, non-nil documents, nonempty level IDs and no repeated field.
/// @param entries The catalog entries, in any order.
/// @param count Number of entries.
/// @return Validity and the index of the first entry that is zero, nil, empty or repeats an earlier entry.
[[nodiscard]] constexpr RoomCatalogCheck CheckRoomCatalog(const RoomCatalogEntry* entries,
                                                          foundation::usize count) noexcept
{
    for (foundation::usize i = 0; i < count; ++i)
    {
        if (entries[i].Room == 0 || entries[i].Document.IsNil() == true || entries[i].LevelId.empty() == true)
        {
            return { .IsValid = false, .Index = i };
        }
        for (foundation::usize j = 0; j < i; ++j)
        {
            if (entries[j].Room == entries[i].Room || entries[j].Document == entries[i].Document ||
                entries[j].LevelId == entries[i].LevelId)
            {
                return { .IsValid = false, .Index = i };
            }
        }
    }
    return {};
}
/// @brief Find a level's room ordinal: by document identity when the record carries one, else by level ID.
/// @return The ordinal, or zero when the catalog has no entry for the level.
[[nodiscard]] constexpr foundation::uint16 FindRoom(const RoomCatalogEntry* entries,
                                                    foundation::usize count,
                                                    foundation::identity::AssetId document,
                                                    std::string_view levelId) noexcept
{
    for (foundation::usize i = 0; i < count; ++i)
    {
        const bool isMatch =
            document.IsNil() == false ? entries[i].Document == document : entries[i].LevelId == levelId;
        if (isMatch == true)
        {
            return entries[i].Room;
        }
    }
    return 0;
}
/// Document UUIDs are the committed values in levels/identity-migration.json.
inline constexpr RoomCatalogEntry ROOM_CATALOG[] = {
    {
        .Document =
        {
            .Value = foundation::identity::Uuid{{0x47,
                                                 0xd7,
                                                 0x5e,
                                                 0x49,
                                                 0xfc,
                                                 0xdf,
                                                 0x44,
                                                 0xeb,
                                                 0x9e,
                                                 0x4e,
                                                 0xdc,
                                                 0xac,
                                                 0x13,
                                                 0xfb,
                                                 0x7b,
                                                 0xce}},
        },
        .LevelId = "first-room",
        .Room = 1,
    },
    {
        .Document =
        {
            .Value = foundation::identity::Uuid{{0x5f,
                                                 0x6f,
                                                 0xb8,
                                                 0x20,
                                                 0x71,
                                                 0xb9,
                                                 0x4f,
                                                 0xa9,
                                                 0xba,
                                                 0x40,
                                                 0xca,
                                                 0xe3,
                                                 0x10,
                                                 0xcb,
                                                 0x5f,
                                                 0xc9}},
        },
        .LevelId = "second-room",
        .Room = 2,
    },
};
/// Number of catalog entries.
inline constexpr foundation::usize ROOM_CATALOG_COUNT = sizeof(ROOM_CATALOG) / sizeof(ROOM_CATALOG[0]);
static_assert(CheckRoomCatalog(ROOM_CATALOG, ROOM_CATALOG_COUNT).IsValid == true,
              "world demo room ordinals and documents must be nonzero and unique");
/// @brief Room ordinal of a level from the checked-in catalog, or zero when it has none.
[[nodiscard]] constexpr foundation::uint16 FindCatalogRoom(foundation::identity::AssetId document,
                                                           std::string_view levelId) noexcept
{
    return FindRoom(ROOM_CATALOG, ROOM_CATALOG_COUNT, document, levelId);
}
} // namespace ludus::world_demo
