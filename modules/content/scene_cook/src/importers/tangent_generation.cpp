#include "tangent_generation.h"

#include "front_end_support.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

extern "C" {
#include "mikktspace.h"
}

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace math = foundation::math;
using foundation::core::int32;

constexpr usize TANGENT_COMPONENTS = IR_TANGENT_COMPONENT_COUNT;
/// Index of the bitangent sign w within an xyzw tangent.
constexpr usize TANGENT_SIGN_COMPONENT = 3;
/// MikkTSpace m_getNumVerticesOfFace result: every IR face is a triangle.
constexpr int32 MIKKTSPACE_FACE_CORNERS = static_cast<int32>(IR_TRIANGLE_CORNER_COUNT);
/// The reference counts corners in int.
constexpr usize MAX_MIKKTSPACE_CORNERS = usize{0x7fffffff} / TANGENT_COMPONENTS;

/// Mesh data visible to the reference callbacks.
struct MikkMesh final
{
    const IrMesh& Mesh;
    const Array<float32>& Normals;
    Array<float32>& CornerTangents;
};

[[nodiscard]] const MikkMesh& GetMesh(const SMikkTSpaceContext* context) noexcept
{
    return *static_cast<const MikkMesh*>(context->m_pUserData);
}

[[nodiscard]] usize GetVertex(const SMikkTSpaceContext* context, int32 face, int32 corner) noexcept
{
    return GetMesh(context)
        .Mesh.Indices[static_cast<usize>(face) * IR_TRIANGLE_CORNER_COUNT + static_cast<usize>(corner)];
}

int32 GetFaceCount(const SMikkTSpaceContext* context)
{
    return static_cast<int32>(GetMesh(context).Mesh.Indices.GetSize() / IR_TRIANGLE_CORNER_COUNT);
}

int32 GetFaceCornerCount(const SMikkTSpaceContext* /*context*/, const int32 /*face*/)
{
    return MIKKTSPACE_FACE_CORNERS;
}

void GetPosition(const SMikkTSpaceContext* context, float32 outPosition[], const int32 face, const int32 corner)
{
    const float32* source =
        GetMesh(context).Mesh.Positions.GetData() + GetVertex(context, face, corner) * IR_VECTOR_COMPONENT_COUNT;
    for (usize k = 0; k < IR_VECTOR_COMPONENT_COUNT; ++k)
    {
        outPosition[k] = source[k];
    }
}

void GetNormal(const SMikkTSpaceContext* context, float32 outNormal[], const int32 face, const int32 corner)
{
    const float32* source =
        GetMesh(context).Normals.GetData() + GetVertex(context, face, corner) * IR_VECTOR_COMPONENT_COUNT;
    for (usize k = 0; k < IR_VECTOR_COMPONENT_COUNT; ++k)
    {
        outNormal[k] = source[k];
    }
}

// IR uvs run top-down; the reference sees the OpenGL convention (v up) used by
// glTF's +Y-up normal maps and by OBJ/Blender bakers, so bitangent signs match.
void GetTexCoord(const SMikkTSpaceContext* context, float32 outUv[], const int32 face, const int32 corner)
{
    const float32* source = GetMesh(context).Mesh.Uvs.GetData() + GetVertex(context, face, corner) * 2;
    outUv[0] = source[0];
    outUv[1] = 1.0F - source[1];
}

// The parameter list is MikkTSpace's m_setTSpaceBasic callback signature.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void SetTangent(const SMikkTSpaceContext* context,
                const float32 tangent[],
                const float32 sign,
                const int32 face,
                const int32 corner)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    float32* target =
        GetMesh(context).CornerTangents.GetData() +
        (static_cast<usize>(face) * IR_TRIANGLE_CORNER_COUNT + static_cast<usize>(corner)) * TANGENT_COMPONENTS;
    target[0] = tangent[0];
    target[1] = tangent[1];
    target[2] = tangent[2];
    target[TANGENT_SIGN_COMPONENT] = sign;
}

