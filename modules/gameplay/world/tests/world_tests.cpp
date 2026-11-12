#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/identity/lifetime.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/world/celestial_transform.hpp>
#include <ludus/gameplay/world/component_pool.hpp>
#include <ludus/gameplay/world/entity_registry.h>
#include <utility>
using namespace ludus::gameplay::world;
using namespace ludus::foundation;
namespace ludus::gameplay::world::internal
{
struct RegistryTestAccess final
{
    static EntityId RetireNext(EntityRegistry& registry) noexcept
    {
        const auto slot = registry.mFree.GetLast();
        registry.mSlots[slot].Generation = identity::TERMINAL_GENERATION;
        return {registry.mWorld, slot, identity::TERMINAL_GENERATION};
    }
    static void ExhaustSequence(EntityRegistry& registry) noexcept
    {
        registry.mNextSequence = std::numeric_limits<uint64>::max();
    }
};
} // namespace ludus::gameplay::world::internal
TEST_CASE("Registry rejects recycled and foreign handles, including after a move", "[world]")
{
    EntityRegistry first;
    EntityRegistry second;
    REQUIRE(first.TryInitialize(2) == Status::Success);
    REQUIRE(second.TryInitialize(2) == Status::Success);
    EntityId old;
    REQUIRE(first.TryCreate(old) == Status::Success);
    REQUIRE_FALSE(second.IsAlive(old));
    REQUIRE(first.GetCreationSequence(old) == 1);
    REQUIRE(first.MarkPendingDestroy(old) == Status::Success);
    REQUIRE(first.IsAlive(old));
    REQUIRE_FALSE(first.IsActive(old));
    REQUIRE(first.TryRelease(old) == Status::Success);
    EntityId replacement;
    REQUIRE(first.TryCreate(replacement) == Status::Success);
    REQUIRE(replacement.Slot == old.Slot);
    REQUIRE(replacement.Generation != old.Generation);
    REQUIRE_FALSE(first.IsAlive(old));
    EntityRegistry moved(std::move(first));
    REQUIRE(moved.IsAlive(replacement));
    // EntityRegistry explicitly leaves a moved-from source without identity.
    // NOLINTNEXTLINE(bugprone-use-after-move): verify the documented source state.
    REQUIRE_FALSE(first.IsAlive(replacement));
}
TEST_CASE("A moved-from registry can initialize a fresh structural world", "[world]")
{
    EntityRegistry source;
    REQUIRE(source.TryInitialize(2) == Status::Success);
    EntityId previous;
    REQUIRE(source.TryCreate(previous) == Status::Success);
    source.SetStructuralPhase(false);
    EntityRegistry destination(std::move(source));
    // NOLINTNEXTLINE(bugprone-use-after-move): initialization reuses the empty source.
    REQUIRE(source.TryInitialize(2) == Status::Success);
    EntityId fresh;
    REQUIRE(source.TryCreate(fresh) == Status::Success);
    REQUIRE(source.GetCreationSequence(fresh) == 1);
    REQUIRE(source.GetStructuralRevision() == 1);
    REQUIRE_FALSE(source.IsAlive(previous));
    REQUIRE(destination.IsAlive(previous));
}
TEST_CASE("Generation retirement and sequence exhaustion never reuse identity", "[world]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(1) == Status::Success);
    const auto retired = internal::RegistryTestAccess::RetireNext(registry);
    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    REQUIRE(entity == retired);
    REQUIRE(registry.TryRelease(entity) == Status::Success);
    REQUIRE(registry.TryCreate(entity) == Status::CapacityExceeded);
    REQUIRE(entity == retired);
    REQUIRE_FALSE(registry.IsAlive(retired));
    EntityRegistry exhausted;
    REQUIRE(exhausted.TryInitialize(1) == Status::Success);
    internal::RegistryTestAccess::ExhaustSequence(exhausted);
    REQUIRE(exhausted.TryCreate(entity) == Status::IdentityExhausted);
    REQUIRE(entity == retired);
}
TEST_CASE("Registries take world tokens from the shared process lifetime source", "[world]")
{
    uint64 before = 0;
    REQUIRE(identity::TryIssueLifetimeId(before) == identity::LifetimeIdStatus::Success);
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(1) == Status::Success);
    uint64 after = 0;
    REQUIRE(identity::TryIssueLifetimeId(after) == identity::LifetimeIdStatus::Success);
    // Nothing else issues tokens in this process, so the steps are exact. This also
    // proves the registry and this test link one copy of the counter.
    REQUIRE(registry.GetWorld() == before + 1);
    REQUIRE(after == registry.GetWorld() + 1);

    // Rejected initialization neither consumes a token nor replaces the current one.
    EntityRegistry rejected;
    REQUIRE(rejected.TryInitialize(0) == Status::InvalidConfiguration);
    REQUIRE(rejected.GetWorld() == 0);
    const uint64 world = registry.GetWorld();
    REQUIRE(registry.TryInitialize(1) == Status::InvalidConfiguration);
    REQUIRE(registry.GetWorld() == world);
    uint64 next = 0;
    REQUIRE(identity::TryIssueLifetimeId(next) == identity::LifetimeIdStatus::Success);
    REQUIRE(next == after + 1);
}
TEST_CASE("Handles from an old world are rejected after the world is rebuilt", "[world]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(2) == Status::Success);
    ComponentPool<uint32> oldPool;
    REQUIRE(oldPool.TryInitialize(registry) == Status::Success);
    EntityId old;
    REQUIRE(registry.TryCreate(old) == Status::Success);
    REQUIRE(oldPool.TryInsert(registry, old, 7) == Status::Success);
    const uint64 oldWorld = registry.GetWorld();

    // Clearing and rebuilding takes a fresh lifetime; it never resets generations
    // under the old world token.
    registry = EntityRegistry{};
    REQUIRE(registry.TryInitialize(2) == Status::Success);
    ComponentPool<uint32> pool;
    REQUIRE(pool.TryInitialize(registry) == Status::Success);
    EntityId fresh;
    REQUIRE(registry.TryCreate(fresh) == Status::Success);
    REQUIRE(pool.TryInsert(registry, fresh, 11) == Status::Success);

    // Same slot, generation and creation sequence: only the world token differs.
    REQUIRE(fresh.Slot == old.Slot);
    REQUIRE(fresh.Generation == old.Generation);
    REQUIRE(registry.GetCreationSequence(fresh) == 1);
    REQUIRE(fresh.World != oldWorld);
    REQUIRE(fresh.World == registry.GetWorld());

    REQUIRE_FALSE(registry.IsAlive(old));
    REQUIRE_FALSE(registry.IsActive(old));
    REQUIRE(registry.GetCreationSequence(old) == 0);
    REQUIRE(registry.MarkPendingDestroy(old) == Status::InvalidEntity);
    REQUIRE(registry.TryRelease(old) == Status::InvalidEntity);
    REQUIRE(pool.Find(old) == nullptr);
    REQUIRE(pool.TryInsert(registry, old, 9) == Status::InvalidEntity);
    REQUIRE(pool.TryRemove(registry, old) == Status::InvalidEntity);

    // A pool bound to the old world rejects the new world and keeps its old value.
    REQUIRE(oldPool.Find(fresh) == nullptr);
    REQUIRE(oldPool.TryInsert(registry, fresh, 3) == Status::InvalidEntity);
    REQUIRE(oldPool.TryRemove(registry, fresh) == Status::InvalidEntity);

    // None of the rejected calls disturbed the live incarnation in the same slot.
    REQUIRE(registry.IsActive(fresh));
    REQUIRE(*pool.Find(fresh) == 11);
    REQUIRE(registry.GetStructuralRevision() == 1);
}
TEST_CASE("Sparse pool swap removal preserves exact owners and enforces phases", "[world]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(3) == Status::Success);
    ComponentPool<uint32> pool;
    REQUIRE(pool.TryInitialize(registry) == Status::Success);
    EntityId entities[3];
    for (usize index = 0; index < 3; ++index)
    {
        REQUIRE(registry.TryCreate(entities[index]) == Status::Success);
        REQUIRE(pool.TryInsert(registry, entities[index], static_cast<uint32>(index + 10)) == Status::Success);
    }
    REQUIRE(pool.TryInsert(registry, entities[1], 99) == Status::DuplicateComponent);
    REQUIRE(pool.TryRemove(registry, entities[1]) == Status::Success);
    REQUIRE(*pool.Find(entities[2]) == 12);
    REQUIRE(pool.GetOwners()[1] == entities[2]);
    REQUIRE(pool.Find(entities[1]) == nullptr);
    REQUIRE(registry.TryRelease(entities[1]) == Status::Success);
    EntityId fresh;
    REQUIRE(registry.TryCreate(fresh) == Status::Success);
    REQUIRE(pool.Find(fresh) == nullptr);
    REQUIRE(pool.TryInsert(registry, fresh, 22) == Status::Success);
    REQUIRE(pool.Find(entities[1]) == nullptr);
    registry.SetStructuralPhase(false);
    REQUIRE(pool.TryRemove(registry, fresh) == Status::InvalidPhase);
    REQUIRE(registry.TryCreate(fresh) == Status::InvalidPhase);
    REQUIRE(pool.GetOwners().GetSize() == pool.GetValues().GetSize());
}

