#include "shape_tessellation.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
[[nodiscard]] bool AddVertex(IrMesh& inoutMesh,
                             float64 x,
                             float64 y,
                             float64 z,
                             float64 normalX,
                             float64 normalY,
                             float64 normalZ,
                             float64 u,
                             float64 v) noexcept
{
    return inoutMesh.Positions.TryAdd(static_cast<float32>(x)) == true &&
           inoutMesh.Positions.TryAdd(static_cast<float32>(y)) == true &&
           inoutMesh.Positions.TryAdd(static_cast<float32>(z)) == true &&
           inoutMesh.Normals.TryAdd(static_cast<float32>(normalX)) == true &&
           inoutMesh.Normals.TryAdd(static_cast<float32>(normalY)) == true &&
           inoutMesh.Normals.TryAdd(static_cast<float32>(normalZ)) == true &&
           inoutMesh.Uvs.TryAdd(static_cast<float32>(u)) == true &&
           inoutMesh.Uvs.TryAdd(static_cast<float32>(1.0 - v)) == true;
}

[[nodiscard]] bool AddTriangle(IrMesh& inoutMesh, uint32 a, uint32 b, uint32 c) noexcept
{
    return inoutMesh.Indices.TryAdd(a) == true && inoutMesh.Indices.TryAdd(b) == true &&
           inoutMesh.Indices.TryAdd(c) == true;
}

/// Copy P, N and uv; validates counts. Indices are added by the caller.
[[nodiscard]] ShapeResult
CopyVertices(const ShapeArguments& arguments, IrMesh& outMesh, std::string_view& outMessage) noexcept
{
    const usize count = arguments.Positions.GetSize() / 3;
    if (count == 0 || arguments.Positions.GetSize() % 3 != 0 || count > 0x7fffffffU)
    {
        outMessage = "P must hold a positive multiple of three values";
        return ShapeResult::Invalid;
    }
    const bool hasNormals = arguments.Normals.GetSize() == count * 3;
    const bool hasUvs = arguments.Uvs.GetSize() == count * 2;
    if ((arguments.Normals.IsEmpty() == false && hasNormals == false) ||
        (arguments.Uvs.IsEmpty() == false && hasUvs == false))
    {
        outMessage = "N or uv count does not match P";
        return ShapeResult::Invalid;
    }
    if (outMesh.Positions.TryResize(count * 3) == false ||
        (hasNormals == true && outMesh.Normals.TryResize(count * 3) == false) ||
        (hasUvs == true && outMesh.Uvs.TryResize(count * 2) == false))
    {
        return ShapeResult::OutOfMemory;
    }
    for (usize i = 0; i < count * 3; ++i)
    {
        if (math::IsFinite(arguments.Positions[i]) == false)
        {
            outMessage = "P is not finite";
            return ShapeResult::Invalid;
        }
        outMesh.Positions[i] = static_cast<float32>(arguments.Positions[i]);
        if (hasNormals == true)
        {
            outMesh.Normals[i] = static_cast<float32>(arguments.Normals[i]);
        }
    }
    for (usize i = 0; hasUvs == true && i < count; ++i)
    {
        outMesh.Uvs[i * 2] = static_cast<float32>(arguments.Uvs[i * 2]);
        outMesh.Uvs[i * 2 + 1] = static_cast<float32>(1.0 - arguments.Uvs[i * 2 + 1]);
    }
    return ShapeResult::Ok;
}

[[nodiscard]] bool ReadIndex(const ShapeArguments& arguments, const IrMesh& mesh, usize at, uint32& outIndex) noexcept
{
    const usize vertexCount = mesh.GetVertexCount();
    const float64 value = arguments.Indices[at];
    if (value < 0.0 || value >= static_cast<float64>(vertexCount) || math::Floor(value) != value)
    {
        return false;
    }
    outIndex = static_cast<uint32>(value);
    return true;
}

[[nodiscard]] ShapeResult BuildIndexedMesh(const ShapeArguments& arguments,
                                           usize cornersPerFace,
                                           IrMesh& outMesh,
                                           std::string_view& outMessage) noexcept
{
    const ShapeResult copied = CopyVertices(arguments, outMesh, outMessage);
    if (copied != ShapeResult::Ok)
    {
        return copied;
    }
    const usize vertexCount = outMesh.GetVertexCount();
    if (arguments.Indices.IsEmpty() == true)
    {
        if (vertexCount != cornersPerFace)
        {
            outMessage = "indices are required";
            return ShapeResult::Invalid;
        }
        if (cornersPerFace == 3)
        {
            return AddTriangle(outMesh, 0, 1, 2) == true ? ShapeResult::Ok : ShapeResult::OutOfMemory;
        }
        return AddTriangle(outMesh, 0, 1, 3) == true && AddTriangle(outMesh, 0, 3, 2) == true
                   ? ShapeResult::Ok
                   : ShapeResult::OutOfMemory;
    }
    if (arguments.Indices.GetSize() % cornersPerFace != 0)
    {
        outMessage = "index count is not a multiple of the face size";
        return ShapeResult::Invalid;
    }
    for (usize face = 0; face < arguments.Indices.GetSize(); face += cornersPerFace)
    {
        uint32 corners[4] = {0, 0, 0, 0};
        for (usize k = 0; k < cornersPerFace; ++k)
        {
            if (ReadIndex(arguments, outMesh, face + k, corners[k]) == false)
            {
                outMessage = "index out of range";
                return ShapeResult::Invalid;
            }
        }
        // Bilinear patches order corners p00 p10 p01 p11.
        const bool isAdded = cornersPerFace == 3 ? AddTriangle(outMesh, corners[0], corners[1], corners[2])
                                                 : AddTriangle(outMesh, corners[0], corners[1], corners[3]) == true &&
                                                       AddTriangle(outMesh, corners[0], corners[3], corners[2]) == true;
        if (isAdded == false)
        {
            return ShapeResult::OutOfMemory;
        }
    }
    return ShapeResult::Ok;
}

