#include "internal/probe_volume.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;

bool IsGiProbeGridValid(const GiProbeGrid& grid) noexcept
{
    if (grid.Counts[0] == 0 && grid.Counts[1] == 0 && grid.Counts[2] == 0)
    {
        return true;
    }
    if (math::IsFinite(grid.OriginWorld) == false || math::IsFinite(grid.Spacing) == false ||
        grid.Spacing < static_cast<float32>(MIN_GI_PROBE_SPACING_METRES) ||
        grid.Spacing > static_cast<float32>(MAX_GI_PROBE_SPACING_METRES))
    {
        return false;
    }
    for (const uint32 count : grid.Counts)
    {
        if (count < 2 || count > MAX_GI_PROBE_AXIS_COUNT)
        {
            return false;
        }
    }
    const math::Vector3 extentWorld = {static_cast<float32>(grid.Counts[0] - 1) * grid.Spacing,
                                       static_cast<float32>(grid.Counts[1] - 1) * grid.Spacing,
                                       static_cast<float32>(grid.Counts[2] - 1) * grid.Spacing};
    const math::Vector3 endWorld = grid.OriginWorld + extentWorld;
    return grid.OriginWorld.X >= -internal::MAX_WORLD_COORDINATE &&
           grid.OriginWorld.Y >= -internal::MAX_WORLD_COORDINATE &&
           grid.OriginWorld.Z >= -internal::MAX_WORLD_COORDINATE && endWorld.X <= internal::MAX_WORLD_COORDINATE &&
           endWorld.Y <= internal::MAX_WORLD_COORDINATE && endWorld.Z <= internal::MAX_WORLD_COORDINATE;
}
} // namespace ludus::graphics::renderer