TEST_CASE("Pool moves transfer ownership and reject insertion into the empty source", "[world]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(2) == Status::Success);
    ComponentPool<uint32> source;
    REQUIRE(source.TryInitialize(registry) == Status::Success);
    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    REQUIRE(source.TryInsert(registry, entity, 4) == Status::Success);
    ComponentPool<uint32> moved(std::move(source));
    REQUIRE(*moved.Find(entity) == 4);
    // NOLINTNEXTLINE(bugprone-use-after-move): moved-from pools reject mutations.
    REQUIRE(source.TryInsert(registry, entity, 5) == Status::InvalidEntity);
}

TEST_CASE("Earth sky rules provide time-dependent rotations and horizon state", "[world][celestial]")
{
    constexpr math::Vector3 LIGHT_FORWARD{0, 0, -1};
    EarthCelestialTransform sun;
    REQUIRE(TrySampleEarthCelestialTransform(EarthCelestialBody::Sun, 0, 0, 0, sun) == true);
    CHECK(sun.IsAboveHorizon == true);
    const math::Vector3 solarNoonDirectionWorld = math::Rotate(sun.Rotation, LIGHT_FORWARD);
    // At the equator near the December solstice, the noon Sun is high in the south.
    CHECK(solarNoonDirectionWorld.Y < -0.9F);
    CHECK(solarNoonDirectionWorld.Z < -0.38F);

    REQUIRE(TrySampleEarthCelestialTransform(EarthCelestialBody::Sun, 0.5, 0, 0, sun) == true);
    CHECK(sun.IsAboveHorizon == false);
    CHECK(math::Rotate(sun.Rotation, LIGHT_FORWARD).Y > 0.9F);

    EarthCelestialTransform moon;
    REQUIRE(TrySampleEarthCelestialTransform(EarthCelestialBody::Moon, 0, 0, 0, moon) == true);
    const math::Vector3 firstMoonDirectionWorld = math::Rotate(moon.Rotation, LIGHT_FORWARD);
    CHECK(moon.IsAboveHorizon == true);
    // NASA JPL Horizons, target 301, airless apparent azimuth and elevation at sea level (0° N, 0° E),
    // 2000-01-01 12:00 UTC. See https://ssd-api.jpl.nasa.gov/doc/horizons.html (observer quantity 4).
    constexpr float32 MOON_AZIMUTH_DEGREES = 257.206634F;
    constexpr float32 MOON_ELEVATION_DEGREES = 30.568243F;
    constexpr float32 MOON_ANGLE_TOLERANCE_DEGREES = 1;
    const float32 moonAzimuthRadians = math::DegreesToRadians(MOON_AZIMUTH_DEGREES);
    const float32 moonElevationRadians = math::DegreesToRadians(MOON_ELEVATION_DEGREES);
    const math::Vector3 expectedMoonDirectionWorld{
        -math::Cos(moonElevationRadians) * math::Sin(moonAzimuthRadians),
        -math::Sin(moonElevationRadians),
        math::Cos(moonElevationRadians) * math::Cos(moonAzimuthRadians),
    };
    CHECK(math::Dot(firstMoonDirectionWorld, expectedMoonDirectionWorld) >
          math::Cos(math::DegreesToRadians(MOON_ANGLE_TOLERANCE_DEGREES)));
    REQUIRE(TrySampleEarthCelestialTransform(EarthCelestialBody::Moon, 1, 0, 0, moon) == true);
    const math::Vector3 secondMoonDirectionWorld = math::Rotate(moon.Rotation, LIGHT_FORWARD);
    CHECK(math::Dot(firstMoonDirectionWorld, secondMoonDirectionWorld) < 0.99F);

    const EarthCelestialTransform last = moon;
    CHECK(TrySampleEarthCelestialTransform(EarthCelestialBody::Moon, MAX_EARTH_CELESTIAL_DAYS + 1, 0, 0, moon) ==
          false);
    CHECK(TrySampleEarthCelestialTransform(EarthCelestialBody::Moon, 0, MAX_EARTH_LATITUDE_DEGREES + 1, 0, moon) ==
          false);
    CHECK(moon.Rotation == last.Rotation);
    CHECK(moon.IsAboveHorizon == last.IsAboveHorizon);
}

