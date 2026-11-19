#include "fixture.h"

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/patch.hpp>
#include <ludus/geometry/content/geometry_content.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>

namespace ludus::qa::patch
{
namespace gc = ::ludus::geometry::content;
using content::Status;
namespace
{
// Thanks to H. Moreton, Watertight Tessellation using Forward Differencing (GH 2001,
// pp. 25–32), for the bitwise shared-boundary criterion. Ludus emits canonical samples.
// https://www.cs.cmu.edu/afs/cs.cmu.edu/academic/class/15869-f11/www/readings/moreton01_tessellation.pdf
// Bernstein coefficients of r, r^2, r(1-r) and 16t(t-1/2)(t-1) on [0,1].
// Both patch axes reverse on the right to preserve normal orientation.
gc::AuthoredPatch MakePatch(Shape shape, bool isRight)
{
    gc::AuthoredPatch patch;
    patch.AuthorId = isRight == true ? 2 : 1;
    constexpr float64 SPARSE[] = {0, 8.0 / 3, -8.0 / 3, 0};
    const float64 width =
        shape == Shape::Mixed ? (isRight == true ? .65 : 1.35) : (shape == Shape::Plane && isRight == true ? .5 : 1);
    for (usize i = 0; i < geom::PATCH_CONTROL_ORDER; ++i)
    {
        const float64 r = static_cast<float64>(3 - i) / 3;
        const float64 square = i == 0 ? 1 : (i == 1 ? 1.0 / 3 : 0);
        const float64 x = (isRight == true ? width : -width) * r;
        for (usize j = 0; j < geom::PATCH_CONTROL_ORDER; ++j)
        {
            const usize tIndex = isRight == true ? 3 - j : j;
            const float64 y = 2 * static_cast<float64>(tIndex) / 3 - 1;
            const float64 dome = j == 1 || j == 2 ? 4.0 / 3 : 0; // 1-y^2
            float64 z = 0;
            switch (shape)
            {
                case Shape::Parabola:
                case Shape::Crease:
                    z = -square + (shape == Shape::Crease && isRight == true ? -.75 * x : 0);
                    break;
                case Shape::Unequal:
                case Shape::UnequalCrease:
                    z = isRight == true ? -square * dome - (shape == Shape::UnequalCrease ? .75 * x : 0) : 0;
                    break;
                case Shape::Sparse:
                    z = isRight == true ? x * SPARSE[tIndex] : 0;
                    break;
                case Shape::Bulge:
                    z = isRight == true ? 4 * (r - square) * dome : 0;
                    break;
                case Shape::Mixed:
                    z = -(isRight == true ? 2 : .04) * width * width * square;
                    break;
                case Shape::Plane:
                    break;
            }
            patch.Control[i][j] = {x, y, z};
        }
    }
    return patch;
}

bool Cook(const gc::GeometrySource& source, gc::GeometryLoad& outLoad)
{
    gc::GeometryDiagnosticReport diagnostic;
    core::Array<uint8> bytes;
    if (gc::CookGeometry(source, bytes, diagnostic) != Status::Ok ||
        gc::LoadGeometry({bytes.GetData(), bytes.GetSize()}, source.Limits, outLoad, diagnostic) != Status::Ok)
    {
        std::fprintf(stderr, "geometry cook/load failed: %s\n", diagnostic.Repair);
        return false;
    }
    return true;
}

void WriteVector(std::ostream& outStream, math::Vector3 vector)
{
    outStream << ' ' << vector.X << ' ' << vector.Y << ' ' << vector.Z;
}
} // namespace

bool BuildFixture(const Case& fixture,
                  float64 error,
                  geom::SurfaceMesh& outMesh,
                  const std::filesystem::path& directory)
{
    gc::GeometrySource source;
    source.Mode = gc::GeometrySourceMode::PatchSurface;
    source.Limits =
    {
        .MaxSpans = 16,
        .MaxLeaves = 64,
        .MaxDepth = 8,
        .MaxSubdivisions = 64,
        .MaxIterations = 128,
        .MaxPreparedBytes = 1U << 20,
        .MaxPatches = 2,
    };
    source.Tolerance =
    {
        .PositionError = error,
        .DistanceError = error,
        .TangentAngleError = .001,
        .ProjectionGap = error,
        .ContinuityAngle = 1e-5,
        .NormalAngleError = .01,
        .CoincidenceDistance = 1e-6,
    };
    geom::SurfaceMeshProfile profile;
    profile.PositionError = error;
    profile.Limits =
    {
        .MaxLevel = 8,
        .MaxCellTests = 1U << 18,
        .MaxVertices = 8192,
        .MaxTriangles = 8192,
        .MaxOutputBytes = 1U << 22,
    };
    std::ofstream data(directory / "mesh.txt");
    data << std::setprecision(17);
    for (usize i = 0; i < 2; ++i)
    {
        const bool isRight = (i == 1) != fixture.IsReordered;
        const gc::AuthoredPatch patch = MakePatch(fixture.Surface, isRight);
        if (source.Patches.TryAdd(patch) == false)
        {
            return false;
        }
    }
    // The disconnected baseline uses identical controls, IDs, tolerances and loader.
    gc::GeometryLoad isolated;
    geom::SurfaceMesh unjoined;
    if (Cook(source, isolated) == false ||
        geom::TryBuildSurfaceMesh(isolated.Surface.GetView(), profile, unjoined, nullptr) !=
            geom::GeometryStatus::Success)
    {
        return false;
    }
    if (source.Adjacency.TryAdd(
    {
        .FirstPatch = 0,
        .FirstSide = geom::PatchSide::U1,
        .SecondPatch = 1,
        .SecondSide = geom::PatchSide::U1,
        .IsReversed = true,
    }) == false)
    {
        return false;
    }
    gc::GeometryLoad loaded;
    if (Cook(source, loaded) == false ||
        geom::TryBuildSurfaceMesh(loaded.Surface.GetView(), profile, outMesh, nullptr) != geom::GeometryStatus::Success)
    {
        return false;
    }
    const geom::SurfaceView surface = loaded.Surface.GetView();
    const usize sharedEdge = surface.Patches[0].Edge[static_cast<usize>(geom::PatchSide::U1)];
    const geom::SurfaceEdge& edge = surface.Edges[sharedEdge];
    if (surface.Patches[1].Edge[static_cast<usize>(geom::PatchSide::U1)] != sharedEdge || edge.Id.PatchId != 1 ||
        edge.Id.Side != geom::PatchSide::U1 || edge.IsReversed == false || edge.IncidenceCount != 2 ||
        edge.IsOrientationConsistent == false)
    {
        return false;
    }
    const geom::SurfaceMeshView mesh = outMesh.GetView();
    if (mesh.Report.IsWithinPositionError == false || mesh.Report.PositionGuarantee != geom::GuaranteeClass::Certified)
    {
        return false;
    }
    for (usize i = 0; i < 2; ++i)
    {
        const geom::SurfaceMeshPatch& patch = mesh.Patches[i];
        data << "p " << surface.Patches[i].Id << ' ' << unjoined.GetView().Patches[i].LevelV << ' ' << patch.LevelV
             << ' ' << patch.PositionBound << ' ' << patch.IndexCount / 3 << '\n';
        for (usize j = 0; j <= 4; ++j)
        {
            const geom::SurfaceParameter parameter{ .PatchIndex = i, .U = 1, .V = static_cast<float64>(j) / 4 };
            math::PatchSample sample;
            math::Vector3 normal;
            if (geom::TryEvaluateSurface(loaded.Surface.GetView(), parameter, sample) !=
                    geom::GeometryStatus::Success ||
                geom::TryEvaluateSurfaceNormal(loaded.Surface.GetView(), parameter, normal) !=
                    geom::GeometryStatus::Success)
            {
                return false;
            }
            data << "d " << surface.Patches[i].Id << ' ' << parameter.V;
            WriteVector(data, sample.Position);
            WriteVector(data, sample.DU);
            WriteVector(data, sample.DV);
            WriteVector(data, normal);
            data << '\n';
        }
    }
    for (const geom::SurfaceMeshVertex& vertex : mesh.Vertices)
    {
        data << "v " << surface.Patches[vertex.Parameter.PatchIndex].Id << ' ' << vertex.Parameter.U << ' '
             << vertex.Parameter.V << ' ' << vertex.PositionIndex;
        WriteVector(data, vertex.Position);
        WriteVector(data, vertex.Normal);
        data << '\n';
    }
    for (usize i = 0; i < mesh.Indices.GetSize(); i += 3)
    {
        data << "f " << mesh.Indices[i] << ' ' << mesh.Indices[i + 1] << ' ' << mesh.Indices[i + 2] << '\n';
    }
    profile.Limits.MaxTriangles = 1;
    geom::SurfaceMesh rejected;
    data.close();
    return data.good() == true &&
           geom::TryBuildSurfaceMesh(loaded.Surface.GetView(), profile, rejected, nullptr) ==
               geom::GeometryStatus::BudgetExceeded &&
           rejected.IsEmpty() == true;
}

usize GetCameraCount(Shape shape)
{
    return shape == Shape::Parabola || shape == Shape::Mixed ? std::size(CAMERAS) : 2;
}

graphics::renderer::ViewDescription MakeView(const Camera& camera)
{
    const float32 cosine = 1 / std::sqrt(1 + camera.Slope * camera.Slope);
    const math::Vector3 forward{camera.Side * cosine, 0, -camera.Slope * cosine};
    const math::Vector3 up{camera.Side * camera.Slope * cosine, 0, cosine};
    constexpr float32 FOCAL = 1.6F;
    constexpr float32 NEAR = .25F;
    constexpr float32 FAR = 8;
    constexpr float32 DEPTH = FAR / (FAR - NEAR);
    graphics::renderer::ViewDescription view;
    view.CameraForward = forward;
    view.CameraPosition = -camera.Distance * forward;
    view.WorldToClip.Columns[0] = {0, FOCAL * up.X, DEPTH * forward.X, forward.X};
    view.WorldToClip.Columns[1] = {-camera.Side * FOCAL, 0, 0, 0};
    view.WorldToClip.Columns[2] = {0, FOCAL * up.Z, DEPTH * forward.Z, forward.Z};
    view.WorldToClip.Columns[3] = {0, 0, DEPTH * (camera.Distance - NEAR), camera.Distance};
    return view;
}
} // namespace ludus::qa::patch
