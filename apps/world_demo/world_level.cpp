#include "internal/level.h"

#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/json.hpp>

namespace ludus::world_demo
{
namespace
{
using foundation::identity::Uuid;
using foundation::parsing::JsonValue;
/// Envelope version whose entities carry only scoped string IDs.
constexpr uint64 WORLD_VERSION_SCOPED_IDS = 1;
/// Envelope version with a document UUID, per-entity UUIDs and labels, and UUID entity references
/// (docs/architecture/object-handles-uuid.md#stage-3-uuid-authoring-records).
constexpr uint64 WORLD_VERSION_UUID_RECORDS = 2;
/// Most authoring entities in one World: actors, collision boxes and the settings entity.
constexpr usize MAX_WORLD_ENTITIES = 49;
bool IdentityValue(JsonValue value, Uuid& output) noexcept
{
    std::string_view text;
    return value.String(text) == true &&
           foundation::identity::TryParseUuid(text.data(),
                                              text.size(),
                                              foundation::identity::UuidPolicy::RequireVersion4,
                                              output) == foundation::identity::UuidStatus::Success;
}
bool NameValue(JsonValue value, Name& output, bool empty = false) noexcept
{
    std::string_view text;
    if (value.String(text) == false || text.size() >= sizeof(output.Text) || (empty == false && text.empty()))
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const auto ch = text[i];
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-')
        {
            return false;
        }
        output.Text[i] = ch;
    }
    return true;
}
bool Number(JsonValue value, float32& output) noexcept
{
    float64 number = 0;
    if (value.Number(number) == false || foundation::math::IsFinite(number) == false || number < -100000 ||
        number > 100000)
    {
        return false;
    }
    output = static_cast<float32>(number);
    return true;
}
bool Transform(JsonValue value, Recipe& recipe) noexcept
{
    return value.Fields({"x", "y", "rotation", "scale_x", "scale_y"}) && Number(value.Get("x"), recipe.Position.X) &&
           Number(value.Get("y"), recipe.Position.Y) && Number(value.Get("rotation"), recipe.Rotation) &&
           Number(value.Get("scale_x"), recipe.Scale.X) && Number(value.Get("scale_y"), recipe.Scale.Y);
}
// A version-2 enemy names its target by UUID in `outTarget`; version 1 names it by ID in `recipe`.
bool Actor(JsonValue value, Recipe& recipe, bool enemy, bool hasUuidReferences, Uuid& outTarget) noexcept
{
    uint64 health = 0;
    const JsonValue target = value.Get("target");
    const bool isTargetRead = enemy == false || (hasUuidReferences == true ? IdentityValue(target, outTarget)
                                                                           : NameValue(target, recipe.Target));
    if ((enemy == true ? value.Fields({"speed", "health", "sprite", "target"})
                       : value.Fields({"speed", "health", "sprite"})) == false ||
        Number(value.Get("speed"), recipe.Speed) == false || value.Get("health").Integer(health) == false ||
        health > 100000 || NameValue(value.Get("sprite"), recipe.Sprite) == false || isTargetRead == false)
    {
        return false;
    }
    recipe.Type = enemy ? Kind::Enemy : Kind::Player;
    recipe.Health = static_cast<uint32>(health);
    return true;
}
/// Read an unsigned integer member within [minimum, maximum].
bool BoundedInteger(JsonValue value, uint64 minimum, uint64 maximum, uint64& output) noexcept
{
    uint64 number = 0;
    if (value.Integer(number) == false || number < minimum || number > maximum)
    {
        return false;
    }
    output = number;
    return true;
}
/// The room-level `scatter` component: lattice conversion, region and keep-out margin.
bool Scatter(JsonValue value, RoomScatter& scatter) noexcept
{
    uint64 units = 0;
    if (value.Fields({"units_per_metre", "min_x", "min_y", "max_x", "max_y", "keep_out_margin"}) == false ||
        BoundedInteger(value.Get("units_per_metre"), 1, MAX_SCATTER_UNITS_PER_METRE, units) == false ||
        Number(value.Get("min_x"), scatter.Minimum.X) == false ||
        Number(value.Get("min_y"), scatter.Minimum.Y) == false ||
        Number(value.Get("max_x"), scatter.Maximum.X) == false ||
        Number(value.Get("max_y"), scatter.Maximum.Y) == false ||
        Number(value.Get("keep_out_margin"), scatter.KeepOutMargin) == false)
    {
        return false;
    }
    scatter.UnitsPerMetre = static_cast<uint32>(units);
    return true;
}
/// A `scatter_layer` component: stable ID, cells, candidates and scale range.
bool ScatterLayer(JsonValue value, ScatterLayerRecipe& layer) noexcept
{
    uint64 id = 0;
    uint64 cellSize = 0;
    uint64 candidates = 0;
    uint64 minimumScale = 0;
    uint64 maximumScale = 0;
    if (value.Fields({"layer", "cell_size", "candidates", "min_scale", "max_scale"}) == false ||
        BoundedInteger(value.Get("layer"), 0, 255, id) == false ||
        BoundedInteger(value.Get("cell_size"), 1, 65535, cellSize) == false ||
        BoundedInteger(value.Get("candidates"), 1, MAX_ROOM_SCATTER_CANDIDATES_PER_CELL, candidates) == false ||
        BoundedInteger(value.Get("min_scale"), 1, 65535, minimumScale) == false ||
        BoundedInteger(value.Get("max_scale"), 1, 65535, maximumScale) == false)
    {
        return false;
    }
    layer.Layer = static_cast<uint8>(id);
    layer.CellSize = static_cast<uint16>(cellSize);
    layer.Candidates = static_cast<uint16>(candidates);
    layer.MinimumScale = static_cast<uint16>(minimumScale);
    layer.MaximumScale = static_cast<uint16>(maximumScale);
    return true;
}
/// A `scatter_variant` component; `outLayer` is the stable ID of the layer it joins.
bool ScatterVariant(JsonValue value, ScatterVariantRecipe& variant, uint8& outLayer) noexcept
{
    uint64 layer = 0;
    uint64 radius = 0;
    if (value.Fields({"layer", "exclusion_radius", "half_x", "half_y", "red", "green", "blue"}) == false ||
        BoundedInteger(value.Get("layer"), 0, 255, layer) == false ||
        BoundedInteger(value.Get("exclusion_radius"), 0, 65535, radius) == false ||
        Number(value.Get("half_x"), variant.HalfExtent.X) == false ||
        Number(value.Get("half_y"), variant.HalfExtent.Y) == false ||
        Number(value.Get("red"), variant.Color[0]) == false || Number(value.Get("green"), variant.Color[1]) == false ||
        Number(value.Get("blue"), variant.Color[2]) == false)
    {
        return false;
    }
    variant.ExclusionRadius = static_cast<uint32>(radius);
    outLayer = static_cast<uint8>(layer);
    return true;
}
/// A variant read before every layer is known; attached to its layer after the entity loop.
struct PendingVariant final
{
    ScatterVariantRecipe Variant{};
    uint8 Layer = 0;
};
/// Most variants one World may author across its layers.
constexpr usize MAX_WORLD_SCATTER_VARIANTS = MAX_ROOM_SCATTER_LAYERS * MAX_ROOM_SCATTER_VARIANTS;
bool Settings(JsonValue value, Level& level) noexcept
{
    std::string_view seed;
    return value.Fields(
               {"id", "seed", "min_x", "min_y", "max_x", "max_y", "camera_x", "camera_y", "vertical_extent"}) &&
           NameValue(value.Get("id"), level.Id) && value.Get("seed").String(seed) &&
           ParseUint64(seed.data(), seed.size(), level.Seed) == NumberParseStatus::Success &&
           Number(value.Get("min_x"), level.Minimum.X) && Number(value.Get("min_y"), level.Minimum.Y) &&
           Number(value.Get("max_x"), level.Maximum.X) && Number(value.Get("max_y"), level.Maximum.Y) &&
           Number(value.Get("camera_x"), level.Camera.X) && Number(value.Get("camera_y"), level.Camera.Y) &&
           Number(value.Get("vertical_extent"), level.VerticalExtent);
}
} // namespace
LevelResult ReadWorldLevel(std::string_view source, Level& output) noexcept
{
    namespace parsing = foundation::parsing;
    parsing::JsonDocument document;
    parsing::ParseError error;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = 65536;
    limits.MaxArrayElements = MAX_WORLD_ENTITIES;
    limits.MaxValues = 4096;
    limits.MaxStringBytes = 128;
    if (source.size() > limits.MaxInputBytes)
    {
        return {LevelError::Capacity, 0};
    }
    if (document.Read(source, error, limits) != parsing::ParseStatus::Ok)
    {
        return {LevelError::Syntax, 0};
    }
    const auto root = document.Root();
    uint64 version = 0;
    std::string_view schema;
    const JsonValue entities = root.Get("entities");
    if (root.Get("version").Integer(version) == false ||
        (version != WORLD_VERSION_SCOPED_IDS && version != WORLD_VERSION_UUID_RECORDS))
    {
        return {LevelError::InvalidValue, 0};
    }
    // Version 2 adds a document UUID; each entity's version-1 `id` became its `label`, and `uuid` is its identity.
    const bool hasUuidRecords = version == WORLD_VERSION_UUID_RECORDS;
    const bool hasRootFields = hasUuidRecords == true ? root.Fields({"version", "schema", "document", "entities"})
                                                      : root.Fields({"version", "schema", "entities"});
    if (hasRootFields == false || root.Get("schema").String(schema) == false || schema != "ludus.world-demo.v1" ||
        entities.Array() == false)
    {
        return {LevelError::InvalidValue, 0};
    }
    Level candidate;
    if (hasUuidRecords == true && IdentityValue(root.Get("document"), candidate.Document.Value) == false)
    {
        return {LevelError::InvalidValue, 0};
    }
    bool settingsSeen = false;
    foundation::StaticArray<Name, MAX_WORLD_ENTITIES> ids{};
    foundation::StaticArray<Uuid, MAX_WORLD_ENTITIES> objects{};
    // Version-2 enemy targets, resolved to labels after every entity's UUID is known.
    foundation::StaticArray<Uuid, MAX_WORLD_ENTITIES> targets{};
    foundation::StaticArray<PendingVariant, MAX_WORLD_SCATTER_VARIANTS> variants{};
    usize variantCount = 0;
    const char* const idKey = hasUuidRecords == true ? "label" : "id";
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const JsonValue entity = entities.At(i);
        std::string_view displayName;
        const JsonValue components = entity.Get("components");
        const bool hasEntityFields = hasUuidRecords == true ? entity.Fields({"uuid", "label", "name", "components"})
                                                            : entity.Fields({"id", "name", "components"});
        if (hasEntityFields == false || NameValue(entity.Get(idKey), ids[i]) == false ||
            entity.Get("name").String(displayName) == false || displayName.empty() == true ||
            components.Object() == false ||
            (hasUuidRecords == true && IdentityValue(entity.Get("uuid"), objects[i]) == false))
        {
            return {LevelError::InvalidValue, 0};
        }
        for (usize prior = 0; prior < i; ++prior)
        {
            if (ids[i] == ids[prior] || (hasUuidRecords == true && objects[i] == objects[prior]))
            {
                return {LevelError::DuplicateField, 0};
            }
        }
        if (components.Get("settings").Valid())
        {
            if (settingsSeen == true || components.Fields({"settings"}) == false ||
                Settings(components.Get("settings"), candidate) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
            candidate.SettingsObject.Value = objects[i];
            settingsSeen = true;
        }
        else if (components.Get("collision").Valid())
        {
            if (candidate.CollisionCount == 16)
            {
                return {LevelError::Capacity, 0};
            }
            auto& box = candidate.Collision[candidate.CollisionCount++];
            const auto collision = components.Get("collision");
            box.Id = ids[i];
            box.Object.Value = objects[i];
            if (components.Fields({"collision"}) == false ||
                collision.Fields({"x", "y", "half_x", "half_y"}) == false ||
                Number(collision.Get("x"), box.Center.X) == false ||
                Number(collision.Get("y"), box.Center.Y) == false ||
                Number(collision.Get("half_x"), box.HalfExtent.X) == false ||
                Number(collision.Get("half_y"), box.HalfExtent.Y) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
        }
        else if (components.Get("scatter").Valid())
        {
            if (candidate.HasScatter == true)
            {
                return {LevelError::DuplicateField, 0};
            }
            if (components.Fields({"scatter"}) == false ||
                Scatter(components.Get("scatter"), candidate.Scatter) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
            candidate.HasScatter = true;
            candidate.Scatter.Id = ids[i];
            candidate.Scatter.Object.Value = objects[i];
        }
        else if (components.Get("scatter_layer").Valid())
        {
            if (candidate.Scatter.LayerCount == MAX_ROOM_SCATTER_LAYERS)
            {
                return {LevelError::Capacity, 0};
            }
            ScatterLayerRecipe& layer = candidate.Scatter.Layers[candidate.Scatter.LayerCount++];
            layer.Id = ids[i];
            layer.Object.Value = objects[i];
            if (components.Fields({"scatter_layer"}) == false ||
                ScatterLayer(components.Get("scatter_layer"), layer) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
        }
        else if (components.Get("scatter_variant").Valid())
        {
            if (variantCount == MAX_WORLD_SCATTER_VARIANTS)
            {
                return {LevelError::Capacity, 0};
            }
            PendingVariant& pending = variants[variantCount++];
            pending.Variant.Id = ids[i];
            pending.Variant.Object.Value = objects[i];
            if (components.Fields({"scatter_variant"}) == false ||
                ScatterVariant(components.Get("scatter_variant"), pending.Variant, pending.Layer) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
        }
        else
        {
            if (candidate.EntityCount == 32)
            {
                return {LevelError::Capacity, 0};
            }
            Uuid& target = targets[candidate.EntityCount];
            auto& recipe = candidate.Entities[candidate.EntityCount++];
            recipe.Id = ids[i];
            recipe.Object.Value = objects[i];
            if (Transform(components.Get("transform"), recipe) == false)
            {
                return {LevelError::InvalidValue, 0};
            }
            if (components.Get("exit").Valid())
            {
                const auto exit = components.Get("exit");
                recipe.Type = Kind::Exit;
                if (components.Fields({"transform", "exit"}) == false ||
                    exit.Fields({"half_x", "half_y", "next_level"}) == false ||
                    Number(exit.Get("half_x"), recipe.HalfExtent.X) == false ||
                    Number(exit.Get("half_y"), recipe.HalfExtent.Y) == false ||
                    NameValue(exit.Get("next_level"), recipe.NextLevel) == false)
                {
                    return {LevelError::InvalidValue, 0};
                }
            }
            else
            {
                const bool enemy = components.Get("enemy").Valid();
                if ((enemy == true ? components.Fields({"transform", "enemy"})
                                   : components.Fields({"transform", "player"})) == false ||
                    Actor(components.Get(enemy == true ? "enemy" : "player"), recipe, enemy, hasUuidRecords, target) ==
                        false)
                {
                    return {LevelError::InvalidValue, 0};
                }
            }
        }
    }
    if (settingsSeen == false)
    {
        return {LevelError::MissingField, 0};
    }
    // Variants join their layer by stable ID in document order; a layer-less variant is an invalid reference.
    for (usize i = 0; i < variantCount; ++i)
    {
        ScatterLayerRecipe* layerOrNull = nullptr;
        for (usize layer = 0; layer < candidate.Scatter.LayerCount && layerOrNull == nullptr; ++layer)
        {
            if (candidate.Scatter.Layers[layer].Layer == variants[i].Layer)
            {
                layerOrNull = &candidate.Scatter.Layers[layer];
            }
        }
        if (layerOrNull == nullptr)
        {
            return {LevelError::InvalidReference, 0};
        }
        if (layerOrNull->VariantCount == MAX_ROOM_SCATTER_VARIANTS)
        {
            return {LevelError::Capacity, 0};
        }
        layerOrNull->Variants[layerOrNull->VariantCount++] = variants[i].Variant;
    }
    // Version 2 references are UUIDs of entities in the same document; resolve each to its label so
    // runtime lookup, validation and replay hashing are identical for both versions.
    for (usize i = 0; i < candidate.EntityCount && hasUuidRecords == true; ++i)
    {
        Recipe& recipe = candidate.Entities[i];
        if (recipe.Type != Kind::Enemy)
        {
            continue;
        }
        bool isResolved = false;
        for (usize j = 0; j < entities.Count() && isResolved == false; ++j)
        {
            if (objects[j] == targets[i])
            {
                recipe.Target = ids[j];
                isResolved = true;
            }
        }
        if (isResolved == false)
        {
            return {LevelError::InvalidReference, 0};
        }
    }
    const auto valid = ValidateLevel(candidate);
    if (valid != LevelError::None)
    {
        return {valid, 0};
    }
    output = candidate;
    return {};
}
} // namespace ludus::world_demo