TEST_CASE("Earth night sky follows planetary ephemerides and sidereal rotation", "[world][celestial]")
{
    struct HorizonAngles final
    {
        float32 AzimuthDegrees;
        float32 ElevationDegrees;
    };
    // NASA JPL Horizons, observer quantity 4, airless at 0° N, 0° E and 0 km, 2000-01-01 12:00 UTC.
    // https://ssd-api.jpl.nasa.gov/doc/horizons.html
    constexpr StaticArray<HorizonAngles, EARTH_VISIBLE_PLANET_COUNT> HORIZONS{{
        {197.803911F, 64.264891F},
        {242.842830F, 46.104095F},
        {106.987824F, 38.687754F},
        {81.168638F, -13.256459F},
        {75.737880F, -27.564694F},
    }};
    EarthNightSkySample noon;
    REQUIRE(TrySampleEarthNightSky(0, 0, 0, noon) == true);
    constexpr float32 ANGULAR_TOLERANCE_DEGREES = 0.2F;
    const float32 minimumDot = math::Cos(math::DegreesToRadians(ANGULAR_TOLERANCE_DEGREES));
    for (usize i = 0; i < HORIZONS.GetSize(); ++i)
    {
        CAPTURE(i);
        const float32 azimuth = math::DegreesToRadians(HORIZONS[i].AzimuthDegrees);
        const float32 elevation = math::DegreesToRadians(HORIZONS[i].ElevationDegrees);
        const math::Vector3 expectedDirectionWorld{
            math::Cos(elevation) * math::Sin(azimuth),
            math::Sin(elevation),
            -math::Cos(elevation) * math::Cos(azimuth),
        };
        CHECK(math::Dot(noon.Planets[i].DirectionWorld, expectedDirectionWorld) > minimumDot);
        CHECK(math::IsFinite(noon.Planets[i].Illuminance) == true);
        CHECK(noon.Planets[i].Illuminance.X > 0);
    }
    constexpr usize VENUS = 1;
    constexpr usize MARS = 2;
    CHECK(noon.Planets[VENUS].Illuminance.X > noon.Planets[MARS].Illuminance.X);
    CHECK(noon.Planets[MARS].Illuminance.X > noon.Planets[MARS].Illuminance.Z);
    CHECK(math::Abs(math::Dot(noon.GalacticCenterWorld, noon.GalacticNorthWorld)) < 1e-6F);
    EarthNightSkySample midnight;
    REQUIRE(TrySampleEarthNightSky(0.5, 0, 0, midnight) == true);
    CHECK(math::Abs(noon.GalacticCenterWorld.Y + midnight.GalacticCenterWorld.Y) < 0.01F);
    CHECK(midnight.Planets[VENUS].DirectionWorld.Y < 0);
    CHECK(TrySampleEarthNightSky(0, 0, MAX_EARTH_LONGITUDE_DEGREES + 1, midnight) == false);
    CHECK(midnight.Planets[VENUS].DirectionWorld.Y < 0);
}

