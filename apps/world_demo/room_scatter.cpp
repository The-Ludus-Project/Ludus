#include "internal/room_scatter.h"
#include "internal/room_catalog.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::world_demo
{
namespace
{
namespace procedural = content::procedural;
using foundation::BorrowedView;
using gameplay::world::Status;
/// Most authored entities of a level.
constexpr usize MAX_LEVEL_ENTITIES = sizeof(Level::Entities) / sizeof(Recipe);
static_assert(MAX_LEVEL_ENTITIES + sizeof(Level::Collision) / sizeof(Box) <= procedural::MAX_SCATTER_KEEP_OUTS);
/// Half-open lattice rectangle around a box, expanded by the keep-out margin; never empty.
bool TryMakeKeepOut(Vec2 center,
                    Vec2 half,
                    float32 margin,
                    uint32 units,
                    procedural::ScatterRectangle& outRectangle) noexcept
{
    procedural::ScatterRectangle rectangle;
    if (TryScaleToLattice(center.X - half.X - margin, units, rectangle.MinimumX) == false ||
        TryScaleToLattice(center.Y - half.Y - margin, units, rectangle.MinimumY) == false ||
        TryScaleToLattice(center.X + half.X + margin, units, rectangle.MaximumX) == false ||
        TryScaleToLattice(center.Y + half.Y + margin, units, rectangle.MaximumY) == false)
    {
        return false;
    }
    rectangle.MaximumX = foundation::math::Max(rectangle.MaximumX, rectangle.MinimumX + 1);
    rectangle.MaximumY = foundation::math::Max(rectangle.MaximumY, rectangle.MinimumY + 1);
    outRectangle = rectangle;
    return true;
}
} // namespace
Status TryBuildRoomScatterRecipe(const Level& level, RoomScatterRecipe& outRecipe) noexcept
{
    if (level.HasScatter == false)
    {
        return Status::InvalidConfiguration;
    }
    const RoomScatter& scatter = level.Scatter;
    procedural::ScatterRecipe& recipe = outRecipe.Recipe;
    recipe = {};
    recipe.Room = FindCatalogRoom(level.Document, level.Id.View());
    if (recipe.Room == 0 || level.EntityCount > MAX_LEVEL_ENTITIES || scatter.LayerCount > MAX_ROOM_SCATTER_LAYERS)
    {
        return Status::InvalidConfiguration;
    }
    if (TryScaleToLattice(scatter.Minimum.X, scatter.UnitsPerMetre, recipe.Region.MinimumX) == false ||
        TryScaleToLattice(scatter.Minimum.Y, scatter.UnitsPerMetre, recipe.Region.MinimumY) == false ||
        TryScaleToLattice(scatter.Maximum.X, scatter.UnitsPerMetre, recipe.Region.MaximumX) == false ||
        TryScaleToLattice(scatter.Maximum.Y, scatter.UnitsPerMetre, recipe.Region.MaximumY) == false)
    {
        return Status::InvalidConfiguration;
    }
    // Keep-outs: every authored spawn point and exit in entity ID order, then every collision box, each with the
    // margin. The ID order makes the keep-out indices, and so the stage bytes, independent of the file's order.
    usize order[MAX_LEVEL_ENTITIES] = {};
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        order[index] = index;
    }
    foundation::Sort(order, level.EntityCount, [&level](usize first, usize second) {
        return level.Entities[first].Id.View() < level.Entities[second].Id.View();
    });
    usize keepOutCount = 0;
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        const Recipe& entity = level.Entities[order[index]];
        const Vec2 half{entity.HalfExtent.X * entity.Scale.X, entity.HalfExtent.Y * entity.Scale.Y};
        if (TryMakeKeepOut(entity.Position,
                           half,
                           scatter.KeepOutMargin,
                           scatter.UnitsPerMetre,
                           outRecipe.KeepOuts[keepOutCount]) == false)
        {
            return Status::InvalidConfiguration;
        }
        ++keepOutCount;
    }
    for (usize index = 0; index < level.CollisionCount; ++index)
    {
        const Box& box = level.Collision[index];
        if (TryMakeKeepOut(box.Center,
                           box.HalfExtent,
                           scatter.KeepOutMargin,
                           scatter.UnitsPerMetre,
                           outRecipe.KeepOuts[keepOutCount]) == false)
        {
            return Status::InvalidConfiguration;
        }
        ++keepOutCount;
    }
    recipe.KeepOuts = BorrowedView<const procedural::ScatterRectangle>(outRecipe.KeepOuts, keepOutCount);
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        const ScatterLayerRecipe& authored = scatter.Layers[index];
        if (authored.VariantCount > MAX_ROOM_SCATTER_VARIANTS)
        {
            return Status::InvalidConfiguration;
        }
        for (usize variant = 0; variant < authored.VariantCount; ++variant)
        {
            outRecipe.Variants[index][variant].ExclusionRadius = authored.Variants[variant].ExclusionRadius;
        }
        outRecipe.Layers[index] =
        {
            .Id = authored.Layer,
            .CellSize = authored.CellSize,
            .CandidatesPerCell = authored.Candidates,
            .Variants =
                BorrowedView<const procedural::ScatterVariant>(outRecipe.Variants[index], authored.VariantCount),
            .MinimumScale = authored.MinimumScale,
            .MaximumScale = authored.MaximumScale,
            .Locks = {},
        };
    }
    recipe.Layers = BorrowedView<const procedural::ScatterLayer>(outRecipe.Layers, scatter.LayerCount);
    if (procedural::ValidateScatterRecipe(recipe).Status != procedural::ScatterStatus::Success)
    {
        return Status::InvalidConfiguration;
    }
    return Status::Success;
}
} // namespace ludus::world_demo