[[nodiscard]] ShapeResult
BuildDisk(const ShapeArguments& arguments, IrMesh& outMesh, std::string_view& outMessage) noexcept
{
    const float64 outer = arguments.Radius;
    const float64 inner = arguments.InnerRadius;
    if (outer <= 0.0 || inner < 0.0 || inner >= outer || math::IsFinite(arguments.Height) == false)
    {
        outMessage = "disk radii are invalid";
        return ShapeResult::Invalid;
    }
    const float64 phiMax = math::DegreesToRadians(math::Clamp(arguments.PhiMaxDegrees, 0.0, 360.0));
    if (phiMax <= 0.0)
    {
        outMessage = "disk phimax is not positive";
        return ShapeResult::Invalid;
    }
    for (uint32 segment = 0; segment <= TESSELLATION_SEGMENTS; ++segment)
    {
        const float64 phi = phiMax * static_cast<float64>(segment) / TESSELLATION_SEGMENTS;
        const float64 cosine = math::Cos(phi);
        const float64 sine = math::Sin(phi);
        const float64 u = static_cast<float64>(segment) / TESSELLATION_SEGMENTS;
        if (AddVertex(outMesh, outer * cosine, outer * sine, arguments.Height, 0.0, 0.0, 1.0, u, 0.0) == false ||
            AddVertex(outMesh, inner * cosine, inner * sine, arguments.Height, 0.0, 0.0, 1.0, u, 1.0) == false)
        {
            return ShapeResult::OutOfMemory;
        }
    }
    for (uint32 segment = 0; segment < TESSELLATION_SEGMENTS; ++segment)
    {
        const uint32 base = segment * 2;
        if (AddTriangle(outMesh, base, base + 2, base + 1) == false ||
            AddTriangle(outMesh, base + 1, base + 2, base + 3) == false)
        {
            return ShapeResult::OutOfMemory;
        }
    }
    return ShapeResult::Ok;
}

[[nodiscard]] ShapeResult
BuildSphere(const ShapeArguments& arguments, IrMesh& outMesh, std::string_view& outMessage) noexcept
{
    const float64 radius = arguments.Radius;
    if (radius <= 0.0 || math::IsFinite(radius) == false)
    {
        outMessage = "sphere radius is invalid";
        return ShapeResult::Invalid;
    }
    // Clip like pbrt: z in [zmin, zmax] and phi in [0, phimax]. Rings are spaced
    // evenly in z (equal-area bands), which also makes clipping exact.
    const float64 zTop = math::Clamp(arguments.ZMax / radius, -1.0, 1.0);
    const float64 zBottom = math::Clamp(arguments.ZMin / radius, -1.0, 1.0);
    const float64 phiMax = math::DegreesToRadians(math::Clamp(arguments.PhiMaxDegrees, 0.0, 360.0));
    if (zTop <= zBottom || phiMax <= 0.0)
    {
        outMessage = "sphere clipping leaves no surface";
        return ShapeResult::Invalid;
    }
    for (uint32 ring = 0; ring <= TESSELLATION_RINGS; ++ring)
    {
        const float64 z = zTop + (zBottom - zTop) * static_cast<float64>(ring) / TESSELLATION_RINGS;
        const float64 ringRadius = math::Sqrt(math::Max(0.0, 1.0 - z * z));
        for (uint32 segment = 0; segment <= TESSELLATION_SEGMENTS; ++segment)
        {
            const float64 phi = phiMax * static_cast<float64>(segment) / TESSELLATION_SEGMENTS;
            const float64 x = ringRadius * math::Cos(phi);
            const float64 y = ringRadius * math::Sin(phi);
            if (AddVertex(outMesh,
                          x * radius,
                          y * radius,
                          z * radius,
                          x,
                          y,
                          z,
                          static_cast<float64>(segment) / TESSELLATION_SEGMENTS,
                          static_cast<float64>(ring) / TESSELLATION_RINGS) == false)
            {
                return ShapeResult::OutOfMemory;
            }
        }
    }
    const uint32 stride = TESSELLATION_SEGMENTS + 1;
    for (uint32 ring = 0; ring < TESSELLATION_RINGS; ++ring)
    {
        for (uint32 segment = 0; segment < TESSELLATION_SEGMENTS; ++segment)
        {
            const uint32 a = ring * stride + segment;
            const uint32 b = a + stride;
            if (AddTriangle(outMesh, a, b, a + 1) == false || AddTriangle(outMesh, a + 1, b, b + 1) == false)
            {
                return ShapeResult::OutOfMemory;
            }
        }
    }
    return ShapeResult::Ok;
}
} // namespace

