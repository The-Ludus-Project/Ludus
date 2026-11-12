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
