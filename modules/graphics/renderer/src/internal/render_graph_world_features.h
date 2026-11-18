#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/raster.h>

#include "../../shaders/renderer_gpu_preview_layout.h"

namespace ludus::graphics::renderer::internal
{
/// True when two Atmospheres give ray kernels the same sky and haze tables from the same camera. The camera position
/// is compared separately: the tables are resolved again only when the scene is prepared again.
[[nodiscard]] bool AreRayAtmospheresEqual(const AtmosphereDescription& first,
                                          const AtmosphereDescription& second) noexcept;

/// Float4 words of the Atmosphere sky table, header included.
inline constexpr foundation::usize ATMOSPHERE_SKY_TABLE_WORDS =
    LUDUS_ATMOSPHERE_SKY_HEADER_WORDS +
    foundation::usize{LUDUS_ATMOSPHERE_SKY_TABLE_WIDTH} * LUDUS_ATMOSPHERE_SKY_TABLE_HEIGHT;

/// The World-feature tables a graph's ray kernels read for one camera (docs/development/render-graph-authoring.md,
/// "World features"), and the scene inputs the Atmosphere sky changes. Owned by one RenderGraph and reused by its
/// updates; every view stays valid until the next Resolve or destruction.
class RenderGraphWorldTables final
{
public:
    /// Resolve the tables a graph's kernels read. A table a kernel reads but the Atmosphere does not light (disabled,
    /// not lighting the scene, or a camera outside the shell) is a header-only table that keeps the World environment
    /// or omits the haze, as the raster does. When the sky is resolved, GetInputs replaces the World environment with
    /// a constant of the sky's mean radiance, so the environment light exists, and gives the linked Sun the
    /// illuminance transmitted to the camera; EvaluateEnvironment then returns the sky itself.
    /// @param atmosphere The camera's Atmosphere before admission.
    /// @param eyeWorld Camera position in world metres.
    /// @param shouldResolveSky The kernels read the sky (RenderGraphPrograms::HasAtmosphereSky).
    /// @param shouldResolveAerialPerspective The kernels read the haze (RenderGraphPrograms::HasAerialPerspective).
    /// @param inputs Scene inputs before the substitution; borrowed until the next Resolve.
    /// @return Ready; InvalidDescription for invalid Atmosphere fields, as the raster rejects them; OutOfMemory.
    ///         Every status but Ready leaves the previous result unusable.
    [[nodiscard]] rhi::RasterStatus Resolve(const AtmosphereDescription& atmosphere,
                                            foundation::math::Vector3 eyeWorld,
                                            bool shouldResolveSky,
                                            bool shouldResolveAerialPerspective,
                                            const SceneShaderInputs& inputs) noexcept;
    /// Scene inputs for the ray kernels, after the substitution described in Resolve.
    [[nodiscard]] const SceneShaderInputs& GetInputs() const noexcept;
    /// Sky table for RenderGraphSceneDescription::AtmosphereSkyTable; empty when not requested.
    [[nodiscard]] foundation::BorrowedView<const foundation::math::Vector4> GetSkyTable() const noexcept;
    /// Haze table for RenderGraphSceneDescription::AerialPerspectiveTable; empty when not requested.
    [[nodiscard]] foundation::BorrowedView<const foundation::math::Vector4> GetAerialPerspectiveTable() const noexcept;
    /// True when the latest Resolve drew the sky, rather than keeping the World environment.
    [[nodiscard]] bool HasSky() const noexcept;

private:
    foundation::Array<foundation::math::Vector4> mSky;
    foundation::Array<foundation::math::Vector4> mAerialPerspective;
    foundation::Array<DirectLight> mLights;
    SceneShaderInputs mInputs{};
    bool mHasSky = false;
};
} // namespace ludus::graphics::renderer::internal