Status ApplyDefaultTriangleUvs(IrMesh& inoutMesh) noexcept
{
    if (inoutMesh.Uvs.IsEmpty() == false)
    {
        return Status::Ok;
    }
    const usize cornerCount = inoutMesh.Indices.GetSize();
    const usize vertexCount = inoutMesh.GetVertexCount();
    usize vectorComponents = 0;
    usize uvComponents = 0;
    uint32 sourceVertices = 0;
    if (cornerCount == 0 || cornerCount % IR_TRIANGLE_CORNER_COUNT != 0 || cornerCount > IR_NONE ||
        inoutMesh.Positions.GetSize() % IR_VECTOR_COMPONENT_COUNT != 0 ||
        (inoutMesh.Normals.IsEmpty() == false && inoutMesh.Normals.GetSize() != inoutMesh.Positions.GetSize()) ||
        inoutMesh.Tangents.IsEmpty() == false || foundation::TryIntegerCast(vertexCount, sourceVertices) == false ||
        foundation::TryMultiply(cornerCount, IR_VECTOR_COMPONENT_COUNT, vectorComponents) == false ||
        foundation::TryMultiply(cornerCount, usize{2}, uvComponents) == false)
    {
        return Status::Invalid;
    }
    IrMesh candidate;
    candidate.SourceVertexCount = inoutMesh.SourceVertexCount != 0 ? inoutMesh.SourceVertexCount : sourceVertices;
    const bool hasNormals = inoutMesh.Normals.IsEmpty() == false;
    if (candidate.Positions.TryResize(vectorComponents) == false || candidate.Uvs.TryResize(uvComponents) == false ||
        candidate.Indices.TryResize(cornerCount) == false ||
        (hasNormals == true && candidate.Normals.TryResize(vectorComponents) == false))
    {
        return Status::OutOfMemory;
    }
    // Thanks to Pharr, Jakob and Humphreys, pbrt-v4 Triangle::InteractionFromIntersection
    // (https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/shapes.h): absent mesh UVs
    // mean (0,0), (1,0), (1,1) per triangle. IR image coordinates run top-down.
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        const usize vertex = inoutMesh.Indices[corner];
        if (vertex >= vertexCount)
        {
            return Status::Invalid;
        }
        for (usize k = 0; k < IR_VECTOR_COMPONENT_COUNT; ++k)
        {
            candidate.Positions[corner * IR_VECTOR_COMPONENT_COUNT + k] =
                inoutMesh.Positions[vertex * IR_VECTOR_COMPONENT_COUNT + k];
            if (hasNormals == true)
            {
                candidate.Normals[corner * IR_VECTOR_COMPONENT_COUNT + k] =
                    inoutMesh.Normals[vertex * IR_VECTOR_COMPONENT_COUNT + k];
            }
        }
        const usize localCorner = corner % IR_TRIANGLE_CORNER_COUNT;
        candidate.Uvs[corner * 2] = localCorner == 0 ? 0.0F : 1.0F;
        candidate.Uvs[corner * 2 + 1] = localCorner == IR_TRIANGLE_CORNER_COUNT - 1 ? 0.0F : 1.0F;
        candidate.Indices[corner] = static_cast<uint32>(corner);
    }
    inoutMesh = static_cast<IrMesh&&>(candidate);
    return Status::Ok;
}

ShapeResult TessellateShape(std::string_view type,
                            const ShapeArguments& arguments,
                            IrMesh& outMesh,
                            std::string_view& outMessage) noexcept
{
    outMesh = IrMesh{};
    if (type == "trianglemesh")
    {
        return BuildIndexedMesh(arguments, 3, outMesh, outMessage);
    }
    if (type == "bilinearmesh")
    {
        return BuildIndexedMesh(arguments, 4, outMesh, outMessage);
    }
    if (type == "loopsubdiv")
    {
        const ShapeResult result = BuildIndexedMesh(arguments, 3, outMesh, outMessage);
        return result == ShapeResult::Ok ? ShapeResult::Approximated : result;
    }
    if (type == "disk")
    {
        return BuildDisk(arguments, outMesh, outMessage);
    }
    if (type == "sphere")
    {
        return BuildSphere(arguments, outMesh, outMessage);
    }
    return ShapeResult::Unsupported;
}
} // namespace ludus::content::scene_cook::importers
