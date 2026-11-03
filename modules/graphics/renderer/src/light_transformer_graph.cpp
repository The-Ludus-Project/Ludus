#include "internal/light_transformer_graph.h"

#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/surface_points.hpp>

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, sections 3.2-3.4 and appendix A.3,
// https://arxiv.org/abs/2510.18189. Use a fixed training scale and translate by
// scene minimum; independently normalizing every scene would alter physical scale.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
Status LightTransformerGraph::Initialize(const LightTransformerModelDescription& description) noexcept
{
    if (description.EncoderAsset.IsValid() == false || description.EncoderAsset.IsEmpty() == true ||
        description.DecoderAsset.IsValid() == false || description.DecoderAsset.IsEmpty() == true ||
        math::IsFinite(description.WorldUnitsPerModelUnit) == false || description.WorldUnitsPerModelUnit <= 0 ||
        math::IsFinite(1 / description.WorldUnitsPerModelUnit) == false ||
        description.ScenePoints < LUDUS_LIGHT_ENCODER_MIN_POINTS ||
        description.ScenePoints > LUDUS_LIGHT_ENCODER_MAX_POINTS || description.MaximumBufferBytes == 0 ||
        description.MaximumSurfacePreparationBytes == 0 ||
        description.MaximumSamplingAttempts < description.ScenePoints ||
        description.EncoderShader.ShouldPrepareAsynchronously == true)
    {
        return Status::InvalidDescription;
    }
    if (description.EncoderAsset.GetSize() > description.MaximumBufferBytes)
    {
        return Status::CapacityExceeded;
    }
    Status status = ReadLightTransformerDecoderWeights(description.DecoderAsset, mDecoder);
    if (status != Status::Ready)
    {
        return status;
    }
    if (mEncoder.TryResize(description.EncoderAsset.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < mEncoder.GetSize(); ++i)
    {
        mEncoder[i] = description.EncoderAsset[i];
    }
    mShader = description.EncoderShader;
    mWorldUnitsPerModelUnit = description.WorldUnitsPerModelUnit;
    mCount = description.ScenePoints;
    mSeed = description.Seed;
    mMaximumBufferBytes = description.MaximumBufferBytes;
    mMaximumSurfacePreparationBytes = description.MaximumSurfacePreparationBytes;
    mMaximumSamplingAttempts = description.MaximumSamplingAttempts;
    return Status::Ready;
}
usize LightTransformerGraph::GetRequiredPacketBytes() const noexcept
{
    constexpr usize WORD_CHANNELS = LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
    constexpr usize DECODER_WORDS = (LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT + WORD_CHANNELS - 1) / WORD_CHANNELS;
    const usize anchors = mCount / LUDUS_LIGHT_ENCODER_ANCHOR_DIVISOR;
    return (LUDUS_LIGHT_ENCODER_PACKET_HEADER_WORDS + anchors * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS + DECODER_WORDS) *
               sizeof(math::Vector4) +
           anchors * sizeof(geometry::PointSearchNode);
}
Status LightTransformerGraph::Prepare(const RenderGraphSceneDescription& scene,
                                      rhi::DeviceHandle device,
                                      LightTransformerEncoding& outEncoding,
                                      LightTransformerPreparationReport& outReport) const noexcept
{
    outReport = {};
    outReport.RequestedPoints = mCount;
    outReport.WorldUnitsPerModelUnit = mWorldUnitsPerModelUnit;
    outReport.MaximumBufferBytes = mMaximumBufferBytes;
    outReport.MaximumSurfacePreparationBytes = mMaximumSurfacePreparationBytes;
    outReport.MaximumSamplingAttempts = mMaximumSamplingAttempts;
    using Issue = LightTransformerPreparationIssue;
    if (scene.HasMaterialInterface == false || scene.Inputs.Materials.IsValid() == false ||
        scene.Inputs.Lights.IsValid() == false || scene.Inputs.EnvironmentPixels.IsValid() == false)
    {
        outReport.Issue = Issue::MaterialInterface;
        return Status::InvalidDescription;
    }
    outReport.AnalyticLights = static_cast<uint32>(scene.Inputs.Lights.GetSize());
    if (scene.Inputs.Lights.IsEmpty() == false)
    {
        outReport.Issue = Issue::AnalyticLights;
        return Status::Unsupported;
    }
    if (scene.Inputs.Environment.X != 0 || scene.Inputs.Environment.Y != 0 || scene.Inputs.Environment.Z != 0 ||
        scene.Inputs.EnvironmentPixels.IsEmpty() == false)
    {
        outReport.Issue = Issue::Environment;
        return Status::Unsupported;
    }
    for (usize i = 0; i < scene.Inputs.Materials.GetSize(); ++i)
    {
        const SceneShaderMaterial& material = scene.Inputs.Materials[i];
        if (material.Scattering.Model != content::scene_format::MaterialModel::Diffuse || material.Metallic != 0)
        {
            ++outReport.UnsupportedMaterials;
        }
    }
    if (outReport.UnsupportedMaterials != 0)
    {
        outReport.Issue = Issue::Materials;
        return Status::Unsupported;
    }
    SurfacePointCloud cloud;
    const SurfacePointSampling sampling{mCount, mSeed, mMaximumSamplingAttempts, mMaximumSurfacePreparationBytes};
    Status status = PrepareSurfacePointCloud(scene, sampling, cloud);
    if (status != Status::Ready)
    {
        outReport.Issue = Issue::Sampling;
        return status;
    }
    for (const SurfacePoint& point : cloud.Points)
    {
        if (point.MaterialIndex == DEFAULT_SCENE_MATERIAL)
        {
            ++outReport.UnassignedSamples;
        }
    }
    if (outReport.UnassignedSamples != 0)
    {
        outReport.Issue = Issue::Materials;
        return Status::Unsupported;
    }
    const math::Vector3 extentWorld = cloud.BoundsWorld.Max - cloud.BoundsWorld.Min;
    outReport.ExtentWorld = extentWorld;
    if (extentWorld.X > mWorldUnitsPerModelUnit || extentWorld.Y > mWorldUnitsPerModelUnit ||
        extentWorld.Z > mWorldUnitsPerModelUnit)
    {
        outReport.Issue = Issue::FixedScale;
        return Status::CapacityExceeded;
    }
    const float32 scale = 1 / mWorldUnitsPerModelUnit;
    const math::Vector3 minimumWorld = cloud.BoundsWorld.Min;
    const math::Vector4 worldToModel{minimumWorld.X, minimumWorld.Y, minimumWorld.Z, scale};
    Array<LightTransformerScenePoint> points;
    if (points.TryResize(cloud.Points.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        const SurfacePoint& point = cloud.Points[i];
        const math::Vector3 positionModel = (point.PositionWorld - minimumWorld) * scale;
        points[i] = {{positionModel.X, positionModel.Y, positionModel.Z, 0},
                     {point.NormalWorld.X, point.NormalWorld.Y, point.NormalWorld.Z, 0},
                     {point.Albedo.X, point.Albedo.Y, point.Albedo.Z, 0},
                     {point.Emission.X, point.Emission.Y, point.Emission.Z, 0}};
    }
    status = PrepareLightTransformerEncoder({mEncoder.GetData(), mEncoder.GetSize()},
                                            {points.GetData(), points.GetSize()},
                                            worldToModel,
                                            device,
                                            mShader,
                                            mMaximumBufferBytes,
                                            outEncoding,
                                            {mDecoder.GetData(), mDecoder.GetSize()});
    if (status != Status::Ready)
    {
        outReport.Issue = status == Status::InvalidDescription ? Issue::ModelAsset : Issue::DeviceCapacity;
    }
    return status;
}
} // namespace ludus::graphics::renderer::internal