TEST_CASE("Custom orbit sky motion separates world spin from the source path", "[world][celestial]")
{
    constexpr math::Vector3 LIGHT_FORWARD{0, 0, -1};
    constexpr float64 FICTIONAL_WORLD_SPIN_DEGREES_PER_HOUR = 30;
    constexpr float64 WORLD_HALF_TURN_HOURS = 6;
    constexpr float64 WORLD_QUARTER_TURN_HOURS = 3;
    CelestialObserver observer;
    observer.SpinDegreesPerHour = FICTIONAL_WORLD_SPIN_DEGREES_PER_HOUR;
    CelestialOrbit orbit;
    CelestialTransform sample;
    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, 0, sample) == true);
    REQUIRE(sample.IsAboveHorizon == true);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).Y < -0.99F);

    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, WORLD_HALF_TURN_HOURS, sample) == true);
    CHECK(sample.IsAboveHorizon == false);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).Y > 0.99F);

    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, WORLD_QUARTER_TURN_HOURS, sample) == true);
    CHECK(sample.IsAboveHorizon == false);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).X > 0.99F);
    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, -WORLD_QUARTER_TURN_HOURS, sample) == true);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).X < -0.99F);

    orbit.DegreesPerHour = observer.SpinDegreesPerHour;
    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, WORLD_HALF_TURN_HOURS, sample) == true);
    CHECK(sample.IsAboveHorizon == true);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).Y < -0.99F);
}