/// Normalised area-weighted vertex normals; (0, 1, 0) where the weights cancel.
[[nodiscard]] bool ComputeNormals(const IrMesh& mesh, Array<float32>& outNormals) noexcept
{
    const usize vertexCount = mesh.GetVertexCount();
    Array<float64> sums;
    if (sums.TryResize(vertexCount * IR_VECTOR_COMPONENT_COUNT, 0.0) == false ||
        outNormals.TryResize(vertexCount * IR_VECTOR_COMPONENT_COUNT) == false)
    {
        return false;
    }
    for (usize i = 0; i + 2 < mesh.Indices.GetSize(); i += IR_TRIANGLE_CORNER_COUNT)
    {
        math::Vector3d points[IR_TRIANGLE_CORNER_COUNT];
        for (usize c = 0; c < IR_TRIANGLE_CORNER_COUNT; ++c)
        {
            const float32* p = mesh.Positions.GetData() + usize{mesh.Indices[i + c]} * IR_VECTOR_COMPONENT_COUNT;
            points[c] = {static_cast<float64>(p[0]), static_cast<float64>(p[1]), static_cast<float64>(p[2])};
        }
        const math::Vector3d face = math::Cross(points[1] - points[0], points[2] - points[0]);
        for (usize c = 0; c < IR_TRIANGLE_CORNER_COUNT; ++c)
        {
            float64* sum = sums.GetData() + usize{mesh.Indices[i + c]} * IR_VECTOR_COMPONENT_COUNT;
            sum[0] += face.X;
            sum[1] += face.Y;
            sum[2] += face.Z;
        }
    }
    for (usize v = 0; v < vertexCount; ++v)
    {
        const usize firstComponent = v * IR_VECTOR_COMPONENT_COUNT;
        const math::Vector3d sum{sums[firstComponent], sums[firstComponent + 1], sums[firstComponent + 2]};
        const float64 length = math::Length(sum);
        const math::Vector3d normal =
            length > 0.0 && math::IsFinite(length) == true ? sum * (1.0 / length) : math::Vector3d{0.0, 1.0, 0.0};
        outNormals[firstComponent] = static_cast<float32>(normal.X);
        outNormals[firstComponent + 1] = static_cast<float32>(normal.Y);
        outNormals[firstComponent + 2] = static_cast<float32>(normal.Z);
    }
    return true;
}

[[nodiscard]] bool AreTangentsEqual(const float32* left, const float32* right) noexcept
{
    for (usize k = 0; k < TANGENT_COMPONENTS; ++k)
    {
        if (left[k] != right[k])
        {
            return false;
        }
    }
    return true;
}

/// Append a copy of vertex's attributes to every per-vertex array of mesh.
[[nodiscard]] bool DuplicateVertex(IrMesh& inoutMesh, usize vertex) noexcept
{
    const auto copy = [&](Array<float32>& inoutValues, usize width) noexcept -> bool {
        if (inoutValues.IsEmpty() == true)
        {
            return true;
        }
        if (inoutValues.TryEnsureCapacity(inoutValues.GetSize() + width) == false)
        {
            return false;
        }
        for (usize k = 0; k < width; ++k)
        {
            (void)inoutValues.TryAdd(inoutValues[vertex * width + k]);
        }
        return true;
    };
    return copy(inoutMesh.Positions, IR_VECTOR_COMPONENT_COUNT) == true &&
           copy(inoutMesh.Normals, IR_VECTOR_COMPONENT_COUNT) == true && copy(inoutMesh.Uvs, 2) == true;
}
} // namespace

