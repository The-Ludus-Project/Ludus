#pragma once

// Shared triangle-mesh fixtures for the PhysicsCollision tests.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::physics::collision::test
{
using foundation::Array;
using foundation::BorrowedView;
using foundation::StaticArray;
using foundation::uint64;
using foundation::math::RandomStream;
using foundation::math::Vector3;
using foundation::math::Vector3d;

inline constexpr usize QUAD_INDEX_COUNT = 6;

/// Owned positions and indices that a test can edit before cooking.
struct MeshData final
{
    Array<Vector3> Positions;
    Array<uint32> Indices;

    [[nodiscard]] TriangleMeshSource GetSource() const noexcept
    {
        return
        {
            .PositionsMesh = BorrowedView<const Vector3>(Positions.GetData(), Positions.GetSize()),
            .Indices = BorrowedView<const uint32>(Indices.GetData(), Indices.GetSize()),
        };
    }

    void AddTriangle(Vector3 first, Vector3 second, Vector3 third)
    {
        const uint32 base = static_cast<uint32>(Positions.GetSize());
        Positions.Add(first);
        Positions.Add(second);
        Positions.Add(third);
        Indices.Add(base);
        Indices.Add(base + 1);
        Indices.Add(base + 2);
    }
};

/// Unit floor quad [0, 1] x [0, 1] at y = height, wound to face +y. Triangle 0 is (0,0)-(0,1)-(1,1), triangle 1 is
/// (0,0)-(1,1)-(1,0) in (x, z); they share the diagonal x = z.
inline void AddFloorQuad(MeshData& inoutMesh, float32 height = 0.0f)
{
    const uint32 base = static_cast<uint32>(inoutMesh.Positions.GetSize());
    inoutMesh.Positions.Add({0.0f, height, 0.0f});
    inoutMesh.Positions.Add({0.0f, height, 1.0f});
    inoutMesh.Positions.Add({1.0f, height, 1.0f});
    inoutMesh.Positions.Add({1.0f, height, 0.0f});
    const StaticArray<uint32, QUAD_INDEX_COUNT> quad{0, 1, 2, 0, 2, 3};
    for (const uint32 corner : quad)
    {
        inoutMesh.Indices.Add(base + corner);
    }
}

/// Grid of cells x cells quads over [0, cells] in x and z, facing +y. Heights come from the stream when supplied.
inline MeshData MakeGrid(uint32 cells, RandomStream* heightsOrNull = nullptr)
{
    MeshData mesh;
    const uint32 side = cells + 1;
    for (uint32 z = 0; z < side; ++z)
    {
        for (uint32 x = 0; x < side; ++x)
        {
            const float32 height = heightsOrNull == nullptr ? 0.0f : heightsOrNull->NextFloat01() - 0.5f;
            mesh.Positions.Add({static_cast<float32>(x), height, static_cast<float32>(z)});
        }
    }
    for (uint32 z = 0; z < cells; ++z)
    {
        for (uint32 x = 0; x < cells; ++x)
        {
            const uint32 corner = z * side + x;
            const StaticArray<uint32, QUAD_INDEX_COUNT> cell{corner,
                                                             corner + side,
                                                             corner + side + 1,
                                                             corner,
                                                             corner + side + 1,
                                                             corner + 1};
            for (const uint32 index : cell)
            {
                mesh.Indices.Add(index);
            }
        }
    }
    return mesh;
}

inline Vector3d Widen(Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

inline float32 NextInRange(RandomStream& inoutRandom, float32 minimum, float32 maximum)
{
    return minimum + (maximum - minimum) * inoutRandom.NextFloat01();
}

inline Vector3 NextPoint(RandomStream& inoutRandom, float32 extent)
{
    return {NextInRange(inoutRandom, -extent, extent),
            NextInRange(inoutRandom, -extent, extent),
            NextInRange(inoutRandom, -extent, extent)};
}

/// Seeded soup of possibly intersecting triangles of varied size inside [-extent, extent]^3.
/// Parameters of a seeded random triangle soup.
struct SoupSettings final
{
    uint64 Seed = 1;
    uint32 TriangleCount = 0;
    float32 ExtentMeters = 1.0f;
};

inline MeshData MakeTriangleSoup(const SoupSettings& settings)
{
    const uint64 seed = settings.Seed;
    const uint32 count = settings.TriangleCount;
    const float32 extent = settings.ExtentMeters;
    RandomStream random;
    (void)random.TryReseed(seed, 0);
    MeshData mesh;
    while (mesh.Indices.GetSize() / 3 < count)
    {
        const Vector3 center = NextPoint(random, extent);
        const float32 size = NextInRange(random, 0.05f, 3.0f);
        const Vector3 first = center + NextPoint(random, size);
        const Vector3 second = center + NextPoint(random, size);
        const Vector3 third = center + NextPoint(random, size);
        const Vector3d firstEdge = Widen(second) - Widen(first);
        const Vector3d secondEdge = Widen(third) - Widen(first);
        // Keep the soup free of degenerate triangles so that every triangle is admitted.
        if (foundation::math::LengthSquared(foundation::math::Cross(firstEdge, secondEdge)) < 1.0e-6)
        {
            continue;
        }
        mesh.AddTriangle(first, second, third);
    }
    return mesh;
}

/// Unit vector from a seeded stream, uniform enough for traversal coverage.
inline Vector3d NextDirection(RandomStream& inoutRandom)
{
    for (;;)
    {
        const Vector3d candidate{static_cast<float64>(NextInRange(inoutRandom, -1.0f, 1.0f)),
                                 static_cast<float64>(NextInRange(inoutRandom, -1.0f, 1.0f)),
                                 static_cast<float64>(NextInRange(inoutRandom, -1.0f, 1.0f))};
        const float64 length = foundation::math::Length(candidate);
        if (length > 0.1 && length <= 1.0)
        {
            return candidate * (1.0 / length);
        }
    }
}

inline TriangleMesh CookOrFail(const MeshData& data, const TriangleMeshCookSettings& settings = {})
{
    TriangleMesh mesh;
    const TriangleMeshCookResult result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    REQUIRE(result.Result == Status::Success);
    return mesh;
}
} // namespace ludus::physics::collision::test
