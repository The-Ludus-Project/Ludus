#include "internal/viewport_object_overlay.h"
#include "internal/viewport_object_overlay_item.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/profiling.hpp>

#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 VERTICES_PER_STROKE = 6;
constexpr usize STROKE_CORNERS = 4;
/// An outline segment is drawn as a casing stroke and an amber stroke.
constexpr usize OUTLINE_STROKES_PER_LINE = 2;
constexpr StaticArray<usize, VERTICES_PER_STROKE> STROKE_TRIANGLES{0, 1, 2, 2, 1, 3};
constexpr float64 MINIMUM_STROKE_LENGTH_PIXELS = 0.000001;

/// Fraction of a stroke's width by which an outline segment extends past each end, so that consecutive segments
/// of a silhouette join without a notch at a corner.
constexpr float64 CAP_WIDTH_FRACTION = 0.5;
constexpr int32 MAXIMUM_ALPHA = 255;

/// A stroke quad for linePixel, widthPixels wide. capPixels extends both ends along the line. The colour is
/// premultiplied, as the scene graph's vertex-colour material expects.
void WriteStroke(const QLineF& linePixel,
                 QRgb color,
                 float32 widthPixels,
                 float32 capPixels,
                 QSGGeometry::ColoredPoint2D* outVerticesPixel) noexcept
{
    const QPointF directionPixel = linePixel.p2() - linePixel.p1();
    const float64 lengthPixels = math::Sqrt(QPointF::dotProduct(directionPixel, directionPixel));
    const bool hasDirection = lengthPixels > MINIMUM_STROKE_LENGTH_PIXELS;
    const QPointF normalPixel = hasDirection == true ? QPointF(-directionPixel.y(), directionPixel.x()) *
                                                           (static_cast<float64>(widthPixels) * 0.5 / lengthPixels)
                                                     : QPointF{};
    const QPointF capVectorPixel =
        hasDirection == true ? directionPixel * (static_cast<float64>(capPixels) / lengthPixels) : QPointF{};
    const QPointF startPixel = linePixel.p1() - capVectorPixel;
    const QPointF endPixel = linePixel.p2() + capVectorPixel;
    const StaticArray<QPointF, STROKE_CORNERS> cornersPixel{startPixel + normalPixel,
                                                            startPixel - normalPixel,
                                                            endPixel + normalPixel,
                                                            endPixel - normalPixel};
    const int32 alpha = qAlpha(color);
    const uint8 red = static_cast<uint8>(qRed(color) * alpha / MAXIMUM_ALPHA);
    const uint8 green = static_cast<uint8>(qGreen(color) * alpha / MAXIMUM_ALPHA);
    const uint8 blue = static_cast<uint8>(qBlue(color) * alpha / MAXIMUM_ALPHA);
    for (usize index = 0; index < STROKE_TRIANGLES.GetSize(); ++index)
    {
        const QPointF positionPixel = cornersPixel[STROKE_TRIANGLES[index]];
        outVerticesPixel[index].set(static_cast<float32>(positionPixel.x()),
                                    static_cast<float32>(positionPixel.y()),
                                    red,
                                    green,
                                    blue,
                                    static_cast<uint8>(alpha));
    }
}

} // namespace

QSGNode* UpdateViewportObjectOverlayNode(const ViewportObjectOverlay& overlay, QSGNode* oldNodeOrNull)
{
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayUpload);
    if (overlay.OutlineLinesPixel.GetSize() > ViewportObjectOverlay::MAX_OUTLINE_LINES ||
        overlay.HandleStrokes.GetSize() > ViewportObjectOverlay::MAX_HANDLE_STROKES)
    {
        delete oldNodeOrNull;
        return nullptr;
    }
    // Every outline segment is stroked twice: a dark casing under the amber line.
    const usize strokeCount =
        overlay.OutlineLinesPixel.GetSize() * OUTLINE_STROKES_PER_LINE + overlay.HandleStrokes.GetSize();
    if (strokeCount == 0 || overlay.FramePixel.isEmpty() == true)
    {
        delete oldNodeOrNull;
        return nullptr;
    }
    QSGClipNode* clipOrNull = static_cast<QSGClipNode*>(oldNodeOrNull);
    if (clipOrNull == nullptr)
    {
        clipOrNull = new QSGClipNode;
        clipOrNull->setIsRectangular(true);
        QSGGeometryNode* node = new QSGGeometryNode;
        QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
        clipOrNull->appendChildNode(node);
    }
    clipOrNull->setClipRect(overlay.FramePixel);
    QSGGeometryNode* node = static_cast<QSGGeometryNode*>(clipOrNull->firstChild());
    QSGGeometry* geometry = node->geometry();
    const int32 vertexCount = static_cast<int32>(strokeCount * VERTICES_PER_STROKE);
    if (geometry->vertexCount() != vertexCount)
    {
        geometry->allocate(vertexCount);
    }
    QSGGeometry::ColoredPoint2D* verticesPixel = geometry->vertexDataAsColoredPoint2D();
    usize offset = 0;
    // All casings first, so no casing darkens a neighbouring segment's amber.
    for (const QLineF& linePixel : overlay.OutlineLinesPixel)
    {
        WriteStroke(linePixel,
                    ViewportObjectOverlay::CASING_COLOR,
                    ViewportObjectOverlay::CASING_WIDTH_PIXELS,
                    ViewportObjectOverlay::CASING_WIDTH_PIXELS * static_cast<float32>(CAP_WIDTH_FRACTION),
                    verticesPixel + offset);
        offset += VERTICES_PER_STROKE;
    }
    for (const QLineF& linePixel : overlay.OutlineLinesPixel)
    {
        WriteStroke(linePixel,
                    ViewportObjectOverlay::SELECTION_COLOR,
                    ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS,
                    ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS * static_cast<float32>(CAP_WIDTH_FRACTION),
                    verticesPixel + offset);
        offset += VERTICES_PER_STROKE;
    }
    for (const ViewportHandleStroke& stroke : overlay.HandleStrokes)
    {
        WriteStroke(stroke.LinePixel, stroke.Color, stroke.WidthPixels, 0, verticesPixel + offset);
        offset += VERTICES_PER_STROKE;
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
    return clipOrNull;
}
} // namespace ludus::editor