Status GenerateTangents(IrMesh& inoutMesh) noexcept
{
    const usize vertexCount = inoutMesh.GetVertexCount();
    const usize cornerCount = inoutMesh.Indices.GetSize();
    if (inoutMesh.Uvs.GetSize() != vertexCount * 2 || cornerCount == 0 || cornerCount % IR_TRIANGLE_CORNER_COUNT != 0 ||
        cornerCount > MAX_MIKKTSPACE_CORNERS)
    {
        return Status::Invalid;
    }
    IrMesh candidate;
    if (candidate.Positions.TryAddRange(inoutMesh.Positions.AsSpan()) == false ||
        candidate.Uvs.TryAddRange(inoutMesh.Uvs.AsSpan()) == false ||
        candidate.Indices.TryAddRange(inoutMesh.Indices.AsSpan()) == false)
    {
        return Status::OutOfMemory;
    }
    if (inoutMesh.Normals.GetSize() == vertexCount * IR_VECTOR_COMPONENT_COUNT)
    {
        if (candidate.Normals.TryAddRange(inoutMesh.Normals.AsSpan()) == false)
        {
            return Status::OutOfMemory;
        }
    }
    else if (ComputeNormals(inoutMesh, candidate.Normals) == false)
    {
        return Status::OutOfMemory;
    }
    Array<float32> cornerTangents;
    if (cornerTangents.TryResize(cornerCount * TANGENT_COMPONENTS, 0.0F) == false)
    {
        return Status::OutOfMemory;
    }
    MikkMesh mikkMesh{candidate, candidate.Normals, cornerTangents};
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = GetFaceCount;
    callbacks.m_getNumVerticesOfFace = GetFaceCornerCount;
    callbacks.m_getPosition = GetPosition;
    callbacks.m_getNormal = GetNormal;
    callbacks.m_getTexCoord = GetTexCoord;
    callbacks.m_setTSpaceBasic = SetTangent;
    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &mikkMesh;
    if (genTangSpaceDefault(&context) == 0)
    {
        // The reference fails only when its scratch allocations fail.
        return Status::OutOfMemory;
    }
    // Give each distinct corner frame of a vertex its own vertex.
    Array<uint32> nextSplit;
    Array<uint8> isAssigned;
    if (candidate.Tangents.TryResize(vertexCount * TANGENT_COMPONENTS, 0.0F) == false ||
        nextSplit.TryResize(vertexCount, IR_NONE) == false || isAssigned.TryResize(vertexCount, uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        const float32* tangent = cornerTangents.GetData() + corner * TANGENT_COMPONENTS;
        uint32 vertex = candidate.Indices[corner];
        const usize original = vertex;
        while (true)
        {
            float32* stored = candidate.Tangents.GetData() + usize{vertex} * TANGENT_COMPONENTS;
            if (isAssigned[vertex] == 0)
            {
                for (usize k = 0; k < TANGENT_COMPONENTS; ++k)
                {
                    stored[k] = tangent[k];
                }
                isAssigned[vertex] = 1;
                break;
            }
            if (AreTangentsEqual(stored, tangent) == true)
            {
                break;
            }
            if (nextSplit[vertex] != IR_NONE)
            {
                vertex = nextSplit[vertex];
                continue;
            }
            const uint32 split = static_cast<uint32>(candidate.GetVertexCount());
            if (DuplicateVertex(candidate, original) == false ||
                candidate.Tangents.TryAddRange(std::span<const float32>(tangent, TANGENT_COMPONENTS)) == false ||
                nextSplit.TryAdd(IR_NONE) == false || isAssigned.TryAdd(uint8{1}) == false)
            {
                return Status::OutOfMemory;
            }
            nextSplit[vertex] = split;
            vertex = split;
            break;
        }
        candidate.Indices[corner] = vertex;
    }
    inoutMesh = static_cast<IrMesh&&>(candidate);
    return Status::Ok;
}

Status GenerateMissingTangents(SceneIr& inoutScene, DiagnosticLog& inoutLog, const Cancellation* cancelOrNull) noexcept
{
    Array<uint8> isVisited;
    if (isVisited.TryResize(inoutScene.Meshes.GetSize(), uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    uint32 generated = 0;
    for (const IrShape& shape : inoutScene.Shapes)
    {
        if (shape.Material == IR_NONE || shape.Mesh == IR_NONE || isVisited[shape.Mesh] != 0)
        {
            continue;
        }
        const IrMaterial& material = inoutScene.Materials[shape.Material];
        IrMesh& mesh = inoutScene.Meshes[shape.Mesh];
        if (material.NormalTexture == IR_NONE || material.NormalTextureKind != IrNormalTextureKind::TangentSpace ||
            mesh.Tangents.IsEmpty() == false)
        {
            continue;
        }
        isVisited[shape.Mesh] = 1;
        if (IsCancelled(cancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        const Status status = GenerateTangents(mesh);
        if (status == Status::OutOfMemory)
        {
            return status;
        }
        if (status != Status::Ok)
        {
            const std::string_view file = shape.SourceFile < inoutScene.SourceFiles.GetSize()
                                              ? inoutScene.SourceFiles[shape.SourceFile].GetView()
                                              : std::string_view();
            (void)inoutLog.Add(DiagnosticSeverity::Warning,
                               codes::TANGENTS_GENERATED,
                               file,
                               shape.Line,
                               "normal-mapped mesh without texture coordinates; no tangents generated");
            continue;
        }
        ++generated;
    }
    if (generated != 0)
    {
        MessageBuilder message;
        (void)message.Append("MikkTSpace tangents generated for ")
            .AppendUnsigned(generated)
            .Append(" normal-mapped meshes; LUSCN v5 does not store tangents yet");
        (void)inoutLog.Add(DiagnosticSeverity::Info, codes::TANGENTS_GENERATED, {}, 0, message.GetView());
    }
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::importers
