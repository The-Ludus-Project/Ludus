#include "playable_snapshot.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/playable_world.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::first_person
{
namespace
{
using namespace ludus::foundation;
using parsing::JsonValue;
namespace format = content::scene_format;

bool MatchesDigest(std::string_view encoded, const content::Digest& digest) noexcept
{
    constexpr std::string_view DIGITS = "0123456789abcdef";
    constexpr uint8 NIBBLE_BITS = 4;
    constexpr uint8 LOW_NIBBLE = 0x0F;
    if (encoded.size() != sizeof(digest.Data) * 2)
    {
        return false;
    }
    for (usize i = 0; i < sizeof(digest.Data); ++i)
    {
        if (encoded[i * 2] != DIGITS[digest.Data[i] >> NIBBLE_BITS] ||
            encoded[i * 2 + 1] != DIGITS[digest.Data[i] & LOW_NIBBLE])
        {
            return false;
        }
    }
    return true;
}

bool IsDigestText(JsonValue value) noexcept
{
    constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
    std::string_view text;
    return value.String(text) == true && text.size() == sizeof(content::Digest::Data) * 2 &&
           text.find_first_not_of(HEX_DIGITS) == std::string_view::npos;
}

bool HasValidArtifacts(JsonValue artifacts) noexcept
{
    if (artifacts.Array() == false || artifacts.Count() > format::MAX_CLOSURE_RESOURCES)
    {
        return false;
    }
    // The producer writes the catalog's unique ASCII resource IDs in sorted order.
    std::string_view previous;
    for (usize i = 0; i < artifacts.Count(); ++i)
    {
        const JsonValue record = artifacts.At(i);
        std::string_view id;
        if (record.Get("id").String(id) == false || id <= previous || id.size() > content::MAX_RESOURCE_ID_BYTES ||
            IsDigestText(record.Get("key")) == false)
        {
            return false;
        }
        previous = id;
    }
    return true;
}

bool ReadSourceId(JsonValue value, uint64& outId) noexcept
{
    std::string_view text;
    return value.String(text) == true && ParseUint64(text.data(), text.size(), outId) == NumberParseStatus::Success;
}

bool ReadScalar(JsonValue value, float32& outValue) noexcept
{
    float64 number = 0;
    return value.Number(number) == true && math::TryConvertToFloat32(number, outValue) == true;
}

bool ReadVector(JsonValue value, math::Vector3& outValue) noexcept
{
    constexpr usize COMPONENTS = 3;
    return value.Count() == COMPONENTS && ReadScalar(value.At(0), outValue.X) == true &&
           ReadScalar(value.At(1), outValue.Y) == true && ReadScalar(value.At(2), outValue.Z) == true;
}

bool ReadAtmosphere(JsonValue sky, graphics::renderer::AtmosphereDescription& outSky) noexcept
{
    graphics::renderer::CelestialBackgroundDescription& celestial = outSky.Celestial;
    const JsonValue planets = sky.Get("planets");
    if (planets.Count() != celestial.Planets.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < planets.Count(); ++i)
    {
        if (ReadVector(planets.At(i).Get("directionWorld"), celestial.Planets[i].DirectionWorld) == false ||
            ReadVector(planets.At(i).Get("illuminance"), celestial.Planets[i].Illuminance) == false)
        {
            return false;
        }
    }
    return sky.Get("enabled").Boolean(outSky.IsEnabled) == true &&
           ReadVector(sky.Get("anchorWorld"), outSky.SurfaceAnchorWorld) == true &&
           ReadVector(sky.Get("upWorld"), outSky.SurfaceUpWorld) == true &&
           ReadVector(sky.Get("toSunWorld"), outSky.DirectionToSunWorld) == true &&
           ReadVector(sky.Get("solarIlluminance"), outSky.SolarIlluminance) == true &&
           ReadScalar(sky.Get("solarRadiusRadians"), outSky.SolarAngularRadiusRadians) == true &&
           ReadSourceId(sky.Get("sunSourceId"), outSky.SunSourceId) == true &&
           sky.Get("lightScene").Boolean(outSky.ShouldLightScene) == true &&
           sky.Get("celestialEnabled").Boolean(celestial.IsEnabled) == true &&
           ReadScalar(sky.Get("starRadiance"), celestial.StarRadiance) == true &&
           ReadVector(sky.Get("toMoonWorld"), celestial.DirectionToMoonWorld) == true &&
           ReadVector(sky.Get("lunarPeakRadiance"), celestial.LunarPeakRadiance) == true &&
           ReadScalar(sky.Get("lunarRadiusRadians"), celestial.LunarAngularRadiusRadians) == true &&
           ReadScalar(sky.Get("milkyWayRadiance"), celestial.MilkyWayRadiance) == true &&
           ReadVector(sky.Get("galacticNorthWorld"), celestial.GalacticNorthWorld) == true &&
           ReadVector(sky.Get("galacticCenterWorld"), celestial.GalacticCenterWorld) == true &&
           ReadVector(sky.Get("toSunAtMoonWorld"), celestial.DirectionToSunAtMoonWorld) == true;
}

bool ReadSpawn(JsonValue value, std::string_view key, PlayerSpawn& outSpawn) noexcept
{
    constexpr usize VECTOR_COMPONENTS = 3;
    constexpr usize QUATERNION_COMPONENTS = 4;
    const JsonValue eye = value.Get("eyeWorld");
    const JsonValue rotation = value.Get("orientation");
    PlayerSpawn spawn;
    float64 speed = 0;
    StaticArray<float64, QUATERNION_COMPONENTS> coordinates{};
    if (eye.Count() != VECTOR_COMPONENTS || rotation.Count() != QUATERNION_COMPONENTS ||
        eye.At(0).Number(spawn.Pose.Position.X) == false || eye.At(1).Number(spawn.Pose.Position.Y) == false ||
        eye.At(2).Number(spawn.Pose.Position.Z) == false || math::IsFinite(spawn.Pose.Position) == false ||
        value.Get("speed").Number(speed) == false || math::TryConvertToFloat32(speed, spawn.Speed) == false ||
        spawn.Speed <= 0)
    {
        return false;
    }
    for (usize i = 0; i < QUATERNION_COMPONENTS; ++i)
    {
        if (rotation.At(i).Number(coordinates[i]) == false || math::Abs(coordinates[i]) > 1)
        {
            return false;
        }
    }
    const math::Quaternion orientation{static_cast<float32>(coordinates[0]),
                                       static_cast<float32>(coordinates[1]),
                                       static_cast<float32>(coordinates[2]),
                                       static_cast<float32>(coordinates[QUATERNION_COMPONENTS - 1])};
    if (math::TryNormalize(orientation, spawn.Pose.Orientation) != math::MathStatus::Success)
    {
        return false;
    }
    spawn.Entity = key;
    outSpawn = spawn;
    return true;
}

bool ReadBindings(JsonValue root, PlayableSnapshot& inoutSnapshot) noexcept
{
    const JsonValue entities = root.Get("entities");
    const JsonValue objects = root.Get("objects");
    if (entities.Array() == false || entities.Count() > static_cast<usize>(format::MAX_WORLD_ENTITIES) ||
        objects.Array() == false || objects.Count() != inoutSnapshot.Scene.Objects.GetSize())
    {
        return false;
    }
    SortedMap<std::string_view, bool> entitiesById;
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const JsonValue entity = entities.At(i);
        std::string_view key;
        if (entity.Get("id").String(key) == false || key.empty() == true)
        {
            return false;
        }
        if (entitiesById.TryAddInPlace(key, true).Inserted == false)
        {
            return false;
        }
        const JsonValue rasterShadows = entity.Get("light").Get("rasterShadows");
        if (rasterShadows.Valid() == true)
        {
            bool shouldCastRasterShadows = true;
            uint64 sourceId = 0;
            if (rasterShadows.Boolean(shouldCastRasterShadows) == false ||
                ReadSourceId(entity.Get("lightSourceId"), sourceId) == false ||
                (shouldCastRasterShadows == false && inoutSnapshot.RasterUnshadowedLightIds.TryAdd(sourceId) == false))
            {
                return false;
            }
        }
        const JsonValue value = entity.Get("spawn");
        if (value.Valid() == true)
        {
            PlayerSpawn spawn;
            if (ReadSpawn(value, key, spawn) == false || inoutSnapshot.Spawns.TryAdd(spawn) == false)
            {
                return false;
            }
        }
    }
    for (usize i = 0; i < objects.Count(); ++i)
    {
        std::string_view key;
        bool isCollider = false;
        uint64 sourceId = 0;
        if (ReadSourceId(objects.At(i).Get("sourceId"), sourceId) == false ||
            sourceId != inoutSnapshot.Scene.Objects[i].SourceId || objects.At(i).Get("entity").String(key) == false ||
            objects.At(i).Get("staticCollider").Boolean(isCollider) == false)
        {
            return false;
        }
        if (entitiesById.Contains(key) == false ||
            (isCollider == true && inoutSnapshot.ColliderObjects.TryAdd(i) == false))
        {
            return false;
        }
    }
    return inoutSnapshot.Spawns.GetSize() != 0;
}
} // namespace
bool DecodePlayableSnapshot(BorrowedView<const uint8> bytes, PlayableSnapshot& outSnapshot) noexcept
{
    if (bytes.IsValid() == false || bytes.IsEmpty() == true || bytes.GetSize() > MAX_PLAYABLE_SNAPSHOT_BYTES)
    {
        return false;
    }
    const std::string_view encoded(reinterpret_cast<const char*>(bytes.GetDataOrNull()), bytes.GetSize());
    const usize end = encoded.substr(0, format::MAX_PLAYABLE_MANIFEST_BYTES + 1).find('\n');
    if (end == std::string_view::npos)
    {
        return false;
    }
    PlayableSnapshot candidate;
    parsing::ParseError error;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = format::MAX_PLAYABLE_MANIFEST_BYTES;
    constexpr usize MANIFEST_WORKSPACE_BYTES = usize{128} * 1024 * 1024;
    limits.MaxWorkspaceBytes = MANIFEST_WORKSPACE_BYTES;
    limits.MaxObjectMembers = parsing::MAX_JSON_OBJECT_MEMBERS;
    if (candidate.Manifest.Read(encoded.substr(0, end), error, limits) != parsing::ParseStatus::Ok)
    {
        return false;
    }
    const JsonValue root = candidate.Manifest.Root();
    if (root.Fields({"version",
                     "renderer",
                     "world",
                     "source",
                     "worldSha256",
                     "sceneSha256",
                     "entities",
                     "objects",
                     "artifacts",
                     "warnings",
                     "earthObserver",
                     "atmosphere"}) == false)
    {
        return false;
    }
    std::string_view source;
    if (root.Get("source").String(source) == false || source.empty() == true ||
        IsDigestText(root.Get("worldSha256")) == false || HasValidArtifacts(root.Get("artifacts")) == false ||
        root.Get("warnings").Array() == false || root.Get("earthObserver").Object() == false)
    {
        return false;
    }
    uint64 version = 0;
    std::string_view renderer;
    std::string_view world;
    std::string_view digest;
    const std::span<const uint8> bundle(bytes.GetDataOrNull() + end + 1, bytes.GetSize() - end - 1);
    if (root.Get("version").Integer(version) == false || version != format::PLAYABLE_WORLD_VERSION ||
        root.Get("renderer").String(renderer) == false || renderer != format::PLAYABLE_RASTER_RENDERER ||
        root.Get("world").String(world) == false || world.empty() == true ||
        root.Get("sceneSha256").String(digest) == false || bundle.size() > format::MAX_ASSEMBLED_BUNDLE_BYTES ||
        MatchesDigest(digest, content::Hash(bundle)) == false ||
        (bundle.empty() == false &&
         resources::SceneDefinition::Decode(bundle.data(), bundle.size(), candidate.Scene) != resources::Status::Ok) ||
        ReadAtmosphere(root.Get("atmosphere"), candidate.Atmosphere) == false || ReadBindings(root, candidate) == false)
    {
        return false;
    }
    outSnapshot = Move(candidate);
    return true;
}
} // namespace ludus::first_person
