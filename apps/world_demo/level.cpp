#include "internal/level.h"
#include "levels.h"
#include <charconv>
#include <cmath>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <span>
namespace ludus::world_demo
{
namespace
{
/// Fastest movement speed a level may give an actor, in world units per second.
constexpr float32 MAX_ACTOR_SPEED = 32.0F;
/// Largest starting health a level may give an actor.
constexpr uint32 MAX_ACTOR_HEALTH = 100000;
/// Initial capacity of the canonical level text buffers, in bytes.
constexpr usize LEVEL_TEXT_CAPACITY_BYTES = 65536;
class Reader final
{
public:
    explicit Reader(std::string_view source) noexcept : mSource(source) {}
    [[nodiscard]] LevelResult Result() const noexcept
    {
        return {mError, mPosition};
    }
    bool Fail(LevelError error) noexcept
    {
        if (mError == LevelError::None)
        {
            mError = error;
        }
        return false;
    }
    void Space() noexcept
    {
        while (mPosition < mSource.size() && (mSource[mPosition] == ' ' || mSource[mPosition] == '\n' ||
                                              mSource[mPosition] == '\r' || mSource[mPosition] == '\t'))
        {
            ++mPosition;
        }
    }
    bool Take(char ch) noexcept
    {
        Space();
        if (mPosition < mSource.size() && mSource[mPosition] == ch)
        {
            ++mPosition;
            return true;
        }
        return false;
    }
    bool Require(char ch) noexcept
    {
        return Take(ch) || Fail(LevelError::Syntax);
    }
    bool End() noexcept
    {
        Space();
        return mPosition == mSource.size() || Fail(LevelError::Syntax);
    }
    template <usize N>
    bool Text(char (&output)[N]) noexcept
    {
        if (Require('"') == false)
        {
            return false;
        }
        usize size = 0;
        while (mPosition < mSource.size())
        {
            char ch = mSource[mPosition++];
            if (ch == '"')
            {
                output[size] = 0;
                return true;
            }
            if (static_cast<uint8>(ch) < 32 || static_cast<uint8>(ch) >= 128)
            {
                return Fail(LevelError::InvalidValue);
            }
            if (ch == '\\')
            {
                if (mPosition == mSource.size())
                {
                    return Fail(LevelError::Syntax);
                }
                ch = mSource[mPosition++];
                switch (ch)
                {
                    case '"':
                    case '\\':
                    case '/':
                        break;
                    case 'b':
                        ch = '\b';
                        break;
                    case 'f':
                        ch = '\f';
                        break;
                    case 'n':
                        ch = '\n';
                        break;
                    case 'r':
                        ch = '\r';
                        break;
                    case 't':
                        ch = '\t';
                        break;
                    case 'u': {
                        uint32 code = 0;
                        for (usize digit = 0; digit < 4; ++digit)
                        {
                            if (mPosition == mSource.size())
                            {
                                return Fail(LevelError::Syntax);
                            }
                            const char hex = mSource[mPosition++];
                            uint32 value = 0;
                            if (hex >= '0' && hex <= '9')
                            {
                                value = static_cast<uint32>(hex - '0');
                            }
                            else if (hex >= 'a' && hex <= 'f')
                            {
                                value = static_cast<uint32>(hex - 'a' + 10);
                            }
                            else if (hex >= 'A' && hex <= 'F')
                            {
                                value = static_cast<uint32>(hex - 'A' + 10);
                            }
                            else
                            {
                                return Fail(LevelError::Syntax);
                            }
                            code = code * 16 + value;
                        }
                        if (code == 0 || code > 127)
                        {
                            return Fail(LevelError::InvalidValue);
                        }
                        ch = static_cast<char>(code);
                        break;
                    }
                    default:
                        return Fail(LevelError::Syntax);
                }
            }
            if (size + 1 >= N)
            {
                return Fail(LevelError::Capacity);
            }
            output[size++] = ch;
        }
        return Fail(LevelError::Syntax);
    }
    bool Number(float32& value) noexcept
    {
        Space();
        const usize start = mPosition;
        (void)Take('-');
        if (mPosition == mSource.size())
        {
            return Fail(LevelError::Syntax);
        }
        if (mSource[mPosition] == '0')
        {
            ++mPosition;
        }
        else
        {
            if (mSource[mPosition] < '1' || mSource[mPosition] > '9')
            {
                return Fail(LevelError::Syntax);
            }
            Digits();
        }
        if (mPosition < mSource.size() && mSource[mPosition] == '.')
        {
            ++mPosition;
            if (Digits() == false)
            {
                return Fail(LevelError::Syntax);
            }
        }
        if (mPosition < mSource.size() && (mSource[mPosition] == 'e' || mSource[mPosition] == 'E'))
        {
            ++mPosition;
            if (mPosition < mSource.size() && (mSource[mPosition] == '+' || mSource[mPosition] == '-'))
            {
                ++mPosition;
            }
            if (Digits() == false)
            {
                return Fail(LevelError::Syntax);
            }
        }
        const auto parsed = ParseFloat32(mSource.data() + start, mPosition - start, value);
        return (parsed == NumberParseStatus::Success && std::isfinite(value) && std::abs(value) <= 100000.0F) ||
               Fail(LevelError::InvalidValue);
    }
    bool Integer(uint64& value) noexcept
    {
        Space();
        const usize start = mPosition;
        if (Digits() == false || (mPosition - start > 1 && mSource[start] == '0'))
        {
            return Fail(LevelError::Syntax);
        }
        const auto parsed = ParseUint64(mSource.data() + start, mPosition - start, value);
        return parsed == NumberParseStatus::Success || Fail(LevelError::InvalidValue);
    }
    bool Pair(Vec2& value) noexcept
    {
        return Require('[') && Number(value.X) && Require(',') && Number(value.Y) && Require(']');
    }
    template <typename Field>
    bool Object(Field field, uint32 required) noexcept
    {
        if (Require('{') == false)
        {
            return false;
        }
        uint32 seen = 0;
        if (Take('}') == false)
        {
            do
            {
                char key[32] = {};
                if (Text(key) == false || Require(':') == false || field(std::string_view(key), seen) == false)
                {
                    return false;
                }
                if (Take('}'))
                {
                    return (seen & required) == required || Fail(LevelError::MissingField);
                }
            } while (Require(','));
            return false;
        }
        return required == 0 || Fail(LevelError::MissingField);
    }
    bool Field(uint32& seen, uint32 bit) noexcept
    {
        if ((seen & bit) != 0)
        {
            return Fail(LevelError::DuplicateField);
        }
        seen |= bit;
        return true;
    }
    template <typename T, usize N, typename Decode>
    bool List(T (&items)[N], usize& count, Decode decode) noexcept
    {
        if (Require('[') == false)
        {
            return false;
        }
        count = 0;
        if (Take(']'))
        {
            return true;
        }
        do
        {
            if (count == N)
            {
                return Fail(LevelError::Capacity);
            }
            if (decode(items[count++]) == false)
            {
                return false;
            }
            if (Take(']'))
            {
                return true;
            }
        } while (Require(','));
        return false;
    }

private:
    bool Digits() noexcept
    {
        const usize start = mPosition;
        while (mPosition < mSource.size() && mSource[mPosition] >= '0' && mSource[mPosition] <= '9')
        {
            ++mPosition;
        }
        return start != mPosition;
    }
    std::string_view mSource;
    usize mPosition = 0;
    LevelError mError = LevelError::None;
};
bool ReadRecipe(Reader& reader, Recipe& recipe) noexcept
{
    uint32 properties = 0;
    const bool valid = reader.Object(
        [&](std::string_view key, uint32& seen) {
            if (key == "id")
            {
                return reader.Field(seen, 1) && reader.Text(recipe.Id.Text);
            }
            if (key == "kind")
            {
                char kind[16] = {};
                if (reader.Field(seen, 2) == false || reader.Text(kind) == false)
                {
                    return false;
                }
                const std::string_view type(kind);
                if (type == "player")
                {
                    recipe.Type = Kind::Player;
                }
                else if (type == "enemy")
                {
                    recipe.Type = Kind::Enemy;
                }
                else if (type == "exit")
                {
                    recipe.Type = Kind::Exit;
                }
                else
                {
                    return reader.Fail(LevelError::InvalidValue);
                }
                return true;
            }
            if (key == "transform")
            {
                return reader.Field(seen, 4) &&
                       reader.Object(
                           [&](std::string_view name, uint32& fields) {
                               if (name == "position")
                               {
                                   return reader.Field(fields, 1) && reader.Pair(recipe.Position);
                               }
                               if (name == "rotation")
                               {
                                   return reader.Field(fields, 2) && reader.Number(recipe.Rotation);
                               }
                               if (name == "scale")
                               {
                                   return reader.Field(fields, 4) && reader.Pair(recipe.Scale);
                               }
                               return reader.Fail(LevelError::UnknownField);
                           },
                           7);
            }
            if (key == "properties")
            {
                return reader.Field(seen, 8) &&
                       reader.Object(
                           [&](std::string_view name, uint32& fields) {
                               bool result = false;
                               if (name == "speed")
                               {
                                   result = reader.Field(fields, 1) && reader.Number(recipe.Speed);
                               }
                               else if (name == "health")
                               {
                                   uint64 health = 0;
                                   result = reader.Field(fields, 2) && reader.Integer(health);
                                   if (health > 100000)
                                   {
                                       return reader.Fail(LevelError::InvalidValue);
                                   }
                                   recipe.Health = static_cast<uint32>(health);
                               }
                               else if (name == "sprite")
                               {
                                   result = reader.Field(fields, 4) && reader.Text(recipe.Sprite.Text);
                               }
                               else if (name == "target")
                               {
                                   result = reader.Field(fields, 8) && reader.Text(recipe.Target.Text);
                               }
                               else if (name == "half_extent")
                               {
                                   result = reader.Field(fields, 16) && reader.Pair(recipe.HalfExtent);
                               }
                               else if (name == "next_level")
                               {
                                   result = reader.Field(fields, 32) && reader.Text(recipe.NextLevel.Text);
                               }
                               else
                               {
                                   return reader.Fail(LevelError::UnknownField);
                               }
                               properties = fields;
                               return result;
                           },
                           0);
            }
            return reader.Fail(LevelError::UnknownField);
        },
        15);
    const uint32 required = recipe.Type == Kind::Player ? 7U : recipe.Type == Kind::Enemy ? 15U : 48U;
    return valid && (properties == required || reader.Fail(LevelError::InvalidValue));
}
bool Bounded(float32 value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= 100000.0F;
}
bool Bounded(Vec2 value) noexcept
{
    return Bounded(value.X) && Bounded(value.Y);
}
bool Positive(Vec2 value) noexcept
{
    return value.X > 0 && value.Y > 0;
}
bool Identifier(const Name& name) noexcept
{
    const auto text = name.View();
    if (text.empty() || text.size() >= sizeof(name.Text))
    {
        return false;
    }
    for (const char ch : text)
    {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-')
        {
            return false;
        }
    }
    return true;
}
} // namespace
LevelResult ReadLevel(std::string_view source, Level& output) noexcept
{
    if (source.size() > 65536)
    {
        return {LevelError::Capacity, 0};
    }
    if (source.find("\"schema\"") != std::string_view::npos)
    {
        return ReadWorldLevel(source, output);
    }
    Reader reader(source);
    Level candidate;
    const bool parsed = reader.Object(
        [&](std::string_view key, uint32& seen) {
            if (key == "version")
            {
                uint64 version = 0;
                return reader.Field(seen, 1) && reader.Integer(version) &&
                       (version == 1 || reader.Fail(LevelError::InvalidValue));
            }
            if (key == "id")
            {
                return reader.Field(seen, 2) && reader.Text(candidate.Id.Text);
            }
            if (key == "seed")
            {
                return reader.Field(seen, 4) && reader.Integer(candidate.Seed);
            }
            if (key == "settings")
            {
                return reader.Field(seen, 8) &&
                       reader.Object(
                           [&](std::string_view name, uint32& fields) {
                               if (name == "bounds")
                               {
                                   return reader.Field(fields, 1) &&
                                          reader.Object(
                                              [&](std::string_view bound, uint32& bits) {
                                                  if (bound == "min")
                                                  {
                                                      return reader.Field(bits, 1) && reader.Pair(candidate.Minimum);
                                                  }
                                                  if (bound == "max")
                                                  {
                                                      return reader.Field(bits, 2) && reader.Pair(candidate.Maximum);
                                                  }
                                                  return reader.Fail(LevelError::UnknownField);
                                              },
                                              3);
                               }
                               if (name == "camera")
                               {
                                   return reader.Field(fields, 2) &&
                                          reader.Object(
                                              [&](std::string_view camera, uint32& bits) {
                                                  if (camera == "center")
                                                  {
                                                      return reader.Field(bits, 1) && reader.Pair(candidate.Camera);
                                                  }
                                                  if (camera == "vertical_extent")
                                                  {
                                                      return reader.Field(bits, 2) &&
                                                             reader.Number(candidate.VerticalExtent);
                                                  }
                                                  return reader.Fail(LevelError::UnknownField);
                                              },
                                              3);
                               }
                               return reader.Fail(LevelError::UnknownField);
                           },
                           3);
            }
            if (key == "collision")
            {
                return reader.Field(seen, 16) &&
                       reader.List(candidate.Collision, candidate.CollisionCount, [&](Box& box) {
                           return reader.Object(
                               [&](std::string_view name, uint32& fields) {
                                   if (name == "id")
                                   {
                                       return reader.Field(fields, 1) && reader.Text(box.Id.Text);
                                   }
                                   if (name == "shape")
                                   {
                                       char shape[8] = {};
                                       return reader.Field(fields, 2) && reader.Text(shape) &&
                                              (std::string_view(shape) == "box" ||
                                               reader.Fail(LevelError::InvalidValue));
                                   }
                                   if (name == "center")
                                   {
                                       return reader.Field(fields, 4) && reader.Pair(box.Center);
                                   }
                                   if (name == "half_extent")
                                   {
                                       return reader.Field(fields, 8) && reader.Pair(box.HalfExtent);
                                   }
                                   return reader.Fail(LevelError::UnknownField);
                               },
                               15);
                       });
            }
            if (key == "entities")
            {
                return reader.Field(seen, 32) &&
                       reader.List(candidate.Entities, candidate.EntityCount, [&](Recipe& recipe) {
                           return ReadRecipe(reader, recipe);
                       });
            }
            return reader.Fail(LevelError::UnknownField);
        },
        63);
    if (parsed == false || reader.End() == false)
    {
        return reader.Result();
    }
    const auto error = ValidateLevel(candidate);
    if (error != LevelError::None)
    {
        return {error, reader.Result().Offset};
    }
    output = candidate;
    return {};
}
namespace
{
/// Whether an authored scatter name repeats an entity, collision box, the scatter entity, one of the first
/// `fullLayers` layers or their variants, or the first `partialVariants` variants (and the ID) of `partialOrNull`.
bool IsScatterNameTaken(const Level& level,
                        const Name& name,
                        usize fullLayers,
                        const ScatterLayerRecipe* partialOrNull,
                        usize partialVariants) noexcept
{
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        if (level.Entities[index].Id == name)
        {
            return true;
        }
    }
    for (usize index = 0; index < level.CollisionCount; ++index)
    {
        if (level.Collision[index].Id == name)
        {
            return true;
        }
    }
    if (level.Scatter.Id == name)
    {
        return true;
    }
    for (usize layer = 0; layer < fullLayers; ++layer)
    {
        const ScatterLayerRecipe& recipe = level.Scatter.Layers[layer];
        if (recipe.Id == name)
        {
            return true;
        }
        for (usize variant = 0; variant < recipe.VariantCount; ++variant)
        {
            if (recipe.Variants[variant].Id == name)
            {
                return true;
            }
        }
    }
    if (partialOrNull == nullptr)
    {
        return false;
    }
    if (partialOrNull->Id == name)
    {
        return true;
    }
    for (usize variant = 0; variant < partialVariants; ++variant)
    {
        if (partialOrNull->Variants[variant].Id == name)
        {
            return true;
        }
    }
    return false;
}
/// Conservative candidate count of one layer over the lattice region, or false when the lattice is unusable.
bool TryCountLayerCandidates(const RoomScatter& scatter, const ScatterLayerRecipe& layer, uint64& outCount) noexcept
{
    int32 minimumX = 0;
    int32 minimumY = 0;
    int32 maximumX = 0;
    int32 maximumY = 0;
    if (TryScaleToLattice(scatter.Minimum.X, scatter.UnitsPerMetre, minimumX) == false ||
        TryScaleToLattice(scatter.Minimum.Y, scatter.UnitsPerMetre, minimumY) == false ||
        TryScaleToLattice(scatter.Maximum.X, scatter.UnitsPerMetre, maximumX) == false ||
        TryScaleToLattice(scatter.Maximum.Y, scatter.UnitsPerMetre, maximumY) == false || minimumX >= maximumX ||
        minimumY >= maximumY)
    {
        return false;
    }
    // Every cell overlapping the region, plus one per axis for the partial cells at each edge.
    const uint64 cellsX = static_cast<uint64>(maximumX - minimumX) / layer.CellSize + 2;
    const uint64 cellsY = static_cast<uint64>(maximumY - minimumY) / layer.CellSize + 2;
    outCount = cellsX * cellsY * layer.Candidates;
    return true;
}
LevelError ValidateScatter(const Level& level) noexcept
{
    const RoomScatter& scatter = level.Scatter;
    if (level.HasScatter == false)
    {
        return scatter.LayerCount == 0 ? LevelError::None : LevelError::InvalidValue;
    }
    if (scatter.LayerCount > MAX_ROOM_SCATTER_LAYERS)
    {
        return LevelError::Capacity;
    }
    if (Identifier(scatter.Id) == false || scatter.LayerCount == 0 || scatter.UnitsPerMetre == 0 ||
        scatter.UnitsPerMetre > MAX_SCATTER_UNITS_PER_METRE || Bounded(scatter.Minimum) == false ||
        Bounded(scatter.Maximum) == false || Bounded(scatter.KeepOutMargin) == false || scatter.KeepOutMargin < 0 ||
        scatter.KeepOutMargin > MAX_SCATTER_KEEP_OUT_MARGIN || scatter.Minimum.X >= scatter.Maximum.X ||
        scatter.Minimum.Y >= scatter.Maximum.Y || scatter.Minimum.X < level.Minimum.X ||
        scatter.Minimum.Y < level.Minimum.Y || scatter.Maximum.X > level.Maximum.X ||
        scatter.Maximum.Y > level.Maximum.Y)
    {
        return LevelError::InvalidValue;
    }
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        if (level.Entities[index].Id == scatter.Id)
        {
            return LevelError::DuplicateField;
        }
    }
    for (usize index = 0; index < level.CollisionCount; ++index)
    {
        if (level.Collision[index].Id == scatter.Id)
        {
            return LevelError::DuplicateField;
        }
    }
    uint64 candidates = 0;
    for (usize index = 0; index < scatter.LayerCount; ++index)
    {
        const ScatterLayerRecipe& layer = scatter.Layers[index];
        if (layer.VariantCount > MAX_ROOM_SCATTER_VARIANTS)
        {
            return LevelError::Capacity;
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (scatter.Layers[prior].Layer == layer.Layer)
            {
                return LevelError::DuplicateField;
            }
        }
        if (Identifier(layer.Id) == false || layer.CellSize == 0 || layer.Candidates == 0 ||
            layer.Candidates > MAX_ROOM_SCATTER_CANDIDATES_PER_CELL || layer.MinimumScale == 0 ||
            layer.MaximumScale < layer.MinimumScale || layer.VariantCount == 0)
        {
            return LevelError::InvalidValue;
        }
        if (IsScatterNameTaken(level, layer.Id, index, nullptr, 0) == true)
        {
            return LevelError::DuplicateField;
        }
        for (usize variant = 0; variant < layer.VariantCount; ++variant)
        {
            const ScatterVariantRecipe& recipe = layer.Variants[variant];
            if (Identifier(recipe.Id) == false || recipe.ExclusionRadius > layer.CellSize ||
                Bounded(recipe.HalfExtent) == false || Positive(recipe.HalfExtent) == false ||
                recipe.HalfExtent.X > level.Maximum.X - level.Minimum.X ||
                recipe.HalfExtent.Y > level.Maximum.Y - level.Minimum.Y)
            {
                return LevelError::InvalidValue;
            }
            for (const float32 channel : recipe.Color)
            {
                if (Bounded(channel) == false || channel < 0 || channel > 1)
                {
                    return LevelError::InvalidValue;
                }
            }
            if (IsScatterNameTaken(level, recipe.Id, index, &layer, variant) == true)
            {
                return LevelError::DuplicateField;
            }
        }
        uint64 layerCandidates = 0;
        if (TryCountLayerCandidates(scatter, layer, layerCandidates) == false)
        {
            return LevelError::InvalidValue;
        }
        candidates += layerCandidates;
        if (candidates > MAX_ROOM_SCATTER_CANDIDATES)
        {
            return LevelError::Capacity;
        }
    }
    return LevelError::None;
}
} // namespace
LevelError ValidateLevel(const Level& level) noexcept
{
    if (level.EntityCount > 32 || level.CollisionCount > 16)
    {
        return LevelError::Capacity;
    }
    if (Bounded(level.Minimum) == false || Bounded(level.Maximum) == false || Bounded(level.Camera) == false ||
        Bounded(level.VerticalExtent) == false || Identifier(level.Id) == false ||
        Positive({level.Maximum.X - level.Minimum.X, level.Maximum.Y - level.Minimum.Y}) == false ||
        level.VerticalExtent <= 0)
    {
        return LevelError::InvalidValue;
    }
    usize players = 0;
    for (usize index = 0; index < level.CollisionCount; ++index)
    {
        const auto& box = level.Collision[index];
        if (Bounded(box.Center) == false || Bounded(box.HalfExtent) == false || Identifier(box.Id) == false ||
            Positive(box.HalfExtent) == false)
        {
            return LevelError::InvalidValue;
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (level.Collision[prior].Id == box.Id)
            {
                return LevelError::DuplicateField;
            }
        }
    }
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        const auto& recipe = level.Entities[index];
        if ((recipe.Type != Kind::Player && recipe.Type != Kind::Enemy && recipe.Type != Kind::Exit) ||
            Bounded(recipe.Position) == false || Bounded(recipe.Rotation) == false || Bounded(recipe.Scale) == false ||
            Bounded(recipe.HalfExtent) == false || Bounded(recipe.Speed) == false ||
            recipe.HalfExtent.X * recipe.Scale.X * 2 >= level.Maximum.X - level.Minimum.X ||
            recipe.HalfExtent.Y * recipe.Scale.Y * 2 >= level.Maximum.Y - level.Minimum.Y ||
            Identifier(recipe.Id) == false || Positive(recipe.Scale) == false || Positive(recipe.HalfExtent) == false ||
            recipe.Position.X < level.Minimum.X || recipe.Position.X > level.Maximum.X ||
            recipe.Position.Y < level.Minimum.Y || recipe.Position.Y > level.Maximum.Y)
        {
            return LevelError::InvalidValue;
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (level.Entities[prior].Id == recipe.Id)
            {
                return LevelError::DuplicateField;
            }
        }
        for (usize box = 0; box < level.CollisionCount; ++box)
        {
            if (level.Collision[box].Id == recipe.Id)
            {
                return LevelError::DuplicateField;
            }
        }
        if (recipe.Type == Kind::Exit)
        {
            if (recipe.NextLevel.View() != "first-room" && recipe.NextLevel.View() != "second-room")
            {
                return LevelError::InvalidReference;
            }
        }
        else
        {
            if ((recipe.Speed > 0) == false || recipe.Speed > MAX_ACTOR_SPEED || recipe.Health == 0 ||
                recipe.Health > MAX_ACTOR_HEALTH)
            {
                return LevelError::InvalidValue;
            }
            if (recipe.Sprite.View() != "player" && recipe.Sprite.View() != "guard")
            {
                return LevelError::MissingAsset;
            }
            if (recipe.Type == Kind::Player)
            {
                ++players;
            }
            else
            {
                bool targetFound = false;
                for (usize other = 0; other < level.EntityCount; ++other)
                {
                    targetFound |=
                        level.Entities[other].Id == recipe.Target && level.Entities[other].Type == Kind::Player;
                }
                if (targetFound == false)
                {
                    return LevelError::InvalidReference;
                }
            }
        }
    }
    if (players != 1)
    {
        return LevelError::InvalidValue;
    }
    return ValidateScatter(level);
}
bool TryScaleToLattice(float32 metres, uint32 unitsPerMetre, int32& outLattice) noexcept
{
    if (Bounded(metres) == false || unitsPerMetre == 0 || unitsPerMetre > MAX_SCATTER_UNITS_PER_METRE)
    {
        return false;
    }
    const float64 scaled = static_cast<float64>(metres) * static_cast<float64>(unitsPerMetre);
    const float64 magnitude = foundation::math::Floor(std::abs(scaled) + 0.5);
    if (magnitude > static_cast<float64>(MAX_SCATTER_LATTICE_MAGNITUDE))
    {
        return false;
    }
    const int32 rounded = static_cast<int32>(magnitude);
    outLattice = scaled < 0 ? -rounded : rounded;
    return true;
}
namespace
{
class Writer final
{
public:
    core::Array<char> Data;
    bool Valid = true;
    void Text(std::string_view text) noexcept
    {
        Valid = Valid && Data.TryAddRange(std::span<const char>(text.data(), text.size()));
    }
    void NameValue(const Name& name) noexcept
    {
        Text("\"");
        Text(name.View());
        Text("\"");
    }
    template <typename T>
    void Number(T value) noexcept
    {
        char buffer[64] = {};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{})
        {
            Valid = false;
            return;
        }
        Text({buffer, static_cast<usize>(result.ptr - buffer)});
    }
    void Pair(Vec2 value) noexcept
    {
        Text("[");
        Number(value.X);
        Text(",");
        Number(value.Y);
        Text("]");
    }
};
} // namespace
bool WriteLevel(const Level& level, core::Array<char>& output) noexcept
{
    if (ValidateLevel(level) != LevelError::None)
    {
        return false;
    }
    Writer writer;
    if (writer.Data.TryEnsureCapacity(LEVEL_TEXT_CAPACITY_BYTES) == false)
    {
        return false;
    }
    writer.Text("{\"version\":1,\"id\":");
    writer.NameValue(level.Id);
    writer.Text(",\"seed\":");
    writer.Number(level.Seed);
    writer.Text(",\"settings\":{\"bounds\":{\"min\":");
    writer.Pair(level.Minimum);
    writer.Text(",\"max\":");
    writer.Pair(level.Maximum);
    writer.Text("},\"camera\":{\"center\":");
    writer.Pair(level.Camera);
    writer.Text(",\"vertical_extent\":");
    writer.Number(level.VerticalExtent);
    writer.Text("}},\"collision\":[");
    for (usize index = 0; index < level.CollisionCount; ++index)
    {
        if (index != 0)
        {
            writer.Text(",");
        }
        const auto& box = level.Collision[index];
        writer.Text("{\"id\":");
        writer.NameValue(box.Id);
        writer.Text(",\"shape\":\"box\",\"center\":");
        writer.Pair(box.Center);
        writer.Text(",\"half_extent\":");
        writer.Pair(box.HalfExtent);
        writer.Text("}");
    }
    writer.Text("],\"entities\":[");
    usize order[32] = {};
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        order[index] = index;
    }
    for (usize index = 1; index < level.EntityCount; ++index)
    {
        const auto value = order[index];
        usize position = index;
        while (position > 0 && level.Entities[value].Id.View() < level.Entities[order[position - 1]].Id.View())
        {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = value;
    }
    for (usize index = 0; index < level.EntityCount; ++index)
    {
        if (index != 0)
        {
            writer.Text(",");
        }
        const auto& recipe = level.Entities[order[index]];
        writer.Text("{\"id\":");
        writer.NameValue(recipe.Id);
        writer.Text(",\"kind\":\"");
        writer.Text(recipe.Type == Kind::Player ? "player" : recipe.Type == Kind::Enemy ? "enemy" : "exit");
        writer.Text("\",\"transform\":{\"position\":");
        writer.Pair(recipe.Position);
        writer.Text(",\"rotation\":");
        writer.Number(recipe.Rotation);
        writer.Text(",\"scale\":");
        writer.Pair(recipe.Scale);
        writer.Text("},\"properties\":{");
        if (recipe.Type == Kind::Exit)
        {
            writer.Text("\"half_extent\":");
            writer.Pair(recipe.HalfExtent);
            writer.Text(",\"next_level\":");
            writer.NameValue(recipe.NextLevel);
        }
        else
        {
            writer.Text("\"speed\":");
            writer.Number(recipe.Speed);
            writer.Text(",\"health\":");
            writer.Number(recipe.Health);
            writer.Text(",\"sprite\":");
            writer.NameValue(recipe.Sprite);
            if (recipe.Type == Kind::Enemy)
            {
                writer.Text(",\"target\":");
                writer.NameValue(recipe.Target);
            }
        }
        writer.Text("}}");
    }
    writer.Text("]}");
    if (writer.Valid == false)
    {
        return false;
    }
    // Formatting is cold-path work. Names are validated ASCII identifiers, so
    // the canonical compact stream has no escaped quotes to interpret here.
    core::Array<char> formatted;
    if (formatted.TryEnsureCapacity(LEVEL_TEXT_CAPACITY_BYTES) == false)
    {
        return false;
    }
    bool valid = true;
    bool quoted = false;
    usize depth = 0;
    const auto append = [&](char value) { valid = valid && formatted.TryAdd(value); };
    const auto line = [&]() {
        append('\n');
        for (usize space = 0; space < depth * 2; ++space)
        {
            append(' ');
        }
    };
    for (usize index = 0; index < writer.Data.GetSize(); ++index)
    {
        const char value = writer.Data[index];
        if (value == '\"')
        {
            quoted = quoted == false;
        }
        if (quoted == false && (value == '}' || value == ']'))
        {
            --depth;
            if (writer.Data[index - 1] != '{' && writer.Data[index - 1] != '[')
            {
                line();
            }
        }
        append(value);
        if (quoted == false && (value == '{' || value == '['))
        {
            ++depth;
            if (writer.Data[index + 1] != '}' && writer.Data[index + 1] != ']')
            {
                line();
            }
        }
        else if (quoted == false && value == ',')
        {
            line();
        }
        else if (quoted == false && value == ':')
        {
            append(' ');
        }
    }
    append('\n');
    if (valid == false)
    {
        return false;
    }
    output = Move(formatted);
    return true;
}
std::string_view ExampleLevel() noexcept
{
    return BuiltinLevel("first-room");
}
std::string_view BuiltinLevel(std::string_view id) noexcept
{
    if (id == "first-room")
    {
        return content::FirstRoom;
    }
    if (id == "second-room")
    {
        return content::SecondRoom;
    }
    return {};
}
} // namespace ludus::world_demo
