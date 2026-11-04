#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::editor
{
/// One outline segment between two homogeneous clip-space points. Neither end is clipped or divided.
struct SilhouetteSegment final
{
    foundation::math::Vector4 FirstClip{};
    foundation::math::Vector4 SecondClip{};
};

/// View-dependent outline of one triangle mesh: the edges where a front-facing triangle meets a
/// back-facing one, and every edge that has a single triangle. The outline is the shape a viewer
/// reads as the object's silhouette, so selecting an object marks its form instead of its
/// tessellation, and its cost follows the edge count rather than a screen-sized mask.
///
/// The edge adjacency is built once per mesh. Vertices at the same position are welded, so a mesh
/// that splits vertices for normals or texture seams still reports its true silhouette. Extraction
/// is a transform of every vertex, one orientation test per triangle and one comparison per edge.
/// Owner-thread value type; Extract reuses its scratch buffers and does not allocate after the first call.
///
/// Facing comes from triangle winding, so a mesh with inconsistently wound triangles shows extra
/// outline along those seams. Only the mesh itself is considered: another object in front does not
/// hide any of the outline.
class MeshSilhouette final
{
public:
    /// Largest mesh whose adjacency is built, bounding the one-off build and each extraction.
    static constexpr foundation::usize MAX_TRIANGLES = 200000;
    /// Vertices in one triangle of a triangle list.
    static constexpr foundation::usize TRIANGLE_VERTICES = 3;
    /// Build the welded edge adjacency of a triangle list. Triangles with an out-of-range index or a
    /// repeated welded vertex are skipped. Failure (no usable triangle, more than MAX_TRIANGLES,
    /// a non-finite position or allocation failure) leaves the silhouette empty.
    /// @param positionsMesh Mesh-space vertex positions, read only for the call.
    /// @param indices Triangle-list vertex indices into positionsMesh, read only for the call.
    [[nodiscard]] bool Build(foundation::BorrowedView<const foundation::math::Vector3> positionsMesh,
                             foundation::BorrowedView<const foundation::uint32> indices);
    /// Forget the mesh and release nothing; capacity is retained for the next Build.
    void Clear() noexcept;
    /// Whether Build has not succeeded since construction or the last Clear.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mEdges.IsEmpty();
    }
    /// Number of welded edges considered by each Extract.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetEdgeCount() const noexcept
    {
        return mEdges.GetSize();
    }
    /// Append the outline segments seen through meshToClip, after any segments already in outSegments, until
    /// maximumSegments in total are present. Returns false if positionsMesh is not the mesh passed to Build or storage
    /// cannot be reserved; outSegments may then hold only a prefix of the outline.
    /// @param positionsMesh The same mesh-space positions given to Build.
    /// @param meshToClip Mesh space to homogeneous clip space; the orientation test is valid for vertices behind the
    ///        camera, so no clipping is needed first.
    /// @param maximumSegments Bound on the total size of outSegments.
    /// @param outSegments Appended segments, in edge order.
    [[nodiscard]] bool Extract(foundation::BorrowedView<const foundation::math::Vector3> positionsMesh,
                               const foundation::math::Matrix4& meshToClip,
                               foundation::usize maximumSegments,
                               foundation::Array<SilhouetteSegment>& outSegments);

private:
    struct Triangle
    {
        foundation::uint32 First = 0;
        foundation::uint32 Second = 0;
        foundation::uint32 Third = 0;
    };
    struct Edge
    {
        /// Welded endpoints, as indices of the first vertex built at each position.
        foundation::uint32 First = 0;
        foundation::uint32 Second = 0;
        /// This edge's triangles are mEdgeTriangles[FirstTriangle, FirstTriangle + TriangleCount).
        foundation::uint32 FirstTriangle = 0;
        foundation::uint32 TriangleCount = 0;
    };
    foundation::Array<Triangle> mTriangles;
    foundation::Array<Edge> mEdges;
    foundation::Array<foundation::uint32> mEdgeTriangles;
    foundation::Array<foundation::math::Vector4> mClip;
    foundation::Array<foundation::int8> mFacing;
    foundation::usize mVertexCount = 0;
};
} // namespace ludus::editor
