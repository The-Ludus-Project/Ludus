#pragma once

#include "../src/internal/reference_data.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/gaussian.hpp>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
/// Independent CPU replay of the template's two-scattering mesh-emitter integral.
/// Borrowed scene storage must outlive this oracle. Fixed-mip color, surface and emission maps
/// and masked coverage follow the reference scene helpers. Coatings/analytic/environment lights are unsupported.
class OneMoreVertexOracle final
{
public:
    /// Freeze sampling statistics over the supplied BVH. False preserves no usable sampling state.
    [[nodiscard]] bool Build(const internal::ReferenceSceneData& scene) noexcept;
    /// Emission/direct/indirect AOVs from one primary ray and independent geometry/BSDF+NEE proposals.
    /// @pre Build succeeded for this unchanged scene and estimator is in [0, 2].
    /// estimator 0 combines both with balance MIS; 1 selects geometry, 2 BSDF+NEE. Uses explicit addressed random.
    [[nodiscard]] math::Vector3 Evaluate(const internal::ReferenceSceneData& scene,
                                         const rhi::RayInput& cameraRay,
                                         math::RandomKey key,
                                         uint32 pixel,
                                         uint32 sample,
                                         uint32 estimator,
                                         math::Vector3& outEmission,
                                         math::Vector3& outDirect,
                                         math::Vector3& outIndirect) const noexcept;
    /// Area-density integral over every BVH leaf; must equal one for all supported endpoint pairs.
    [[nodiscard]] float32 IntegratePdf(const internal::ReferenceSceneData& scene,
                                       uint32 receiverTriangle,
                                       uint32 emitterTriangle) const noexcept;

private:
    struct Statistics final
    {
        float32 AreaWorld = 0;
        math::Vector3 PositionSumWorld{};
        math::Vector3 DiagonalSumWorld{};
        math::Vector3 CrossSumWorld{};
        math::Vector3 NormalSumWorld{};
        math::Vector3 ColorSum{};
        float32 MetallicSum = 0;
        float32 AlphaSquaredSum = 0;
        float32 SpecularitySum = 0;
        math::GaussianAxes AxesWorld;
    };
    struct Point final
    {
        math::Vector3 PositionWorld{};
        math::Vector3 NormalWorld{};
        math::Vector3 GeometricNormalWorld{};
        ReferenceBsdf Bsdf;
        math::Vector3 Emission{};
        uint32 Triangle = 0;
        bool IsEmissionTwoSided = false;
        bool HasCoverage = true;
    };
    struct Context final
    {
        Point Receiver;
        Point Emitter;
        math::Vector3 ViewWorld{};
    };
    Array<Statistics> mStatistics;
    Array<uint32> mLeafIndices;
    math::Vector3 mOriginWorld{};
    [[nodiscard]] Point
    loadPoint(const internal::ReferenceSceneData& scene, uint32 triangle, float32 u, float32 v) const noexcept;
    [[nodiscard]] float32
    evaluateImportance(const internal::ReferenceSceneData& scene, uint32 node, const Context& context) const noexcept;
    [[nodiscard]] float32 computeLeftProbability(const internal::ReferenceSceneData& scene,
                                                 uint32 node,
                                                 const Context& context) const noexcept;
    [[nodiscard]] float32
    evaluatePdf(const internal::ReferenceSceneData& scene, uint32 triangle, const Context& context) const noexcept;
    [[nodiscard]] Point sampleGeometry(const internal::ReferenceSceneData& scene,
                                       const Context& context,
                                       math::Vector3 random,
                                       float32& outPdf) const noexcept;
    [[nodiscard]] math::Vector3 evaluateConnection(const internal::ReferenceSceneData& scene,
                                                   const Context& context,
                                                   const Point& middle,
                                                   bool shouldTestFirst,
                                                   float32& outBsdfPdf) const noexcept;
    [[nodiscard]] bool
    isVisible(const internal::ReferenceSceneData& scene, const Point& from, const Point& to) const noexcept;
};
} // namespace ludus::graphics::renderer::testing
