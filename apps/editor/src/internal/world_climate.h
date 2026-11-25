#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/content/weather/climate_cook.h>
#include <ludus/foundation/containers/array.hpp>

#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QString>

// Authored regional climate of a built-in World: the optional root member "climate", its validation by the
// engine cooker, and its cooked LUWTH bytes for Play. See docs/modules/gameplay/weather.md#author-a-world-climate.
namespace ludus::editor
{
/// World root member holding the World's authored climate. Absent means the World has no weather.
inline constexpr QLatin1StringView WORLD_CLIMATE{"climate"};
/// Layout version of the "climate" member.
inline constexpr foundation::int32 WORLD_CLIMATE_VERSION = 1;
/// Longest authored preset, schedule or region name, in UTF-16 code units.
inline constexpr qsizetype MAX_WORLD_CLIMATE_NAME_LENGTH = 64;

/// Cook a World's "climate" member to LUWTH bytes with the engine cooker, so the Editor admits exactly the
/// climates the runtime loads. Durations are in Play's fixed ticks (PLAYABLE_WORLD_TICKS_PER_SECOND).
/// @param climate The member's value.
/// @param outBytes Replaced by the canonical blob only on success.
/// @param outError On failure, names the failing preset, schedule entry or region and the repair.
/// @return Whether the member is a valid climate.
[[nodiscard]] bool
TryCookWorldClimate(const QJsonValue& climate, foundation::Array<foundation::uint8>& outBytes, QString& outError);

/// Prepare a committed climate edit: keep the climate revision when nothing changed, otherwise raise it by one,
/// and raise the revision of each preset whose values changed, so saved weather detects the edit.
/// @param previous The member before the edit; undefined when the World had no climate.
/// @param edited The edited member; its revisions are replaced.
/// @return The member to commit.
[[nodiscard]] QJsonObject ReviseWorldClimate(const QJsonValue& previous, const QJsonObject& edited);
} // namespace ludus::editor
