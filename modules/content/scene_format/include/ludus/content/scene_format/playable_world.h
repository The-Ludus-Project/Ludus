#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/limits.h>

#include <string_view>

namespace ludus::content::scene_format
{
/// Additional authored entities beside the imported scene instances in a built-in World.
inline constexpr foundation::int32 MAX_AUTHORED_WORLD_ENTITIES = 128;
/// Total entity keys addressable in one prepared or applied World.
inline constexpr foundation::int32 MAX_WORLD_ENTITIES =
    static_cast<foundation::int32>(IMPORT_LIMITS.Objects) + MAX_AUTHORED_WORLD_ENTITIES;
/// Version of the playable World manifest. One compact JSON line precedes its LUSCN bytes. Version 2 adds the
/// "climate" member: null, or the World's cooked LUWTH climate as lowercase hex "bytes" with its "sha256".
inline constexpr foundation::uint32 PLAYABLE_WORLD_VERSION = 2;
/// Fixed session ticks per second of Editor and standalone Play. A playable World's climate durations use it.
inline constexpr foundation::uint32 PLAYABLE_WORLD_TICKS_PER_SECOND = 60;
/// Maximum UTF-8 bytes before the line terminator, independent of the LUSCN version's bundle budget.
inline constexpr foundation::usize MAX_PLAYABLE_MANIFEST_BYTES = foundation::usize{8} * 1024 * 1024;
/// Engine raster renderer supported by the first-person World player.
inline constexpr std::string_view PLAYABLE_RASTER_RENDERER = "raster";
} // namespace ludus::content::scene_format