TEST_CASE("Custom orbits support a tilted nearby moon and preserve rejected samples", "[world][celestial]")
{
    constexpr math::Vector3 LIGHT_FORWARD{0, 0, -1};
    constexpr float64 QUARTER_TURN_DEGREES = 90;
    constexpr float64 ORBIT_TILT_DEGREES = 60;
    constexpr float64 NEARBY_MOON_OBSERVER_RATIO = 0.6;
    CelestialObserver observer;
    observer.SpinDegreesPerHour = 0;
    CelestialOrbit orbit;
    orbit.PhaseDegrees = QUARTER_TURN_DEGREES;
    orbit.AscendingNodeDegrees = -QUARTER_TURN_DEGREES;
    orbit.InclinationDegrees = ORBIT_TILT_DEGREES;

    CelestialTransform sample;
    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, 0, sample) == true);
    CHECK(sample.IsAboveHorizon == true);
    const math::Vector3 distantDirectionWorld = math::Rotate(sample.Rotation, LIGHT_FORWARD);
    CHECK(distantDirectionWorld.Z > 0.8F);
    CHECK(distantDirectionWorld.Y < -0.49F);

    orbit.ObserverRadiusRatio = NEARBY_MOON_OBSERVER_RATIO;
    REQUIRE(TrySampleCelestialOrbitTransform(orbit, observer, 0, sample) == true);
    CHECK(sample.IsAboveHorizon == false);
    CHECK(math::Rotate(sample.Rotation, LIGHT_FORWARD).Y > 0);

    const CelestialTransform accepted = sample;
    CHECK(TrySampleCelestialOrbitTransform(orbit, observer, MAX_CELESTIAL_ORBIT_HOURS + 1, sample) == false);
    orbit.ObserverRadiusRatio = 1;
    CHECK(TrySampleCelestialOrbitTransform(orbit, observer, 0, sample) == false);
    orbit.ObserverRadiusRatio = 0;
    observer.SpinDegreesPerHour = std::numeric_limits<float64>::quiet_NaN();
    CHECK(TrySampleCelestialOrbitTransform(orbit, observer, 0, sample) == false);
    CHECK(sample.Rotation == accepted.Rotation);
    CHECK(sample.IsAboveHorizon == accepted.IsAboveHorizon);
}
