#pragma once

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/gameplay/weather/climate.h>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/runtime/resources/scene.h>

#include <string_view>

namespace ludus::first_person
{
/// Encoded admission bound including the manifest terminator and the scene payload.
inline constexpr foundation::usize MAX_PLAYABLE_SNAPSHOT_BYTES =
    content::scene_format::MAX_PLAYABLE_MANIFEST_BYTES + 1 + content::scene_format::MAX_ASSEMBLED_BUNDLE_BYTES;

/// Spawn identity borrows the snapshot's manifest. Position is the eye in metres.
struct PlayerSpawn final
{
    std::string_view Entity;
    gameplay::camera::CameraPose Pose;
    foundation::float32 Speed = 0;
};

/// CPU-owned runtime content, independent of the editor, source files and asset cooker.
/// Object records in Manifest are indexed in Scene.Objects order. Collider indices select that table.
struct PlayableSnapshot final
{
    foundation::parsing::JsonDocument Manifest;
    resources::SceneDefinition Scene;
    graphics::renderer::AtmosphereDescription Atmosphere;
    foundation::Array<PlayerSpawn> Spawns;
    foundation::Array<foundation::usize> ColliderObjects;
    /// Analytic sources whose authored light component disables raster shadow casting.
    foundation::Array<foundation::uint64> RasterUnshadowedLightIds;
    /// The World's validated cooked climate; empty when the World authors no climate.
    gameplay::weather::Climate Climate;
};

/// Decode one complete .luplay file. Views in the result borrow its owned manifest, never bytes.
/// Failure (including a version, digest, reference or admission error) preserves outSnapshot.
[[nodiscard]] bool DecodePlayableSnapshot(foundation::BorrowedView<const foundation::uint8> bytes,
                                          PlayableSnapshot& outSnapshot) noexcept;
} // namespace ludus::first_person
