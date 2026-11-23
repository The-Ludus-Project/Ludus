#pragma once

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/strings/static_string.hpp>
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

/// Longest renderer name the diagnostic reports, enough to name any manifest renderer the player
/// rejects (for example "reference"); a longer name is truncated on a UTF-8 boundary.
inline constexpr foundation::usize MAX_PLAYABLE_RENDERER_NAME_BYTES = 64;

/// Why DecodePlayableSnapshot accepted or rejected a .luplay file. An unsupported renderer is a valid
/// manifest the player cannot draw, distinct from an unreadable file the author must prepare again.
enum class PlayableSnapshotStatus : foundation::uint8
{
    Ok,                  ///< The snapshot decoded; outSnapshot holds it.
    Unreadable,          ///< The file is missing, truncated, tampered, or otherwise not a valid snapshot.
    UnsupportedRenderer, ///< The manifest is well-formed but names a renderer the player cannot draw.
};

/// Outcome of DecodePlayableSnapshot. On UnsupportedRenderer, Renderer names the rejected renderer so the
/// caller can tell the author which profile to replace; it is empty for every other status.
struct PlayableSnapshotDiagnostic final
{
    PlayableSnapshotStatus Status = PlayableSnapshotStatus::Unreadable; ///< Why the file was accepted or rejected.
    /// The rejected renderer name on UnsupportedRenderer, truncated to MAX_PLAYABLE_RENDERER_NAME_BYTES; empty
    /// otherwise.
    foundation::StaticString<MAX_PLAYABLE_RENDERER_NAME_BYTES> Renderer;
};

/// Decode one complete .luplay file and report why it was accepted or rejected. Views in the result
/// borrow its owned manifest, never bytes. Failure (including a version, digest, reference, admission or
/// unsupported-renderer error) preserves outSnapshot. On an unsupported renderer the manifest parsed and
/// its renderer is a valid string the player cannot draw; outDiagnostic names it.
/// @param bytes The complete .luplay file.
/// @param outSnapshot Replaced only when the status is Ok.
/// @param outDiagnostic The decode status and, for an unsupported renderer, the rejected renderer name.
void DecodePlayableSnapshot(foundation::BorrowedView<const foundation::uint8> bytes,
                            PlayableSnapshot& outSnapshot,
                            PlayableSnapshotDiagnostic& outDiagnostic) noexcept;

/// Decode one complete .luplay file. Views in the result borrow its owned manifest, never bytes.
/// Failure (including a version, digest, reference or admission error) preserves outSnapshot.
/// @return True when the snapshot decoded; false for every rejection, including an unsupported renderer.
/// Use the three-argument overload to distinguish an unsupported renderer from an unreadable file.
[[nodiscard]] bool DecodePlayableSnapshot(foundation::BorrowedView<const foundation::uint8> bytes,
                                          PlayableSnapshot& outSnapshot) noexcept;
} // namespace ludus::first_person
