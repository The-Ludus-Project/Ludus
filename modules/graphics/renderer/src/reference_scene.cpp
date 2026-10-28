#include "internal/reference_data.h"

#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/status.hpp>

#include <cstring>

// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle
// Intersection", JCGT 2(1), 2013, section 3,
// https://jcgt.org/published/0002/01/05/paper.pdf: use one ray-dependent axis
// permutation/shear and consistent edge functions. This independent CPU oracle
// computes every edge in float64, traverses a binned-SAH BVH (BuildNode) and does
// not claim the paper's complete optimized conservative-bound implementation.
namespace ludus::graphics::renderer::internal
{
namespace
{
float32 Component(math::Vector3 p, usize axis) noexcept
{
    return axis == 0 ? p.X : axis == 1 ? p.Y : p.Z;
}
math::Aabb3 TriangleBounds(const ReferenceTriangle& triangle) noexcept
{
    auto bounds = math::EmptyAabb3();
    for (auto p : triangle.Positions)
    {
        bounds = math::Merge(bounds, {p, p});
    }
    return bounds;
}
struct TriangleLess final
{
    const ReferenceSceneData& Scene;
    usize Axis;
    bool operator()(uint32 a, uint32 b) const noexcept
    {
        const auto& left = Scene.Triangles[a];
        const auto& right = Scene.Triangles[b];
        const auto x = Component(left.Positions[0] + left.Positions[1] + left.Positions[2], Axis);
        const auto y = Component(right.Positions[0] + right.Positions[1] + right.Positions[2], Axis);
        return x == y ? a < b : x < y;
    }
};
// Thanks to Ingo Wald, "On fast Construction of SAH-based Bounding Volume
// Hierarchies", IEEE/Eurographics Symposium on Interactive Ray Tracing 2007,
// pp. 33-40, https://www.sci.utah.edu/~wald/Publications/2007/ParallelBVHBuild/fastbuild.pdf:
// bin triangle centroids along each axis and choose the plane with the lowest
// surface area heuristic cost. This serial build keeps the existing binary node
// layout and four-triangle leaves, and falls back to a balanced median split
// whenever SAH splitting could exceed REFERENCE_BVH_MAX_DEPTH.
constexpr usize AXIS_COUNT = 3;
constexpr usize SAH_BIN_COUNT = 16;
// Relative costs of visiting one node and testing one triangle.
constexpr float64 SAH_TRAVERSAL_COST = 1;
constexpr float64 SAH_INTERSECTION_COST = 1;
// Three times the centroid; TriangleLess orders by the same sum.
math::Vector3 ComputeCentroidSum(const ReferenceTriangle& triangle) noexcept
{
    return triangle.Positions[0] + triangle.Positions[1] + triangle.Positions[2];
}
// Half the surface area; SAH compares ratios, so the factor of two cancels.
float64 ComputeHalfArea(const math::Aabb3& bounds) noexcept
{
    if (bounds.Min.X > bounds.Max.X || bounds.Min.Y > bounds.Max.Y || bounds.Min.Z > bounds.Max.Z)
    {
        return 0;
    }
    const float64 x = float64(bounds.Max.X) - float64(bounds.Min.X);
    const float64 y = float64(bounds.Max.Y) - float64(bounds.Min.Y);
    const float64 z = float64(bounds.Max.Z) - float64(bounds.Min.Z);
    return x * y + y * z + z * x;
}
constexpr uint32 UINT32_BITS = 32;
uint32 ComputeCeilingLog2(uint32 value) noexcept
{
    uint32 result = 0;
    while (result < UINT32_BITS && (uint64{1} << result) < value)
    {
        ++result;
    }
    return result;
}
struct SahSplit final
{
    float64 Cost = 0;
    usize Axis = 0;
    usize Bin = 0;
    bool IsValid = false;
};
usize ComputeBin(float32 centroid, float32 minimum, float32 extent) noexcept
{
    const float64 scaled = (float64(centroid) - float64(minimum)) / float64(extent) * float64(SAH_BIN_COUNT);
    return scaled <= 0 ? 0 : math::Min(static_cast<usize>(scaled), SAH_BIN_COUNT - 1);
}
SahSplit
FindSahSplit(const ReferenceSceneData& scene, const ReferenceBvhNode& node, const math::Aabb3& centroidBounds) noexcept
{
    SahSplit best;
    const float64 area = ComputeHalfArea(node.Bounds);
    if (area <= 0)
    {
        return best;
    }
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float32 minimum = Component(centroidBounds.Min, axis);
        const float32 extent = Component(centroidBounds.Max, axis) - minimum;
        if ((extent > 0) == false)
        {
            continue;
        }
        StaticArray<uint32, SAH_BIN_COUNT> counts{};
        StaticArray<math::Aabb3, SAH_BIN_COUNT> binBounds;
        binBounds.Fill(math::EmptyAabb3());
        for (usize i = node.First; i < usize(node.First) + node.Count; ++i)
        {
            const ReferenceTriangle& triangle = scene.Triangles[scene.Order[i]];
            const usize bin = ComputeBin(Component(ComputeCentroidSum(triangle), axis), minimum, extent);
            ++counts[bin];
            binBounds[bin] = math::Merge(binBounds[bin], TriangleBounds(triangle));
        }
        // Suffix areas/counts give every plane's right side; the left side accumulates.
        StaticArray<float64, SAH_BIN_COUNT> rightAreas{};
        StaticArray<uint32, SAH_BIN_COUNT> rightCounts{};
        math::Aabb3 right = math::EmptyAabb3();
        uint32 rightCount = 0;
        for (usize bin = SAH_BIN_COUNT - 1; bin > 0; --bin)
        {
            right = math::Merge(right, binBounds[bin]);
            rightCount += counts[bin];
            rightAreas[bin] = ComputeHalfArea(right);
            rightCounts[bin] = rightCount;
        }
        math::Aabb3 left = math::EmptyAabb3();
        uint32 leftCount = 0;
        for (usize bin = 1; bin < SAH_BIN_COUNT; ++bin)
        {
            left = math::Merge(left, binBounds[bin - 1]);
            leftCount += counts[bin - 1];
            if (leftCount == 0 || rightCounts[bin] == 0)
            {
                continue;
            }
            const float64 cost =
                SAH_TRAVERSAL_COST +
                SAH_INTERSECTION_COST *
                    (ComputeHalfArea(left) * float64(leftCount) + rightAreas[bin] * float64(rightCounts[bin])) / area;
            if (best.IsValid == false || cost < best.Cost)
            {
                best = {cost, axis, bin, true};
            }
        }
    }
    return best;
}
uint32 BuildNode(ReferenceSceneData& inoutScene, uint32 first, uint32 count, uint32 depth, uint32* scratch) noexcept
{
    const uint32 node = static_cast<uint32>(inoutScene.Nodes.GetSize());
    ReferenceBvhNode value;
    value.Bounds = math::EmptyAabb3();
    value.First = first;
    value.Count = count;
    math::Aabb3 centroidBounds = math::EmptyAabb3();
    for (usize i = first; i < usize(first) + count; ++i)
    {
        const ReferenceTriangle& triangle = inoutScene.Triangles[inoutScene.Order[i]];
        const math::Vector3 centroid = ComputeCentroidSum(triangle);
        value.Bounds = math::Merge(value.Bounds, TriangleBounds(triangle));
        centroidBounds = math::Merge(centroidBounds, {centroid, centroid});
    }
    // Exact capacity was reserved before recursion; these additions cannot allocate.
    static_cast<void>(inoutScene.Nodes.TryAdd(value));
    if (count <= 1)
    {
        return node;
    }
    const SahSplit split = FindSahSplit(inoutScene, value, centroidBounds);
    if (count <= REFERENCE_BVH_MAX_LEAF_TRIANGLES &&
        (split.IsValid == false || SAH_INTERSECTION_COST * float64(count) <= split.Cost))
    {
        return node;
    }
    usize axis = 0;
    uint32 leftCount = 0;
    if (split.IsValid == true && depth + ComputeCeilingLog2(count) < REFERENCE_BVH_MAX_DEPTH)
    {
        // Stable partition by bin keeps the build deterministic.
        axis = split.Axis;
        const float32 minimum = Component(centroidBounds.Min, axis);
        const float32 extent = Component(centroidBounds.Max, axis) - minimum;
        uint32 rightCount = 0;
        for (usize i = first; i < usize(first) + count; ++i)
        {
            const uint32 triangle = inoutScene.Order[i];
            if (ComputeBin(Component(ComputeCentroidSum(inoutScene.Triangles[triangle]), axis), minimum, extent) <
                split.Bin)
            {
                inoutScene.Order[first + leftCount] = triangle;
                ++leftCount;
            }
            else
            {
                scratch[rightCount] = triangle;
                ++rightCount;
            }
        }
        for (uint32 i = 0; i < rightCount; ++i)
        {
            inoutScene.Order[first + leftCount + i] = scratch[i];
        }
    }
    else
    {
        // Balanced median split: each level halves the range, bounding the remaining depth.
        const math::Vector3 extent = value.Bounds.Max - value.Bounds.Min;
        axis = extent.X >= extent.Y && extent.X >= extent.Z ? 0 : extent.Y >= extent.Z ? 1 : 2;
        static_cast<void>(
            StableSort(inoutScene.Order.GetData() + first, scratch, count, TriangleLess{inoutScene, axis}));
        leftCount = count / 2;
    }
    const uint32 left = BuildNode(inoutScene, first, leftCount, depth + 1, scratch);
    const uint32 right = BuildNode(inoutScene, first + leftCount, count - leftCount, depth + 1, scratch);
    inoutScene.Nodes[node].Left = left;
    inoutScene.Nodes[node].Right = right;
    inoutScene.Nodes[node].Count = 0;
    inoutScene.Nodes[node].SplitAxis = static_cast<uint32>(axis);
    return node;
}
struct OracleIntersection final
{
    float64 Distance = 0;
    float32 U = 0, V = 0;
};
bool Intersect(const ReferenceTriangle& triangle,
               const rhi::RayInput& ray,
               float64 limit,
               OracleIntersection& output) noexcept
{
    usize z = 0;
    for (usize axis = 1; axis < 3; ++axis)
    {
        if (math::Abs(ray.Direction[axis]) > math::Abs(ray.Direction[z]))
        {
            z = axis;
        }
    }
    usize x = (z + 1) % 3, y = (x + 1) % 3;
    if (ray.Direction[z] < 0)
    {
        const auto temporary = x;
        x = y;
        y = temporary;
    }
    const auto sx = float64(ray.Direction[x]) / float64(ray.Direction[z]);
    const auto sy = float64(ray.Direction[y]) / float64(ray.Direction[z]);
    const auto sz = 1 / float64(ray.Direction[z]);
    float64 px[3]{}, py[3]{}, pz[3]{};
    for (usize corner = 0; corner < 3; ++corner)
    {
        const auto& p = triangle.Positions[corner];
        const auto dz = float64(Component(p, z)) - float64(ray.Origin[z]);
        px[corner] = (float64(Component(p, x)) - float64(ray.Origin[x])) - sx * dz;
        py[corner] = (float64(Component(p, y)) - float64(ray.Origin[y])) - sy * dz;
        pz[corner] = dz * sz;
    }
    const float64 edge[3]{px[2] * py[1] - py[2] * px[1], px[0] * py[2] - py[0] * px[2], px[1] * py[0] - py[1] * px[0]};
    if ((edge[0] < 0 || edge[1] < 0 || edge[2] < 0) && (edge[0] > 0 || edge[1] > 0 || edge[2] > 0))
    {
        return false;
    }
    const auto determinant = edge[0] + edge[1] + edge[2];
    if (determinant == 0)
    {
        return false;
    }
    const auto distance = (edge[0] * pz[0] + edge[1] * pz[1] + edge[2] * pz[2]) / determinant;
    if (distance < float64(ray.MinDistance) || distance > limit)
    {
        return false;
    }
    output = {distance, static_cast<float32>(edge[1] / determinant), static_cast<float32>(edge[2] / determinant)};
    return true;
}
int32 Address(int32 coordinate, bool repeat, uint32 extent) noexcept
{
    const auto size = static_cast<int32>(extent);
    if (repeat == false)
    {
        return math::Clamp(coordinate, int32{0}, size - 1);
    }
    const auto remainder = coordinate % size;
    return remainder < 0 ? remainder + size : remainder;
}
math::Vector4
Texel(const rhi::TextureCpuContent& texture, const rhi::SamplerDescription& sampler, int32 x, int32 y) noexcept
{
    const auto column = Address(x, sampler.U == rhi::RasterAddress::Repeat, texture.Width);
    const auto row = Address(y, sampler.V == rhi::RasterAddress::Repeat, texture.Height);
    const auto* p = texture.Bytes + usize(row) * texture.RowPitch + usize(column) * 4;
    math::Vector4 value{float32(p[0]) / 255, float32(p[1]) / 255, float32(p[2]) / 255, float32(p[3]) / 255};
    if (texture.Format == rhi::RasterFormat::Rgba8Srgb)
    {
        value.X = math::DecodeSrgb(value.X);
        value.Y = math::DecodeSrgb(value.Y);
        value.Z = math::DecodeSrgb(value.Z);
    }
    return value;
}
rhi::RasterStatus CameraRay(const ReferenceJob& job, float32 x, float32 y, rhi::RayInput& output) noexcept
{
    const auto& d = job.Description;
    const auto nearClip = d.ReverseZ ? 1.0F : 0.0F, farClip = 1 - nearClip;
    const auto near = job.ClipToWorld * math::Vector4{x, y, nearClip, 1};
    const auto far = job.ClipToWorld * math::Vector4{x, y, farClip, 1};
    if (math::IsFinite(near) == false || math::IsFinite(far) == false || near.W == 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const math::Vector3 nearPoint{near.X / near.W, near.Y / near.W, near.Z / near.W};
    const auto origin = d.OrthographicCamera ? nearPoint : d.CameraPosition;
    math::Vector3 direction;
    if (far.W == 0)
    {
        if (d.InfiniteFar == false || d.OrthographicCamera)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        direction = ReferenceNormalize({far.X, far.Y, far.Z});
    }
    else
    {
        direction = ReferenceNormalize(math::Vector3{far.X / far.W, far.Y / far.W, far.Z / far.W} - origin);
    }
    if (math::IsFinite(origin) == false || math::Length(direction) == 0 || math::Abs(origin.X) > MAX_WORLD_COORDINATE ||
        math::Abs(origin.Y) > MAX_WORLD_COORDINATE || math::Abs(origin.Z) > MAX_WORLD_COORDINATE)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    float32 distance = 1;
    if (far.W != 0)
    {
        distance = math::Dot(math::Vector3{far.X / far.W, far.Y / far.W, far.Z / far.W} - origin, direction);
    }
    else if (math::IsEmpty(job.Scene.Bounds) == false)
    {
        float64 maximum = 0;
        for (usize corner = 0; corner < 8; ++corner)
        {
            const auto& b = job.Scene.Bounds;
            const float64 dx = float64((corner & 1) != 0 ? b.Max.X : b.Min.X) - float64(origin.X);
            const float64 dy = float64((corner & 2) != 0 ? b.Max.Y : b.Min.Y) - float64(origin.Y);
            const float64 dz = float64((corner & 4) != 0 ? b.Max.Z : b.Min.Z) - float64(origin.Z);
            maximum = math::Max(maximum, math::Sqrt(dx * dx + dy * dy + dz * dz));
        }
        distance = static_cast<float32>(maximum * 1.000001 + 1);
    }
    auto result = ReferenceRay(origin, direction, distance);
    result.MinDistance = d.OrthographicCamera ? 0 : math::Dot(nearPoint - origin, direction);
    if (math::IsFinite(distance) == false || distance < 0 || result.MinDistance < 0 || result.MinDistance > distance)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
} // namespace
math::Vector3 ComputeReferencePositionError(const math::Affine3& meshToWorld, math::Vector3 positionMesh) noexcept
{
    // Preserve absolute affine products before cancellation. This is the shared
    // FP32 profile allowance, not a platform-specific hardware error guarantee.
    constexpr float32 AFFINE_FP32_ERROR_ALLOWANCE = 1e-6F;
    return {
        (math::Abs(meshToWorld.Columns[0].X * positionMesh.X) + math::Abs(meshToWorld.Columns[1].X * positionMesh.Y) +
         math::Abs(meshToWorld.Columns[2].X * positionMesh.Z) + math::Abs(meshToWorld.Translation.X)) *
            AFFINE_FP32_ERROR_ALLOWANCE,
        (math::Abs(meshToWorld.Columns[0].Y * positionMesh.X) + math::Abs(meshToWorld.Columns[1].Y * positionMesh.Y) +
         math::Abs(meshToWorld.Columns[2].Y * positionMesh.Z) + math::Abs(meshToWorld.Translation.Y)) *
            AFFINE_FP32_ERROR_ALLOWANCE,
        (math::Abs(meshToWorld.Columns[0].Z * positionMesh.X) + math::Abs(meshToWorld.Columns[1].Z * positionMesh.Y) +
         math::Abs(meshToWorld.Columns[2].Z * positionMesh.Z) + math::Abs(meshToWorld.Translation.Z)) *
            AFFINE_FP32_ERROR_ALLOWANCE,
    };
}
rhi::RasterStatus BuildReferenceBvh(ReferenceSceneData& scene) noexcept
{
    const auto count = scene.Triangles.GetSize();
    scene.Bounds = math::EmptyAabb3();
    if (count == 0)
    {
        return rhi::RasterStatus::Ready;
    }
    if (count > (~uint32{0}) / 2)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    Array<uint32> scratch;
    if (scene.Order.TryResize(count) == false || scratch.TryResize(count) == false ||
        scene.Nodes.TryEnsureCapacity(count * 2) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    // Zero-area triangles carry no surface: a ray cannot meaningfully hit one and
    // its geometric normal is undefined. Leave them out of the hierarchy (the
    // static bake keeps them only as empty chart tiles).
    usize surfaceCount = 0;
    for (usize i = 0; i < count; ++i)
    {
        if (scene.Triangles[i].Area > 0)
        {
            scene.Order[surfaceCount] = static_cast<uint32>(i);
            ++surfaceCount;
        }
    }
    if (scene.Order.TryResize(surfaceCount) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    if (surfaceCount == 0)
    {
        return rhi::RasterStatus::Ready;
    }
    static_cast<void>(BuildNode(scene, 0, static_cast<uint32>(surfaceCount), 0, scratch.GetData()));
    scene.Bounds = scene.Nodes[0].Bounds;
    return rhi::RasterStatus::Ready;
}
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering", 4th edition (2023), section 12.6.2 "Power Light Sampler",
// https://pbr-book.org/4ed/Light_Sources/Light_Sampling: choose emitters in
// proportion to emitted power (luminance times area for a diffuse triangle). pbrt
// builds an alias table over every light; this variant uses an integer CDF over
// emitter triangles only, quantized so the bounded random mapper selects
// each emitter with an exactly known probability; it keeps the uniform proposal
// across analytic, emitter and environment categories.
rhi::RasterStatus BuildReferenceEmitterSelection(ReferenceSceneData& inoutScene) noexcept
{
    const usize count = inoutScene.Emitters.GetSize();
    Array<uint32> cdf;
    if (cdf.TryResize(count) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    const auto computeWeight = [&inoutScene](usize emitter) noexcept -> float64 {
        const ReferenceTriangle& triangle = inoutScene.Triangles[inoutScene.Emitters[emitter]];
        const math::Vector3 emission = inoutScene.Instances[triangle.Instance].Surface->Emission;
        const float64 luminance = .2126 * float64(math::Max(emission.X, 0.F)) +
                                  .7152 * float64(math::Max(emission.Y, 0.F)) +
                                  .0722 * float64(math::Max(emission.Z, 0.F));
        const float64 weight = luminance * float64(triangle.Area);
        return math::IsFinite(weight) == true && weight > 0 ? weight : 0;
    };
    float64 total = 0;
    for (usize emitter = 0; emitter < count; ++emitter)
    {
        if (inoutScene.Emitters[emitter] >= inoutScene.Triangles.GetSize() ||
            (emitter != 0 && inoutScene.Emitters[emitter] <= inoutScene.Emitters[emitter - 1]))
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        total += computeWeight(emitter);
    }
    if (math::IsFinite(total) == false)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    uint64 cumulative = 0;
    for (usize emitter = 0; emitter < count; ++emitter)
    {
        const float64 weight = computeWeight(emitter);
        if (weight > 0)
        {
            // Round half up; any nonnegative rounding keeps the probabilities exact for the sampler.
            const float64 scaled = weight / total * float64(REFERENCE_EMITTER_WEIGHT_TOTAL);
            cumulative += math::Max(static_cast<uint64>(math::Floor(scaled + .5)), uint64{1});
        }
        if (cumulative > ~uint32{0})
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        cdf[emitter] = static_cast<uint32>(cumulative);
    }
    inoutScene.EmitterCdf = Move(cdf);
    return rhi::RasterStatus::Ready;
}
usize SelectReferenceEmitter(const ReferenceSceneData& scene, uint32 value) noexcept
{
    LUDUS_ASSERT(scene.EmitterCdf.GetSize() != 0 && value < scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1]);
    usize low = 0;
    usize high = scene.EmitterCdf.GetSize() - 1;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (scene.EmitterCdf[middle] <= value)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}
bool FindReferenceEmitter(const ReferenceSceneData& scene, uint32 triangle, usize& outEmitter) noexcept
{
    usize low = 0;
    usize high = scene.Emitters.GetSize();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (scene.Emitters[middle] < triangle)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == scene.Emitters.GetSize() || scene.Emitters[low] != triangle)
    {
        return false;
    }
    outEmitter = low;
    return true;
}
float32 GetReferenceEmitterProbability(const ReferenceSceneData& scene, usize emitter) noexcept
{
    const usize count = scene.EmitterCdf.GetSize();
    if (emitter >= count || scene.EmitterCdf[count - 1] == 0)
    {
        return 0;
    }
    const uint32 previous = emitter == 0 ? 0 : scene.EmitterCdf[emitter - 1];
    return float32(scene.EmitterCdf[emitter] - previous) / float32(scene.EmitterCdf[count - 1]);
}
bool PackReferenceEmitterSelection(const ReferenceSceneData& scene, BorrowedView<math::Vector4> outWords) noexcept
{
    const usize count = scene.Emitters.GetSize();
    if (scene.EmitterCdf.GetSize() != count || outWords.GetSize() < count)
    {
        return false;
    }
    for (usize emitter = 0; emitter < count; ++emitter)
    {
        std::memcpy(&outWords[emitter].X, &scene.Emitters[emitter], sizeof(uint32));
        std::memcpy(&outWords[emitter].Y, &scene.EmitterCdf[emitter], sizeof(uint32));
    }
    return true;
}
ReferenceEmissiveTriangleSelection SelectReferenceEmissiveTriangle(const ReferenceSceneData& scene,
                                                                   math::RandomKey key,
                                                                   math::RandomAddress address) noexcept
{
    ReferenceEmissiveTriangleSelection result;
    const usize count = scene.EmitterCdf.GetSize();
    if (count == 0 || count != scene.Emitters.GetSize() || scene.EmitterCdf[count - 1] == 0)
    {
        return result;
    }
    uint32 value = 0;
    if (math::TrySampleBounded(key, address, scene.EmitterCdf[count - 1], value) != math::MathStatus::Success)
    {
        return result;
    }
    const usize emitter = SelectReferenceEmitter(scene, value);
    result.TriangleIndex = scene.Emitters[emitter];
    result.SelectionPmf = GetReferenceEmitterProbability(scene, emitter);
    result.HasSample = true;
    return result;
}
float32 EvaluateReferenceEmissiveTriangleSelectionPmf(const ReferenceSceneData& scene, uint32 triangle) noexcept
{
    usize emitter = 0;
    if (scene.EmitterCdf.GetSize() != scene.Emitters.GetSize() ||
        FindReferenceEmitter(scene, triangle, emitter) == false)
    {
        return 0;
    }
    return GetReferenceEmitterProbability(scene, emitter);
}
math::Vector4 SampleReferenceImage(const ReferenceSurfaceData& surface,
                                   const rhi::TextureCpuContent& texture,
                                   math::Vector2 uv) noexcept
{
    if (texture.Bytes == nullptr)
    {
        return {1, 1, 1, 1};
    }
    uv = {uv.X * surface.UvTransform.X + surface.UvTransform.Z, uv.Y * surface.UvTransform.Y + surface.UvTransform.W};
    // Reject arithmetic overflow before floor-to-integer texture addressing.
    if (math::IsFinite(uv) == false)
    {
        return {__builtin_nanf(""), __builtin_nanf(""), __builtin_nanf(""), __builtin_nanf("")};
    }
    uv.X = surface.Sampler.U == rhi::RasterAddress::Repeat ? uv.X - static_cast<float32>(math::Floor(float64(uv.X)))
                                                           : math::Clamp(uv.X, 0.0F, 1.0F);
    uv.Y = surface.Sampler.V == rhi::RasterAddress::Repeat ? uv.Y - static_cast<float32>(math::Floor(float64(uv.Y)))
                                                           : math::Clamp(uv.Y, 0.0F, 1.0F);
    const float32 x = uv.X * float32(texture.Width);
    const float32 y = uv.Y * float32(texture.Height);
    if (surface.Sampler.Filter == rhi::RasterFilter::Nearest)
    {
        return Texel(texture,
                     surface.Sampler,
                     static_cast<int32>(math::Floor(float64(x))),
                     static_cast<int32>(math::Floor(float64(y))));
    }
    const float32 px = x - 0.5F;
    const float32 py = y - 0.5F;
    const int32 ix = static_cast<int32>(math::Floor(float64(px)));
    const int32 iy = static_cast<int32>(math::Floor(float64(py)));
    const float32 tx = px - float32(ix);
    const float32 ty = py - float32(iy);
    const math::Vector4 a =
        Texel(texture, surface.Sampler, ix, iy) * (1 - tx) + Texel(texture, surface.Sampler, ix + 1, iy) * tx;
    const math::Vector4 b =
        Texel(texture, surface.Sampler, ix, iy + 1) * (1 - tx) + Texel(texture, surface.Sampler, ix + 1, iy + 1) * tx;
    return a * (1 - ty) + b * ty;
}
math::Vector4 SampleReferenceTexture(const ReferenceInstance& instance, math::Vector2 uv) noexcept
{
    return SampleReferenceImage(*instance.Surface, instance.Texture, uv);
}
math::Vector3 SampleReferenceEmission(const ReferenceInstance& instance, math::Vector2 uv) noexcept
{
    const math::Vector4 texel = instance.Surface->HasEmissionMap
                                    ? SampleReferenceImage(*instance.Surface, instance.EmissionMap, uv)
                                    : math::Vector4{1, 1, 1, 1};
    return math::Hadamard(instance.Surface->Emission, math::Vector3{texel.X, texel.Y, texel.Z});
}
math::Vector4 InterpolateReferenceColor(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept
{
    const math::Vector4 color = triangle.Colors[0] * (1 - u - v) + triangle.Colors[1] * u + triangle.Colors[2] * v;
    if (math::IsFinite(color) == false)
    {
        return color;
    }
    // MeshDescription::Colors admits linear multipliers in [0,1]. Even constant white can
    // round one ULP above one in a three-term barycentric sum; keep the interpolant in that
    // input domain before BSDF evaluation. Radiance and path throughput remain unbounded.
    return {math::Clamp(color.X, 0.0F, 1.0F),
            math::Clamp(color.Y, 0.0F, 1.0F),
            math::Clamp(color.Z, 0.0F, 1.0F),
            math::Clamp(color.W, 0.0F, 1.0F)};
}
math::Vector4 SampleReferenceSurfaceMap(const ReferenceInstance& instance, math::Vector2 uv) noexcept
{
    return SampleReferenceImage(*instance.Surface, instance.SurfaceMap, uv);
}
math::Vector3 ApplyReferenceNormal(const ReferenceTriangle& triangle,
                                   const ReferenceSurfaceData& surface,
                                   math::Vector3 normalWorld,
                                   math::Vector3 normalTangent,
                                   float32 u,
                                   float32 v) noexcept
{
    const math::Vector4 authored =
        triangle.Tangents[0] * (1 - u - v) + triangle.Tangents[1] * u + triangle.Tangents[2] * v;
    if (surface.AreAuthoredInputsEnabled == true && math::Abs(authored.W) > .5F)
    {
        math::Vector3 tangent{authored.X, authored.Y, authored.Z};
        if (math::TryNormalize(tangent - normalWorld * math::Dot(normalWorld, tangent), tangent) !=
            math::MathStatus::Success)
        {
            return normalWorld;
        }
        const math::Vector3 bitangent = math::Cross(normalWorld, tangent) * (authored.W < 0 ? -1.0F : 1.0F);
        return ReferenceNormalize(tangent * normalTangent.X + bitangent * normalTangent.Y +
                                  normalWorld * normalTangent.Z);
    }
    math::Vector3 edgeUWorld = triangle.Positions[1] - triangle.Positions[0];
    math::Vector3 edgeVWorld = triangle.Positions[2] - triangle.Positions[0];
    math::Vector2 edgeUUv =
        math::Hadamard(triangle.Uvs[1] - triangle.Uvs[0], math::Vector2{surface.UvTransform.X, surface.UvTransform.Y});
    math::Vector2 edgeVUv =
        math::Hadamard(triangle.Uvs[2] - triangle.Uvs[0], math::Vector2{surface.UvTransform.X, surface.UvTransform.Y});
    const float32 uvMagnitude = math::Max(math::Max(math::Abs(edgeUUv.X), math::Abs(edgeUUv.Y)),
                                          math::Max(math::Abs(edgeVUv.X), math::Abs(edgeVUv.Y)));
    const float32 worldMagnitude =
        math::Max(math::Max(math::Max(math::Abs(edgeUWorld.X), math::Abs(edgeUWorld.Y)), math::Abs(edgeUWorld.Z)),
                  math::Max(math::Max(math::Abs(edgeVWorld.X), math::Abs(edgeVWorld.Y)), math::Abs(edgeVWorld.Z)));
    if (uvMagnitude <= 1e-30F || worldMagnitude <= 1e-30F)
    {
        return normalWorld;
    }
    edgeUUv = edgeUUv / uvMagnitude;
    edgeVUv = edgeVUv / uvMagnitude;
    edgeUWorld = edgeUWorld / worldMagnitude;
    edgeVWorld = edgeVWorld / worldMagnitude;
    const float32 determinant = edgeUUv.X * edgeVUv.Y - edgeUUv.Y * edgeVUv.X;
    if (math::Abs(determinant) <= 1e-6F * math::Max(math::Abs(edgeUUv.X * edgeVUv.Y), math::Abs(edgeUUv.Y * edgeVUv.X)))
    {
        return normalWorld;
    }
    math::Vector3 tangentWorld = (edgeUWorld * edgeVUv.Y - edgeVWorld * edgeUUv.Y) / determinant;
    math::Vector3 bitangentWorld = (edgeVWorld * edgeUUv.X - edgeUWorld * edgeVUv.X) / determinant;
    tangentWorld = tangentWorld - normalWorld * math::Dot(normalWorld, tangentWorld);
    if (math::Dot(tangentWorld, tangentWorld) <= 1e-20F)
    {
        return normalWorld;
    }
    tangentWorld = ReferenceNormalize(tangentWorld);
    const float32 handedness = math::Dot(math::Cross(normalWorld, tangentWorld), bitangentWorld) < 0 ? -1.0F : 1.0F;
    bitangentWorld = math::Cross(normalWorld, tangentWorld) * handedness;
    return ReferenceNormalize(tangentWorld * normalTangent.X + bitangentWorld * normalTangent.Y +
                              normalWorld * normalTangent.Z);
}
bool ReferenceOpacity(const ReferenceInstance& instance, math::Vector2 uv, float32 vertexAlpha) noexcept
{
    return instance.Surface->Alpha != MaterialAlpha::Masked ||
           SampleReferenceTexture(instance, uv).W * instance.Surface->Tint.W * vertexAlpha >=
               instance.Surface->AlphaCutoff;
}
rhi::RasterStatus
TraceReferenceOracle(const ReferenceSceneData& scene, const rhi::RayInput& ray, rhi::RayResult& output) noexcept
{
    rhi::RayResult result;
    if (scene.Nodes.IsEmpty())
    {
        output = result;
        return rhi::RasterStatus::Ready;
    }
    uint32 stack[64]{};
    usize pending = 1;
    float64 closest = float64(ray.MaxDistance);
    uint32 chosen = ~uint32{0};
    while (pending != 0)
    {
        const auto& node = scene.Nodes[stack[--pending]];
        math::RayHit box;
        if (math::TryRayAabb({ReferenceOrigin(ray), ReferenceDirection(ray)},
                             node.Bounds,
                             {float64(ray.MinDistance), closest},
                             box) != math::MathStatus::Success)
        {
            return rhi::RasterStatus::Failed;
        }
        if (box.Result == math::HitResult::Miss)
        {
            continue;
        }
        if (node.Count == 0)
        {
            if (pending > 62)
            {
                return rhi::RasterStatus::CapacityExceeded;
            }
            // Visit the child on the ray's near side of the split first, so a closest
            // hit there shortens the interval that culls the far child.
            const bool isReversed = Component(ReferenceDirection(ray), node.SplitAxis) < 0;
            stack[pending++] = isReversed == true ? node.Left : node.Right;
            stack[pending++] = isReversed == true ? node.Right : node.Left;
            continue;
        }
        for (usize i = node.First; i < usize(node.First) + node.Count; ++i)
        {
            const auto index = scene.Order[i];
            const auto& triangle = scene.Triangles[index];
            OracleIntersection intersection;
            if (Intersect(triangle, ray, closest, intersection) == false ||
                (intersection.Distance == closest && index >= chosen))
            {
                continue;
            }
            const auto u = intersection.U, v = intersection.V;
            const auto uv = triangle.Uvs[0] * (1 - u - v) + triangle.Uvs[1] * u + triangle.Uvs[2] * v;
            const ReferenceInstance& instance = scene.Instances[triangle.Instance];
            const float32 alpha = InterpolateReferenceColor(triangle, u, v).W;
            if (instance.Surface->Alpha == MaterialAlpha::Masked &&
                SampleReferenceTexture(instance, uv).W * instance.Surface->Tint.W * alpha <
                    instance.Surface->AlphaCutoff)
            {
                continue;
            }
            closest = intersection.Distance;
            chosen = index;
            result =
                {static_cast<float32>(intersection.Distance), u, v, 0, triangle.Primitive, triangle.Instance, 1, 0};
            if (ray.VisibilityOnly != 0)
            {
                output = result;
                return rhi::RasterStatus::Ready;
            }
        }
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus PrepareReferenceCamera(ReferenceJob& job) noexcept
{
    const auto& d = job.Description;
    if (math::IsFinite(d.WorldToClip) == false || math::IsFinite(d.CameraPosition) == false ||
        math::TryInverse(d.WorldToClip, job.ClipToWorld) != math::MathStatus::Success ||
        (d.InfiniteFar && (d.ReverseZ == false || d.OrthographicCamera)))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    if (d.OrthographicCamera == false)
    {
        const auto eye = d.WorldToClip * math::Vector4{d.CameraPosition.X, d.CameraPosition.Y, d.CameraPosition.Z, 1};
        if (math::Abs(eye.W) > 1e-5F)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
    }
    rhi::RayInput ray;
    for (usize corner = 0; corner < 4; ++corner)
    {
        const auto status = CameraRay(job, (corner & 1) != 0 ? 1.0F : -1.0F, (corner & 2) != 0 ? 1.0F : -1.0F, ray);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus GenerateReferencePrimary(const ReferenceJob& job, ReferencePath& path, rhi::RayInput& output) noexcept
{
    const auto& d = job.Description;
    const auto x = path.Pixel % d.Width, y = path.Pixel / d.Width;
    const auto jitterX = d.CenterRays ? 0.5F : ReferenceRandom(job, path, 0);
    const auto jitterY = d.CenterRays ? 0.5F : ReferenceRandom(job, path, 1);
    return CameraRay(job,
                     2 * (float32(x) + jitterX) / float32(d.Width) - 1,
                     1 - 2 * (float32(y) + jitterY) / float32(d.Height),
                     output);
}
} // namespace ludus::graphics::renderer::internal
