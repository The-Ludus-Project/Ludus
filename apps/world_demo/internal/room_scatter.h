#pragma once
// A World-demo room's scatter recipe, built once from its authored level for both the
// game (GameWorld::Prepare) and the Editor's staged generation session, so the two
// generate from identical recipe bytes
// (docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter).
#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/environment/scatter.h>
#include <ludus/gameplay/world/entity_id.h>

#include "level.h"
#include "random_domains.h"

namespace ludus::world_demo
{
/// The scatter domains of the World demo's random domain manifest.
inline constexpr content::procedural::ScatterDomains WORLD_SCATTER_DOMAINS
{
    .Placement = RANDOM_DOMAIN_SCATTER_PLACEMENT,
    .Variant = RANDOM_DOMAIN_SCATTER_VARIANT,
    .Transform = RANDOM_DOMAIN_SCATTER_TRANSFORM,
};

/// @brief A room's kernel recipe and the arrays it borrows.
///
/// Recipe's views point into this object's own arrays, so it is neither copyable nor movable; keep it where it was
/// built for as long as the recipe is used. The layers' Locks are empty.
struct RoomScatterRecipe final
{
    RoomScatterRecipe() = default;
    RoomScatterRecipe(const RoomScatterRecipe&) = delete;
    RoomScatterRecipe& operator=(const RoomScatterRecipe&) = delete;
    RoomScatterRecipe(RoomScatterRecipe&&) = delete;
    RoomScatterRecipe& operator=(RoomScatterRecipe&&) = delete;

    /// Every spawn point and exit in entity ID order, then every collision box, each grown by the keep-out margin.
    content::procedural::ScatterRectangle KeepOuts[content::procedural::MAX_SCATTER_KEEP_OUTS] = {};
    /// Each layer's variants, in authored order.
    content::procedural::ScatterVariant Variants[MAX_ROOM_SCATTER_LAYERS][MAX_ROOM_SCATTER_VARIANTS] = {};
    /// The authored layers, in authored order.
    content::procedural::ScatterLayer Layers[MAX_ROOM_SCATTER_LAYERS] = {};
    /// The room's recipe; its Room is the level's catalog ordinal.
    content::procedural::ScatterRecipe Recipe;
};

/// @brief Build a room's validated scatter recipe from its level.
/// @param level A level whose HasScatter is true.
/// @param outRecipe Rebuilt in place; its contents are unspecified on failure.
/// @return Success; or InvalidConfiguration when the level has no scatter, its room is not in the room catalog, a
///         coordinate leaves the scatter lattice, or the kernel rejects the recipe.
[[nodiscard]] gameplay::world::Status TryBuildRoomScatterRecipe(const Level& level,
                                                                RoomScatterRecipe& outRecipe) noexcept;
} // namespace ludus::world_demo
