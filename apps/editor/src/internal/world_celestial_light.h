#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/gameplay/world/celestial_transform.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QStringView>

namespace ludus::gameplay::world
{
struct EarthNightSkySample;
}

namespace ludus::editor
{
inline constexpr QLatin1StringView WORLD_CELESTIAL_ROLE{"celestialRole"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_DRIVER{"celestialDriver"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_MANUAL_PHOTOMETRY{"celestialManualPhotometry"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_EARTH_SUN{"Earth Sun"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_EARTH_MOON{"Earth Moon"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_TIME{"celestialTime"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_LATITUDE{"celestialLatitude"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_LONGITUDE{"celestialLongitude"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_STATIC_SUN{"Static Sun"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_DYNAMIC{"Dynamic Celestial"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_AUTHORED{"Transform"};
inline constexpr QLatin1StringView WORLD_CELESTIAL_ORBIT{"Custom Orbit"};
/// World root member holding the one Earth date, time and observer location that every Earth Sun and
/// Earth Moon light of the World shares. Its members use the light property keys WORLD_CELESTIAL_TIME,
/// WORLD_CELESTIAL_LATITUDE and WORLD_CELESTIAL_LONGITUDE; an omitted member means zero.
inline constexpr QLatin1StringView WORLD_EARTH_OBSERVER{"earth_observer"};

enum class WorldCelestialFieldGroup : foundation::uint8
{
    Other,
    Role,
    Driver,
    Observer,
    Earth,
    Orbit,
};

/// Schema extension for built-in lights. Older Worlds can omit these properties.
[[nodiscard]] QJsonArray MakeWorldCelestialProperties();
/// Classify fields for the Properties inspector and optional-property validation.
[[nodiscard]] WorldCelestialFieldGroup GetWorldCelestialFieldGroup(QStringView property);
/// True for optional fields added by MakeWorldCelestialProperties.
[[nodiscard]] bool IsWorldCelestialProperty(QStringView property);
/// True for a directional light using either Earth preset.
[[nodiscard]] bool IsWorldEarthLight(const QJsonObject& light);
/// Compare the effective time and position, treating omitted coordinates and time as zero.
[[nodiscard]] bool AreWorldEarthObserversEqual(const QJsonObject& first, const QJsonObject& second);
/// Copy the shared Earth observer values into a light, preserving absent defaults.
void CopyWorldEarthObserver(const QJsonObject& source, QJsonObject& inoutLight);
/// True for a directional light whose active celestial rule is Custom Orbit.
[[nodiscard]] bool IsWorldCustomOrbitLight(const QJsonObject& light);
/// The World's shared Earth date, time and location: the root WORLD_EARTH_OBSERVER member or, for a World
/// saved before that member existed, the values stored on its first Earth light. Empty when neither exists.
/// @param world A complete World document with its `entities` array.
[[nodiscard]] QJsonObject GetWorldEarthObserver(const QJsonObject& world);
/// The light's properties with the World's shared Earth date, time and location in place of its own.
/// A Custom Orbit light keeps its own observer coordinates, and an Earth light of a World without the root
/// member keeps the values saved on it. Every reader of a light's Earth sample uses these values.
/// @param light A `light` component's property values.
/// @param world The World document that owns the light.
[[nodiscard]] QJsonObject MakeWorldLightValues(const QJsonObject& light, const QJsonObject& world);
/// Move the Earth date, time and location of an edited Earth light into the World root member, then remove
/// those values from every Earth light so the World stores them once. Values equal to the ones the edited
/// light showed before the edit keep the shared observer, so a newly activated preset inherits it.
/// A light that is not an Earth light after the edit leaves the World unchanged.
/// @param previousWorld The World before the edit; it has the same entity order as inoutWorld.
/// @param editedIndex Index in `entities` of the edited entity.
/// @param inoutWorld The World after the edit.
void LiftWorldEarthObserver(const QJsonObject& previousWorld, qsizetype editedIndex, QJsonObject& inoutWorld);
/// True when a root WORLD_EARTH_OBSERVER value has only the three observer members, each a number within
/// the Earth date and coordinate ranges.
[[nodiscard]] bool IsWorldEarthObserverValid(const QJsonValue& value);
/// Sample the selected Earth preset. An inactive or invalid preset preserves outLighting.
[[nodiscard]] bool TrySampleWorldEarthLight(const QJsonObject& light,
                                            gameplay::world::EarthCelestialLighting& outLighting);
/// Sample the selected celestial rule. An authored or static light keeps its rotation.
/// Failure preserves the outputs. A body at or below the horizon sets the horizon flag to false.
[[nodiscard]] bool TryResolveWorldCelestialRotation(const QJsonObject& light,
                                                    foundation::math::Quaternion& inoutRotation,
                                                    bool& outIsAboveHorizon,
                                                    gameplay::world::EarthCelestialLighting& outEarthLighting);
/// Sample the Earth background from a dynamic Earth Sun or Earth Moon driver. Preserves outSky otherwise.
[[nodiscard]] bool TrySampleWorldEarthNightSky(const QJsonObject& light, gameplay::world::EarthNightSkySample& outSky);
} // namespace ludus::editor