namespace ludus::graphics::renderer::internal
{
namespace
{
using Status = rhi::RasterStatus;
constexpr uint16 PROBE_DIRECTION_Z_SLOT = 0;
constexpr uint16 PROBE_DIRECTION_ANGLE_SLOT = 1;
constexpr uint32 PROBE_STREAM_MASK = 0x80000000U;
constexpr float64 UNORM_BYTE_MAX = 255;
constexpr float32 IRRADIANCE_MAX = 10000;
constexpr float32 SPHERE_SOLID_ANGLE_FACTOR = 4;

uint8 EncodeUnitByte(float32 value) noexcept
{
    const float64 scaled = static_cast<float64>(math::Clamp(value, 0.F, 1.F)) * UNORM_BYTE_MAX;
    return static_cast<uint8>(math::Floor(scaled + .5));
}

math::Vector3 ComputeProbePosition(const GiProbeGrid& grid, uint32 probeIndex) noexcept
{
    const uint32 x = probeIndex % grid.Counts[0];
    const uint32 y = (probeIndex / grid.Counts[0]) % grid.Counts[1];
    const uint32 z = probeIndex / (grid.Counts[0] * grid.Counts[1]);
    return grid.OriginWorld +
           math::Vector3{static_cast<float32>(x), static_cast<float32>(y), static_cast<float32>(z)} * grid.Spacing;
}

math::Vector3 ComputeVisibilityDirection(uint32 texel) noexcept
{
    const uint32 face = texel / LUDUS_GI_PROBE_FACE_TEXELS;
    const uint32 faceTexel = texel % LUDUS_GI_PROBE_FACE_TEXELS;
    const uint32 column = faceTexel % LUDUS_GI_PROBE_FACE_WIDTH;
    const uint32 row = faceTexel / LUDUS_GI_PROBE_FACE_WIDTH;
    constexpr float32 TEXEL_SPAN = 2.F / LUDUS_GI_PROBE_FACE_WIDTH;
    const float32 u = (static_cast<float32>(column) + .5F) * TEXEL_SPAN - 1;
    const float32 v = (static_cast<float32>(row) + .5F) * TEXEL_SPAN - 1;
    const float32 axis = face % 2 == 0 ? 1.F : -1.F;
    const math::Vector3 directionWorld =
        face < 2 ? math::Vector3{axis, u, v} : (face < 2 * 2 ? math::Vector3{v, axis, u} : math::Vector3{u, v, axis});
    return directionWorld / math::Length(directionWorld);
}
} // namespace

uint32 CountGiProbes(const GiProbeGrid& grid) noexcept
{
    return grid.Counts[0] * grid.Counts[1] * grid.Counts[2];
}

uint32 ComputeGiProbeAtlasHeight(const GiProbeGrid& grid) noexcept
{
    const uint32 texels = CountGiProbes(grid) * LUDUS_GI_PROBE_TEXELS;
    return (texels + LUDUS_GI_PROBE_ATLAS_WIDTH - 1) / LUDUS_GI_PROBE_ATLAS_WIDTH;
}

rhi::RasterStatus ProbeVolumeBake::Initialize(const GiProbeGrid& grid, usize byteBudget) noexcept
{
    if (IsGiProbeGridValid(grid) == false)
    {
        return Status::InvalidDescription;
    }
    const uint32 count = CountGiProbes(grid);
    const usize pixelBytes =
        usize{ComputeGiProbeAtlasHeight(grid)} * LUDUS_GI_PROBE_ATLAS_WIDTH * content::scene_format::TEXEL_BYTES;
    if (usize{count} * sizeof(Probe) + pixelBytes > byteBudget)
    {
        return Status::CapacityExceeded;
    }
    if (mProbes.TryResize(count) == false || mPixels.TryResize(pixelBytes) == false)
    {
        return Status::OutOfMemory;
    }
    mGrid = grid;
    return Status::Ready;
}

rhi::RasterStatus
ProbeVolumeBake::Accumulate(const ReferenceJob& job, ReferenceCaptureInfo& inoutInfo, GiProbeSample address) noexcept
{
    const uint32 probeIndex = address.ProbeIndex;
    const uint32 sampleIndex = address.SampleIndex;
    Probe& probe = mProbes[probeIndex];
    const math::Vector3 positionWorld = ComputeProbePosition(mGrid, probeIndex);
    if (sampleIndex < LUDUS_GI_PROBE_VISIBILITY_TEXELS)
    {
        // Inspired by Majercik, Guertin, Nowrouzezahrai and McGuire, "Dynamic Diffuse Global
        // Illumination with Ray-Traced Irradiance Fields", JCGT 8(2), 2019, section 5.
        // https://jcgt.org/published/0008/02/01/ . Thanks to the authors. This offline
        // prototype stores nearest axial depth instead of filtered distance moments.
        const math::Vector3 directionWorld = ComputeVisibilityDirection(sampleIndex);
        const float32 axial =
            math::Max(math::Abs(directionWorld.X), math::Max(math::Abs(directionWorld.Y), math::Abs(directionWorld.Z)));
        const float32 maxAxialDistance = mGrid.Spacing * LUDUS_GI_PROBE_DEPTH_CELLS;
        rhi::RayResult hit;
        const Status status =
            TraceReferenceOracle(job.Scene, ReferenceRay(positionWorld, directionWorld, maxAxialDistance / axial), hit);
        if (status == Status::Ready)
        {
            probe.Depth[sampleIndex] = EncodeUnitByte(hit.Hit == 0 ? 1.F : hit.Distance * axial / maxAxialDistance);
            // Do not illuminate from a probe touching geometry, where its free-space directions are undefined.
            constexpr uint8 MINIMUM_CLEARANCE_DEPTH = 2;
            probe.IsValid = probe.IsValid == true && probe.Depth[sampleIndex] >= MINIMUM_CLEARANCE_DEPTH;
        }
        return status;
    }

    if (probe.IsValid == false)
    {
        return Status::Ready;
    }
    const uint32 stream = probeIndex | PROBE_STREAM_MASK;
    const uint32 sample = sampleIndex - LUDUS_GI_PROBE_VISIBILITY_TEXELS;
    const float32 z = 2 * math::SampleFloat01(job.Key, {stream, sample, PROBE_DIRECTION_Z_SLOT}) - 1;
    const float32 angle = math::kTauF * math::SampleFloat01(job.Key, {stream, sample, PROBE_DIRECTION_ANGLE_SLOT});
    const float32 radius = math::Sqrt(math::Max(0.F, 1 - z * z));
    const math::Vector3 directionWorld = {radius * math::Cos(angle), radius * math::Sin(angle), z};
    math::Vector3 radiance;
    const Status status =
        TraceReferenceGiProbe(job, inoutInfo, {stream, sample}, positionWorld, directionWorld, radiance);
    if (status != Status::Ready)
    {
        return status;
    }
    const StaticArray<float32, LUDUS_GI_PROBE_FACES> projectedCosines{directionWorld.X,
                                                                      -directionWorld.X,
                                                                      directionWorld.Y,
                                                                      -directionWorld.Y,
                                                                      directionWorld.Z,
                                                                      -directionWorld.Z};
    for (uint32 face = 0; face < LUDUS_GI_PROBE_FACES; ++face)
    {
        probe.Irradiance[face].Add(radiance * math::Max(0.F, projectedCosines[face]));
    }
    return Status::Ready;
}

rhi::RasterStatus ProbeVolumeBake::Encode() noexcept
{
    constexpr float32 SAMPLE_WEIGHT = SPHERE_SOLID_ANGLE_FACTOR * math::kPiF / GI_PROBE_RADIANCE_SAMPLES;
    constexpr usize PIXEL_BYTES = content::scene_format::TEXEL_BYTES;
    constexpr usize BLUE_CHANNEL = 2;
    constexpr usize ALPHA_CHANNEL = PIXEL_BYTES - 1;
    for (const Probe& probe : mProbes)
    {
        for (const ReferenceSum& sum : probe.Irradiance)
        {
            const math::Vector3 irradiance = sum.Value * SAMPLE_WEIGHT;
            if (ReferenceColor(irradiance, IRRADIANCE_MAX) == false)
            {
                return Status::CapacityExceeded;
            }
            mScale = math::Max(mScale, math::Max(irradiance.X, math::Max(irradiance.Y, irradiance.Z)));
        }
    }
    usize offset = 0;
    for (const Probe& probe : mProbes)
    {
        for (const ReferenceSum& sum : probe.Irradiance)
        {
            const math::Vector3 normalized = mScale > 0 ? sum.Value * (SAMPLE_WEIGHT / mScale) : math::Vector3{};
            mPixels[offset] = EncodeUnitByte(normalized.X);
            mPixels[offset + 1] = EncodeUnitByte(normalized.Y);
            mPixels[offset + BLUE_CHANNEL] = EncodeUnitByte(normalized.Z);
            mPixels[offset + ALPHA_CHANNEL] = probe.IsValid == true ? static_cast<uint8>(UNORM_BYTE_MAX) : 0;
            offset += PIXEL_BYTES;
        }
        for (const uint8 depth : probe.Depth)
        {
            mPixels[offset] = depth;
            offset += PIXEL_BYTES;
        }
    }
    return Status::Ready;
}

rhi::RasterStatus ProbeVolumeBake::GetAtlas(StaticGiProbeAtlas& outAtlas) const noexcept
{
    if (mProbes.IsEmpty() == true)
    {
        return Status::NotReady;
    }
    outAtlas =
    {
        .Grid = mGrid,
        .Width = LUDUS_GI_PROBE_ATLAS_WIDTH,
        .Height = ComputeGiProbeAtlasHeight(mGrid),
        .IrradianceScale = mScale,
        .Pixels = {mPixels.GetData(), mPixels.GetSize()},
    };
    return Status::Ready;
}
} // namespace ludus::graphics::renderer::internal
