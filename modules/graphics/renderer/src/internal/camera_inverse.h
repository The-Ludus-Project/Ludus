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
/// Rebase a clip-to-world inverse to the eye: subtract eyeWorld times the homogeneous row from the position rows,
/// accumulated in float64. For a world inverse that is already well conditioned; no allocation or shared state.
[[nodiscard]] foundation::math::Matrix4 MakeCameraClipToEye(const foundation::math::Matrix4& clipToWorld,
                                                            foundation::math::Vector3 eyeWorld) noexcept;
/// Invert about the camera origin to avoid rejecting a well-conditioned projection far from the world origin.
/// Translation is accumulated in float64. Returns false on invalid inputs or a singular/ill-conditioned
/// eye-relative projection, preserving outClipToWorld. No allocation or shared state.
[[nodiscard]] bool TryMakeCameraClipToWorld(const ViewDescription& camera,
                                            foundation::math::Matrix4& outClipToWorld) noexcept;
} // namespace internal
} // namespace ludus::graphics::renderer
