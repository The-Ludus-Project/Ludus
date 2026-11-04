#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QByteArray>

namespace ludus::editor
{
/// What the viewport asks the GPU to outline over the displayed image: one mesh of the scene, placed by a transform,
/// stroked with logical-pixel widths. The mesh arrays are borrowed for one call; the receiver copies what it keeps.
struct SelectionOutlineRequest final
{
    /// Identity of the scene the mesh belongs to; with Mesh it decides whether the mesh must be uploaded again.
    QByteArray SceneIdentity;
    /// Index of the mesh in that scene.
    foundation::int32 Mesh = -1;
    /// Mesh-local positions.
    foundation::BorrowedView<const foundation::math::Vector3> PositionsMesh;
    /// Triangle-list indices into PositionsMesh.
    foundation::BorrowedView<const foundation::uint32> Indices;
    /// Placement of the mesh in the world, which a drag preview moves ahead of the shown image.
    foundation::math::Affine3 MeshToWorld{};
    /// Image pixels per logical pixel of the viewport, so strokes keep their on-screen width on a scaled display or
    /// when the image is shown larger or smaller than it was rendered.
    foundation::float32 StrokeScale = 1;
    /// Straight RGBA of the main stroke, display-encoded.
    foundation::math::Vector4 OutlineColor{1, 1, 1, 1};
    /// Straight RGBA of the casing under it, display-encoded.
    foundation::math::Vector4 CasingColor{0, 0, 0, 0};
    /// Logical width of the main stroke.
    foundation::float32 OutlineWidth = 2;
    /// Logical width of the casing.
    foundation::float32 CasingWidth = 4;
};
} // namespace ludus::editor
