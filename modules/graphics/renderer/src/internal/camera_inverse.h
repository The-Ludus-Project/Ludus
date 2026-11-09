#pragma once
#include <ludus/foundation/math/matrix.hpp>

namespace ludus::graphics::renderer
{
struct ViewDescription;
namespace internal
{
/// Invert world-to-clip after rebasing its origin to the camera, preserving output on failure.
[[nodiscard]] bool TryMakeCameraClipToEye(const ViewDescription& camera,
                                          foundation::math::Matrix4& outClipToEye) noexcept;
/// Invert about the camera origin to avoid rejecting a well-conditioned projection far from the world origin.
/// Translation is accumulated in float64. Returns false on invalid inputs or a singular/ill-conditioned
/// eye-relative projection, preserving outClipToWorld. No allocation or shared state.
[[nodiscard]] bool TryMakeCameraClipToWorld(const ViewDescription& camera,
                                            foundation::math::Matrix4& outClipToWorld) noexcept;
} // namespace internal
} // namespace ludus::graphics::renderer
