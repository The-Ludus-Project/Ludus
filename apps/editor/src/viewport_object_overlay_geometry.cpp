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
constexpr StaticArray<usize, VERTICES_PER_STROKE> STROKE_TRIANGLES{0, 1, 2, 2, 1, 3};
constexpr float64 MINIMUM_STROKE_LENGTH_PIXELS = 0.000001;

void WriteStroke(const QLineF& linePixel,
                 QRgb color,
                 float32 widthPixels,
                 QSGGeometry::ColoredPoint2D* outVerticesPixel) noexcept
{
    const QPointF directionPixel = linePixel.p2() - linePixel.p1();
    const float64 lengthPixels = math::Sqrt(QPointF::dotProduct(directionPixel, directionPixel));
    const QPointF normalPixel = lengthPixels > MINIMUM_STROKE_LENGTH_PIXELS
                                    ? QPointF(-directionPixel.y(), directionPixel.x()) *
                                          (static_cast<float64>(widthPixels) * 0.5 / lengthPixels)
                                    : QPointF{};
    const StaticArray<QPointF, STROKE_CORNERS> cornersPixel{linePixel.p1() + normalPixel,
                                                            linePixel.p1() - normalPixel,
                                                            linePixel.p2() + normalPixel,
                                                            linePixel.p2() - normalPixel};
    for (usize index = 0; index < STROKE_TRIANGLES.GetSize(); ++index)
    {
        const QPointF positionPixel = cornersPixel[STROKE_TRIANGLES[index]];
        outVerticesPixel[index].set(static_cast<float32>(positionPixel.x()),
                                    static_cast<float32>(positionPixel.y()),
                                    static_cast<uint8>(qRed(color)),
                                    static_cast<uint8>(qGreen(color)),
                                    static_cast<uint8>(qBlue(color)),
                                    static_cast<uint8>(qAlpha(color)));
    }
}

} // namespace

QSGNode* UpdateViewportObjectOverlayNode(const ViewportObjectOverlay& overlay, QSGNode* oldNodeOrNull)
{
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayUpload);
    if (overlay.WireLinesPixel.GetSize() > ViewportObjectOverlay::MAX_WIRE_LINES ||
        overlay.HandleStrokes.GetSize() > ViewportObjectOverlay::MAX_HANDLE_STROKES)
    {
        delete oldNodeOrNull;
        return nullptr;
    }
    const usize strokeCount = overlay.WireLinesPixel.GetSize() + overlay.HandleStrokes.GetSize();
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
    for (const QLineF& linePixel : overlay.WireLinesPixel)
    {
        WriteStroke(linePixel,
                    ViewportObjectOverlay::SELECTION_COLOR,
                    ViewportObjectOverlay::WIRE_WIDTH_PIXELS,
                    verticesPixel + offset);
        offset += VERTICES_PER_STROKE;
    }
    for (const ViewportHandleStroke& stroke : overlay.HandleStrokes)
    {
        WriteStroke(stroke.LinePixel, stroke.Color, stroke.WidthPixels, verticesPixel + offset);
        offset += VERTICES_PER_STROKE;
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
    return clipOrNull;
}
} // namespace ludus::editor
