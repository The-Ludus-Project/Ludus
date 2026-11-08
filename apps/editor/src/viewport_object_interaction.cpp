#include "internal/viewport_object_interaction.h"
#include "design_tokens.h"
#include "internal/render_preview.h"
#include "internal/viewport_gizmo_readout.h"
#include "internal/world_document.h"
#include "internal/world_terrain.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QColor>
#include <QJsonArray>
#include <QPainter>
#include <QPen>
#include <QString>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr int32 AXIS_COUNT = ViewportObjectInteraction::AXIS_COUNT;
constexpr usize TRIANGLE_VERTICES = ViewportObjectOverlay::TRIANGLE_VERTICES;
/// Corners of a mesh's bounding box, as the combinations of its three axis bits.
constexpr usize BOX_CORNERS = 8;
constexpr usize MAX_OUTLINE_LINES = ViewportObjectOverlay::MAX_OUTLINE_LINES;
/// A luminaire's outline: the panel square, its two diagonals and the emission direction.
constexpr usize LUMINAIRE_OUTLINE_CORNERS = 4;
constexpr usize LUMINAIRE_OUTLINE_LINES = LUMINAIRE_OUTLINE_CORNERS + 3;
constexpr float32 PROJECTION_EPSILON = 0.000001F;
constexpr float64 HANDLE_PIXELS = 80;
constexpr float64 HANDLE_HIT_PIXELS = 9;
constexpr float64 MINIMUM_AXIS_PIXELS = 8;
/// Arrowhead length and half width of a move handle, in logical pixels.
constexpr float64 ARROW_LENGTH_PIXELS = 15;
constexpr float64 ARROW_HALF_WIDTH_PIXELS = 6;
/// Half side of a scale handle's cube and of the pivot marker, in logical pixels.
constexpr float64 CUBE_HALF_PIXELS = 5.5;
constexpr float64 PIVOT_HALF_PIXELS = 3.5;
/// Plane handle: a parallelogram this far from the pivot along both screen axes, in logical pixels.
constexpr float64 PLANE_INNER_PIXELS = 22;
constexpr float64 PLANE_OUTER_PIXELS = 38;
/// Dark casing around a plane or centre handle, so it reads over a bright object.
constexpr float64 CASING_GROWTH_PIXELS = 1.5;
/// Slack added around a plane handle when testing the pointer against it.
constexpr float64 PLANE_HIT_MARGIN_PIXELS = 3;
/// Smallest sine of the angle between a plane's two screen axes, and smallest cosine between the plane's normal and
/// the view direction, for which its handle is shown; below either the plane is seen too nearly edge-on to drag in.
constexpr float64 MINIMUM_PLANE_SINE = 0.3;
constexpr float32 MINIMUM_PLANE_FACING = 0.25F;
/// Share, in percent, of the viewport colour mixed into a plane handle at rest, so the axes stay the louder cue.
constexpr int32 PLANE_REST_PERCENT = 20;
/// Half side of the scale tool's centre cube, and half side of the square around the pivot that hits it.
constexpr float64 CENTRE_HALF_PIXELS = 7.5;
constexpr float64 CENTRE_HIT_HALF_PIXELS = 10;
/// A pointer this close to an axis shaft grabs the axis even where a plane handle also lies under it.
constexpr float64 SHAFT_PRIORITY_PIXELS = 4;
/// Unit vector, up and to the right on the screen, along which dragging the centre handle grows the scale.
constexpr float64 UNIFORM_DRAG_COMPONENT = 0.70710678118654752;
constexpr float64 MINIMUM_PLANE_DETERMINANT = 0.000000001;
constexpr float64 MOVE_SNAP_METRES = 0.1;
constexpr float64 ROTATE_SNAP_DEGREES = 15;
constexpr float64 SCALE_SNAP = 0.1;
/// Lowest fraction of the original scale previewed by one drag; WorldDocument owns absolute admission limits.
constexpr float64 MINIMUM_SCALE_FACTOR = 0.01;
constexpr float64 DEGREES_PER_TURN = math::RadiansToDegrees(math::kTau);
constexpr float64 PIXELS_PER_DEGREE = HANDLE_PIXELS * math::DegreesToRadians(1.0);
constexpr float64 HANDLE_WIDTH = 3;
/// Width of the handle under the pointer or being dragged.
constexpr float64 EMPHASIZED_HANDLE_WIDTH = 4.5;
/// Share, in percent, of white mixed into the emphasized handle's axis colour.
constexpr int32 EMPHASIS_PERCENT = 45;
/// Share, in percent, of the viewport colour mixed into a handle that recedes: the other axes while one
/// is dragged, and the half of a rotation ring behind the pivot.
constexpr int32 RECEDE_AXES_PERCENT = 65;
constexpr int32 RECEDE_BACK_PERCENT = 45;
constexpr int32 PERCENT_DENOMINATOR = 100;
constexpr float64 LABEL_OFFSET = 12;
constexpr float64 GLYPH_WIDTH_PIXELS = 6;
constexpr float64 GLYPH_HEIGHT_PIXELS = 8;
constexpr usize MAX_HANDLE_STROKES = ViewportObjectOverlay::MAX_HANDLE_STROKES;
constexpr float64 MAXIMUM_GESTURE_PIXELS = 10000000;
/// Mesh-bounds growth per unit of its largest extent, so a hit on a box face survives slab rounding.
constexpr float32 PICK_BOUNDS_PADDING = 0.0001F;
/// Ray parameter bound before any hit; finite because FoundationMath intervals reject NaN bounds.
constexpr float64 UNBOUNDED_PICK_DISTANCE = 1.0e300;
constexpr float32 COLOR_CHANNEL_MAXIMUM = 255;

/// Axis colours from the design tokens ("always with the letter X/Y/Z", hence the glyph at each tip).
constexpr StaticArray<QRgb, AXIS_COUNT> AXIS_COLORS{design::GetThemeArgb(design::ThemeColour::AxisX, true),
                                                    design::GetThemeArgb(design::ThemeColour::AxisY, true),
                                                    design::GetThemeArgb(design::ThemeColour::AxisZ, true)};
/// Colour that receding handles fade towards, so they never turn translucent over the frame.
constexpr QRgb RECEDE_COLOR = design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true);
constexpr QRgb EMPHASIS_COLOR = design::GetThemeArgb(design::ThemeColour::TextHud, true);
constexpr QRgb PIVOT_COLOR = design::GetThemeArgb(design::ThemeColour::TextHud, true);
/// The centre handle rests in the text-over-viewport colour and takes the accent colour under the pointer.
constexpr QRgb CENTRE_COLOR = design::GetThemeArgb(design::ThemeColour::TextHud, true);
constexpr QRgb CENTRE_EMPHASIS_COLOR = design::GetThemeArgb(design::ThemeColour::AccentPrimary, true);
/// The two axes a plane handle spans, indexed by the axis normal to the plane.
struct PlaneAxes final
{
    int32 First = 0;
    int32 Second = 1;
};
constexpr StaticArray<PlaneAxes, AXIS_COUNT> PLANE_AXES{{{1, 2}, {0, 2}, {0, 1}}};
constexpr int32 MixChannel(int32 from, int32 to, int32 percent) noexcept
{
    return from + (to - from) * percent / PERCENT_DENOMINATOR;
}
/// The opaque colour percent of the way from one colour to another.
constexpr QRgb MixColour(QRgb from, QRgb to, int32 percent) noexcept
{
    return qRgb(MixChannel(qRed(from), qRed(to), percent),
                MixChannel(qGreen(from), qGreen(to), percent),
                MixChannel(qBlue(from), qBlue(to), percent));
}
float64 ComputeSnap(float64 value, float64 interval) noexcept
{
    const float64 scaled = value / interval;
    return static_cast<float64>(static_cast<int64>(scaled + (scaled < 0 ? -0.5 : 0.5))) * interval;
}
/// The ray through positionPixel, from the near plane towards the far side; false for a degenerate camera.
bool TryMakePixelRay(const math::Matrix4& clipToWorld,
                     const QRectF& framePixel,
                     QPointF positionPixel,
                     math::Ray3& outRay)
{
    const float32 x = static_cast<float32>((positionPixel.x() - framePixel.left()) / framePixel.width() * 2 - 1);
    const float32 y = static_cast<float32>(1 - (positionPixel.y() - framePixel.top()) / framePixel.height() * 2);
    math::Vector3 nearWorld;
    math::Vector3 middleWorld;
    if (math::TryPerspectiveDivide(clipToWorld * math::Vector4{x, y, 1, 1}, 0, nearWorld) !=
            math::MathStatus::Success ||
        math::TryPerspectiveDivide(clipToWorld * math::Vector4{x, y, 0.5F, 1}, 0, middleWorld) !=
            math::MathStatus::Success)
    {
        return false;
    }
    outRay = {nearWorld, middleWorld - nearWorld};
    return true;
}
/// Where ray meets the plane through originWorld with normal normalWorld, in front of the ray's origin.
bool TryIntersectPlane(const math::Ray3& ray,
                       math::Vector3 originWorld,
                       math::Vector3 normalWorld,
                       math::Vector3& outHitWorld)
{
    math::Vector3 unitDirection;
    if (math::TryNormalize(ray.Direction, unitDirection) != math::MathStatus::Success)
    {
        return false;
    }
    const float32 alignment = math::Dot(normalWorld, unitDirection);
    if (math::Abs(alignment) < PROJECTION_EPSILON)
    {
        return false;
    }
    const float32 distance = math::Dot(normalWorld, originWorld - ray.Origin) / alignment;
    if (math::IsFinite(distance) == false || distance < 0)
    {
        return false;
    }
    outHitWorld = ray.Origin + unitDirection * distance;
    return true;
}
/// Whether point lies inside the convex quad, whichever way round its corners run.
bool IsInsideQuad(QPointF point, const ViewportHandleFill::Corners& cornersPixel)
{
    bool hasPositive = false;
    bool hasNegative = false;
    for (usize corner = 0; corner < cornersPixel.GetSize(); ++corner)
    {
        const QPointF edge = cornersPixel[(corner + 1) % cornersPixel.GetSize()] - cornersPixel[corner];
        const QPointF toPoint = point - cornersPixel[corner];
        const float64 cross = edge.x() * toPoint.y() - edge.y() * toPoint.x();
        hasPositive = hasPositive == true || cross > 0;
        hasNegative = hasNegative == true || cross < 0;
    }
    return hasPositive == false || hasNegative == false;
}
/// The quad with every corner moved growthPixels further from its centre.
ViewportHandleFill::Corners GrowQuad(const ViewportHandleFill::Corners& cornersPixel, float64 growthPixels)
{
    QPointF centrePixel;
    for (const QPointF& cornerPixel : cornersPixel)
    {
        centrePixel += cornerPixel;
    }
    centrePixel /= static_cast<float64>(cornersPixel.GetSize());
    ViewportHandleFill::Corners grown = cornersPixel;
    for (QPointF& cornerPixel : grown)
    {
        const QPointF outward = cornerPixel - centrePixel;
        const float64 length = math::Sqrt(QPointF::dotProduct(outward, outward));
        if (length > 0)
        {
            cornerPixel += outward * (growthPixels / length);
        }
    }
    return grown;
}
float64 ComputeDistanceSquared(QPointF point, const QLineF& line) noexcept
{
    const QPointF direction = line.p2() - line.p1();
    const float64 lengthSquared = QPointF::dotProduct(direction, direction);
    const float64 t = lengthSquared > 0
                          ? math::Clamp(QPointF::dotProduct(point - line.p1(), direction) / lengthSquared, 0.0, 1.0)
                          : 0;
    const QPointF difference = point - line.p1() - direction * t;
    return QPointF::dotProduct(difference, difference);
}
} // namespace
ViewportObjectInteraction::ViewportObjectInteraction(RenderPreview& inoutPreview, QObject* parentOrNull)
    : QObject(parentOrNull), mPreviewOrNull(&inoutPreview)
{
    connect(&inoutPreview, &RenderPreview::NavigationChanged, this, &ViewportObjectInteraction::Changed);
    connect(&inoutPreview, &RenderPreview::Changed, this, &ViewportObjectInteraction::Changed);
    connect(&inoutPreview, &QObject::destroyed, this, [this]() {
        mCanEdit = false;
        SetActive(false);
    });
}
void ViewportObjectInteraction::SetWorld(const WorldDocument& world, const QString& selected, bool canEdit)
{
    Cancel();
    if (mPreviewOrNull.isNull() == true)
    {
        mCanEdit = false;
        return;
    }
    mSelected = selected;
    mPlacement = world.ScenePlacement(world.Entity(selected));
    mLuminaireHalfExtentWorld = 0;
    const QJsonObject luminaireBinding = mPreviewOrNull->Scene().LuminaireBinding;
    if (luminaireBinding.isEmpty() == false)
    {
        const QJsonValue halfExtent = world.Entity(selected)
                                          .value(QStringLiteral("components"))
                                          .toObject()
                                          .value(luminaireBinding.value(QStringLiteral("component")).toString())
                                          .toObject()
                                          .value(luminaireBinding.value(QStringLiteral("properties"))
                                                     .toObject()
                                                     .value(QStringLiteral("halfExtent"))
                                                     .toString());
        if (halfExtent.isDouble() == true && halfExtent.toDouble() > 0)
        {
            mLuminaireHalfExtentWorld = static_cast<float32>(halfExtent.toDouble());
        }
    }
    if (mPlacement.isEmpty() == true)
    {
        const QJsonObject binding = mPreviewOrNull->Scene().TransformBinding;
        const QJsonObject values = world.Entity(selected)
                                       .value(QStringLiteral("components"))
                                       .toObject()
                                       .value(binding.value(QStringLiteral("component")).toString())
                                       .toObject();
        // The preview descriptor's binding names each semantic transform property under "properties".
        const QJsonObject properties = binding.value(QStringLiteral("properties")).toObject();
        if (binding.isEmpty() == false && values.isEmpty() == false)
        {
            mPlacement.insert(QStringLiteral("basis"), QJsonArray{1, 0, 0, 0, 1, 0, 0, 0, 1});
            mPlacement.insert(QStringLiteral("parent"), QString());
            const TransformPropertyNames keys{QStringLiteral("position"),
                                              QStringLiteral("rotation"),
                                              QStringLiteral("scale")};
            const TransformPropertyNames prefixes{QString(), QStringLiteral("r"), QStringLiteral("s")};
            for (usize kind = 0; kind < keys.GetSize(); ++kind)
            {
                QJsonArray axes;
                for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
                {
                    const QString name = prefixes[kind] + QChar('x' + axis);
                    axes.append(values.value(properties.value(name).toString()));
                }
                mPlacement.insert(keys[kind], axes);
            }
        }
    }
    mCanEdit = canEdit == true && mPlacement.isEmpty() == false;
    mParentToWorld = {};
    const QString parentId = mPlacement.value(QStringLiteral("parent")).toString();
    bool hasParent = parentId.isEmpty();
    for (const PreviewObject& object : mPreviewOrNull->Scene().Objects)
    {
        if (object.Id == parentId)
        {
            mParentToWorld = object.Transform;
            hasParent = true;
        }
    }
    mCanEdit = mCanEdit == true && hasParent == true;
    Q_EMIT Changed();
}
void ViewportObjectInteraction::SetTerrainFootprint(const QJsonObject& recipe)
{
    const math::Vector2 extent{static_cast<float32>(recipe.value(QStringLiteral("width")).toDouble()),
                               static_cast<float32>(recipe.value(QStringLiteral("depth")).toDouble())};
    const float32 base = static_cast<float32>(recipe.value(QStringLiteral("minimumElevation")).toDouble());
    if (mTerrainExtent == extent && mTerrainBaseElevation == base)
    {
        return;
    }
    Cancel();
    mTerrainExtent = extent;
    mTerrainBaseElevation = base;
    Q_EMIT Changed();
}
void ViewportObjectInteraction::SetTerrainBrush(const QString& key, float32 radius, bool shouldPickHeight)
{
    if (key == mTerrainBrushKey && radius == mTerrainBrushRadius && shouldPickHeight == mShouldPickTerrainHeight)
    {
        return;
    }
    Cancel();
    mTerrainBrushKey = key;
    mTerrainBrushRadius = radius;
    mShouldPickTerrainHeight = shouldPickHeight;
    Q_EMIT Changed();
}
void ViewportObjectInteraction::SetTool(Tool tool)
{
    Cancel();
    mTool = tool;
    Q_EMIT Changed();
}
void ViewportObjectInteraction::SetActive(bool isActive)
{
    if (mIsActive != isActive)
    {
        Cancel();
        mIsActive = isActive;
        Q_EMIT Changed();
    }
}
void ViewportObjectInteraction::SetImageSize(QSize size)
{
    if (size != mImageSize)
    {
        Cancel();
        mImageSize = size;
        Q_EMIT Changed();
    }
}
void ViewportObjectInteraction::SetPresentedView(const math::Matrix4& worldToClip)
{
    if (mHasPresentedView == true && worldToClip == mPresentedWorldToClip)
    {
        return;
    }
    // A drag keeps the projection it was pressed with; the next build or press picks this one up.
    mPresentedWorldToClip = worldToClip;
    mHasPresentedView = true;
    Q_EMIT Changed();
}
void ViewportObjectInteraction::ClearPresentedView()
{
    if (mHasPresentedView == true)
    {
        mHasPresentedView = false;
        Q_EMIT Changed();
    }
}
QRectF ViewportObjectInteraction::resolveFrame(const QRectF& framePixel) const
{
    if (mImageSize.isEmpty() == true)
    {
        return framePixel;
    }
    const QSizeF size = QSizeF(mImageSize).scaled(framePixel.size(), Qt::KeepAspectRatio);
    return {framePixel.center() - QPointF(size.width() * 0.5, size.height() * 0.5), size};
}
bool ViewportObjectInteraction::describe(const QRectF& framePixel, math::Matrix4& outWorldToClip) const
{
    if (mPreviewOrNull.isNull() == true || mIsActive == false || framePixel.width() < 1 || framePixel.height() < 1)
    {
        return false;
    }
    const RenderScene& scene = mPreviewOrNull->Scene();
    if (scene.Definition != nullptr && scene.Definition->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size()))
    {
        return false;
    }
    graphics::renderer::ViewDescription view;
    QString error;
    if (mPreviewOrNull->DescribeView(static_cast<uint32>(framePixel.width()),
                                     static_cast<uint32>(framePixel.height()),
                                     view,
                                     error) == false)
    {
        return false;
    }
    // The live camera can be ahead of an asynchronously rendered image; draw over what is shown.
    outWorldToClip = mHasPresentedView == true ? mPresentedWorldToClip : view.WorldToClip;
    return true;
}
QPointF ViewportObjectInteraction::convertClipToPixel(const math::Vector4& clip) const
{
    return {mFramePixel.left() + (static_cast<float64>(clip.X / clip.W) + 1) * mFramePixel.width() * 0.5,
            mFramePixel.top() + (1 - static_cast<float64>(clip.Y / clip.W)) * mFramePixel.height() * 0.5};
}
bool ViewportObjectInteraction::project(math::Vector3 positionWorld, QPointF& outPositionPixel) const
{
    const math::Vector4 clip = mWorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
    if (math::IsFinite(clip) == false || clip.W <= PROJECTION_EPSILON || clip.Z < 0 || clip.Z > clip.W)
    {
        return false;
    }
    outPositionPixel = convertClipToPixel(clip);
    return true;
}
bool ViewportObjectInteraction::projectSegment(const SilhouetteSegment& segment, QLineF& outLinePixel) const
{
    math::Vector4 first = segment.FirstClip;
    math::Vector4 second = segment.SecondClip;
    if (math::IsFinite(first) == false || math::IsFinite(second) == false)
    {
        return false;
    }
    const bool isFirstBehind = first.W <= PROJECTION_EPSILON;
    const bool isSecondBehind = second.W <= PROJECTION_EPSILON;
    if (isFirstBehind == true && isSecondBehind == true)
    {
        return false;
    }
    // Keep the part of a segment crossing the camera plane that lies in front of it.
    if (isFirstBehind == true || isSecondBehind == true)
    {
        math::Vector4& behind = isFirstBehind == true ? first : second;
        const math::Vector4& front = isFirstBehind == true ? second : first;
        const float32 fraction = (PROJECTION_EPSILON - behind.W) / (front.W - behind.W);
        behind = {behind.X + (front.X - behind.X) * fraction,
                  behind.Y + (front.Y - behind.Y) * fraction,
                  behind.Z + (front.Z - behind.Z) * fraction,
                  PROJECTION_EPSILON};
    }
    // A segment wholly beyond one side of the frame has nothing to draw.
    const bool isLeftOfFrame = first.X < -first.W && second.X < -second.W;
    const bool isRightOfFrame = first.X > first.W && second.X > second.W;
    const bool isBelowFrame = first.Y < -first.W && second.Y < -second.W;
    const bool isAboveFrame = first.Y > first.W && second.Y > second.W;
    if (isLeftOfFrame == true || isRightOfFrame == true || isBelowFrame == true || isAboveFrame == true)
    {
        return false;
    }
    outLinePixel = QLineF(convertClipToPixel(first), convertClipToPixel(second));
    return true;
}
bool ViewportObjectInteraction::refreshSilhouette(int32 meshIndex)
{
    const RenderScene& scene = mPreviewOrNull->Scene();
    if (scene.Identity == mSilhouetteIdentity && meshIndex == mSilhouetteMesh)
    {
        return mHasSilhouette;
    }
    mSilhouetteIdentity = scene.Identity;
    mSilhouetteMesh = meshIndex;
    mHasSilhouette =
        mSilhouette.Build(GetRenderSceneMeshPositions(scene, meshIndex), GetRenderSceneMeshIndices(scene, meshIndex));
    return mHasSilhouette;
}
void ViewportObjectInteraction::appendObjectOutline(const PreviewObject& object,
                                                    const math::Affine3& meshToWorld,
                                                    ViewportObjectOverlay& inoutOverlay)
{
    const RenderScene& scene = mPreviewOrNull->Scene();
    const math::Matrix4 meshToClip = mWorldToClip * math::ToMatrix4(meshToWorld);
    mOutlineSegments.Clear();
    const bool hasSilhouette = refreshSilhouette(object.Mesh) == true &&
                               mSilhouette.Extract(GetRenderSceneMeshPositions(scene, object.Mesh),
                                                   meshToClip,
                                                   MAX_OUTLINE_LINES - inoutOverlay.OutlineLinesPixel.GetSize(),
                                                   static_cast<uint32>(mFramePixel.width()),
                                                   static_cast<uint32>(mFramePixel.height()),
                                                   mOutlineSegments) == true;
    if (hasSilhouette == false)
    {
        // A mesh too large for adjacency, or without a usable triangle, is marked by its bounding box.
        mOutlineSegments.Clear();
        if (refreshMeshBounds() == false || math::IsEmpty(mMeshBoundsMesh[static_cast<usize>(object.Mesh)]) == true)
        {
            return;
        }
        const math::Aabb3& bounds = mMeshBoundsMesh[static_cast<usize>(object.Mesh)];
        StaticArray<math::Vector4, BOX_CORNERS> cornersClip{};
        for (usize corner = 0; corner < BOX_CORNERS; ++corner)
        {
            const math::Vector4 cornerMesh{(corner & 1U) != 0 ? bounds.Max.X : bounds.Min.X,
                                           (corner & 2U) != 0 ? bounds.Max.Y : bounds.Min.Y,
                                           (corner & 4U) != 0 ? bounds.Max.Z : bounds.Min.Z,
                                           1};
            cornersClip[corner] = meshToClip * cornerMesh;
        }
        // Each pair of corners that differ in exactly one axis bit is a box edge.
        for (usize first = 0; first < BOX_CORNERS; ++first)
        {
            for (usize bit = 1; bit < BOX_CORNERS; bit <<= 1U)
            {
                if ((first & bit) == 0)
                {
                    mOutlineSegments.Add({cornersClip[first], cornersClip[first | bit]});
                }
            }
        }
    }
    for (const SilhouetteSegment& segment : mOutlineSegments)
    {
        QLineF linePixel;
        if (inoutOverlay.OutlineLinesPixel.GetSize() < MAX_OUTLINE_LINES && projectSegment(segment, linePixel) == true)
        {
            // Admission above reserves every possible segment; Add cannot allocate here.
            inoutOverlay.OutlineLinesPixel.Add(linePixel);
        }
    }
}
bool ViewportObjectInteraction::refreshMeshBounds()
{
    const RenderScene& scene = mPreviewOrNull->Scene();
    if (scene.Identity == mMeshBoundsIdentity && mMeshBoundsMesh.GetSize() == static_cast<usize>(scene.Meshes.size()))
    {
        return true;
    }
    mMeshBoundsIdentity.clear();
    mMeshBoundsMesh.Clear();
    if (mMeshBoundsMesh.TryResize(static_cast<usize>(scene.Meshes.size())) == false)
    {
        return false;
    }
    for (int32 meshIndex = 0; meshIndex < scene.Meshes.size(); ++meshIndex)
    {
        const BorrowedView<const math::Vector3> positionsMesh = GetRenderSceneMeshPositions(scene, meshIndex);
        math::Aabb3 boundsMesh = math::EmptyAabb3();
        for (usize i = 0; i < positionsMesh.GetSize(); ++i)
        {
            const math::Vector3 positionMesh = positionsMesh[i];
            boundsMesh.Min = {math::Min(boundsMesh.Min.X, positionMesh.X),
                              math::Min(boundsMesh.Min.Y, positionMesh.Y),
                              math::Min(boundsMesh.Min.Z, positionMesh.Z)};
            boundsMesh.Max = {math::Max(boundsMesh.Max.X, positionMesh.X),
                              math::Max(boundsMesh.Max.Y, positionMesh.Y),
                              math::Max(boundsMesh.Max.Z, positionMesh.Z)};
        }
        if (math::IsEmpty(boundsMesh) == false)
        {
            const math::Vector3 extentMesh = boundsMesh.Max - boundsMesh.Min;
            const float32 padding =
                math::Max(math::Max(extentMesh.X, extentMesh.Y), extentMesh.Z) * PICK_BOUNDS_PADDING;
            boundsMesh.Min = boundsMesh.Min - math::Vector3{padding, padding, padding};
            boundsMesh.Max = boundsMesh.Max + math::Vector3{padding, padding, padding};
        }
        mMeshBoundsMesh[static_cast<usize>(meshIndex)] = boundsMesh;
    }
    mMeshBoundsIdentity = scene.Identity;
    return true;
}
QString ViewportObjectInteraction::Pick(QPointF positionPixel, const QRectF& outerFramePixel)
{
    math::Vector3 hitMesh;
    return pick(positionPixel, resolveFrame(outerFramePixel), hitMesh);
}
QString ViewportObjectInteraction::pick(QPointF positionPixel, const QRectF& framePixel, math::Vector3& outHitMesh)
{
    math::Matrix4 worldToClip;
    math::Matrix4 clipToWorld;
    if (framePixel.contains(positionPixel) == false || describe(framePixel, worldToClip) == false ||
        math::TryInverse(worldToClip, clipToWorld) != math::MathStatus::Success)
    {
        return {};
    }
    math::Ray3 rayWorld;
    if (TryMakePixelRay(clipToWorld, framePixel, positionPixel, rayWorld) == false)
    {
        return {};
    }
    float64 nearest = 0;
    bool hasHit = false;
    QString result;
    const RenderScene& scene = mPreviewOrNull->Scene();
    const bool hasMeshBounds = refreshMeshBounds();
    for (const PreviewObject& object : scene.Objects)
    {
        math::Affine3 worldToMesh;
        if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size() ||
            math::TryInverse(object.Transform, worldToMesh) != math::MathStatus::Success)
        {
            continue;
        }
        const math::Ray3 rayMesh{math::TransformPoint(worldToMesh, rayWorld.Origin),
                                 math::TransformVector(worldToMesh, rayWorld.Direction)};
        // An affine map preserves the ray parameter, so mesh-space box entry compares with the world hit.
        math::RayHit boundsHit;
        if (hasMeshBounds == true &&
            math::TryRayAabb(rayMesh,
                             mMeshBoundsMesh[static_cast<usize>(object.Mesh)],
                             {0, hasHit == true ? nearest : UNBOUNDED_PICK_DISTANCE},
                             boundsHit) == math::MathStatus::Success &&
            boundsHit.Result == math::HitResult::Miss)
        {
            continue;
        }
        const BorrowedView<const math::Vector3> positionsMesh = GetRenderSceneMeshPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetRenderSceneMeshIndices(scene, object.Mesh);
        for (usize index = 0; index + TRIANGLE_VERTICES <= indices.GetSize(); index += TRIANGLE_VERTICES)
        {
            const uint32 a = indices[index];
            const uint32 b = indices[index + 1];
            const uint32 c = indices[index + 2];
            float64 distance = 0;
            if (a >= positionsMesh.GetSize() || b >= positionsMesh.GetSize() || c >= positionsMesh.GetSize() ||
                IntersectRenderSceneTriangle(rayMesh, positionsMesh[a], positionsMesh[b], positionsMesh[c], distance) ==
                    false ||
                (hasHit == true && distance >= nearest))
            {
                continue;
            }
            const math::Vector3 hitWorld = rayWorld.Origin + rayWorld.Direction * static_cast<float32>(distance);
            const math::Vector4 clip = worldToClip * math::Vector4{hitWorld.X, hitWorld.Y, hitWorld.Z, 1};
            if (math::IsFinite(clip) == true && clip.W > 0 && clip.Z >= 0 && clip.Z <= clip.W)
            {
                nearest = distance;
                outHitMesh = rayMesh.Origin + rayMesh.Direction * static_cast<float32>(distance);
                result = object.Id;
                hasHit = true;
            }
        }
    }
    return result;
}
bool ViewportObjectInteraction::updateTerrainBrush(QPointF positionPixel, const QRectF& outerFramePixel)
{
    math::Vector3 hitMesh;
    const bool hasHit = mIsActive == true && mCanEdit == true &&
                        pick(positionPixel, resolveFrame(outerFramePixel), hitMesh) == mTerrainBrushKey;
    if (hasHit != mHasTerrainBrushHit || (hasHit == true && hitMesh != mTerrainBrushHitMesh))
    {
        mHasTerrainBrushHit = hasHit;
        mTerrainBrushHitMesh = hitMesh;
        Q_EMIT Changed();
    }
    return hasHit;
}
void ViewportObjectInteraction::appendTerrainStrokePoint()
{
    const QJsonArray point{static_cast<float64>(mTerrainBrushHitMesh.X), static_cast<float64>(mTerrainBrushHitMesh.Z)};
    if (mTerrainStroke.isEmpty() == false)
    {
        const QJsonArray previous = mTerrainStroke.last().toArray();
        const math::Vector2 differenceMesh{mTerrainBrushHitMesh.X - static_cast<float32>(previous[0].toDouble()),
                                           mTerrainBrushHitMesh.Z - static_cast<float32>(previous[1].toDouble())};
        constexpr float32 MINIMUM_POINT_SPACING_FRACTION = 0.25F;
        const float32 spacing = mTerrainBrushRadius * MINIMUM_POINT_SPACING_FRACTION;
        if (math::LengthSquared(differenceMesh) < spacing * spacing)
        {
            return;
        }
    }
    if (mTerrainStroke.size() == MAX_TERRAIN_STROKE_POINTS)
    {
        mHasTerrainStrokeOverflowed = true;
        return;
    }
    mTerrainStroke.append(point);
}
math::Affine3 ViewportObjectInteraction::makeTransform(const QJsonObject& placement) const
{
    // The World's own placement composition, so a released object stays where the cook puts it.
    return math::Compose(mParentToWorld, WorldDocument::MakeSceneMeshToParent(placement));
}
void ViewportObjectInteraction::buildHandles()
{
    mHandles.Clear();
    mPlaneHandles.Clear();
    mHasUniformHandle = false;
    if (mCanEdit == false || mTool == Tool::Select || mTerrainBrushKey.isEmpty() == false)
    {
        return;
    }
    const math::Affine3 meshToWorld = makeTransform(IsDragging() == true ? mCandidate : mPlacement);
    if (project(meshToWorld.Translation, mPivotPixel) == false)
    {
        return;
    }
    const math::Vector3 rightWorld = math::Rotate(mPreviewOrNull->GetVisibleCameraSample().Pose.Orientation, {1, 0, 0});
    QPointF rightPixel;
    if (project(meshToWorld.Translation + rightWorld, rightPixel) == false)
    {
        return;
    }
    const QPointF pixelPerMetre = rightPixel - mPivotPixel;
    const float64 pixelsPerMetre = math::Sqrt(QPointF::dotProduct(pixelPerMetre, pixelPerMetre));
    if (pixelsPerMetre < static_cast<float64>(PROJECTION_EPSILON))
    {
        return;
    }
    const float32 radiusWorld = static_cast<float32>(HANDLE_PIXELS / pixelsPerMetre);
    const math::Vector3 forwardWorld =
        math::Rotate(mPreviewOrNull->GetVisibleCameraSample().Pose.Orientation, {0, 0, -1});
    StaticArray<QPointF, AXIS_COUNT> axisUnitsPixel{};
    StaticArray<bool, AXIS_COUNT> hasAxisUnits{};
    for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        math::Affine3 axisFrameToWorld = meshToWorld;
        if (mTool == Tool::Rotate)
        {
            QJsonObject axisPlacement = IsDragging() == true ? mCandidate : mPlacement;
            QJsonArray rotation = axisPlacement.value(QStringLiteral("rotation")).toArray();
            // Euler XYZ adjustments compose Rz * Ry * Rx. Each handle follows the
            // derivative axis of its stored angle, including a rotated imported basis/parent.
            for (int32 preceding = 0; preceding < axis; ++preceding)
            {
                rotation[preceding] = 0;
            }
            axisPlacement.insert(QStringLiteral("rotation"), rotation);
            axisFrameToWorld = makeTransform(axisPlacement);
        }
        math::Vector3 directionWorld =
            mTool == Tool::Move ? mParentToWorld.Columns[axis] : axisFrameToWorld.Columns[axis];
        math::Vector3 unitWorld;
        if (math::TryNormalize(directionWorld, unitWorld) != math::MathStatus::Success)
        {
            continue;
        }
        QPointF endPixel;
        if (project(meshToWorld.Translation + unitWorld * radiusWorld, endPixel) == false)
        {
            continue;
        }
        const QPointF directionPixel = endPixel - mPivotPixel;
        const float64 length = math::Sqrt(QPointF::dotProduct(directionPixel, directionPixel));
        if (mTool != Tool::Rotate)
        {
            if (length >= MINIMUM_AXIS_PIXELS)
            {
                axisUnitsPixel[static_cast<usize>(axis)] = directionPixel / length;
                hasAxisUnits[static_cast<usize>(axis)] = true;
                if (mHandles.TryAdd(
                        {QLineF(mPivotPixel, mPivotPixel + directionPixel * (HANDLE_PIXELS / length)), axis, false}) ==
                    false)
                {
                    mHandles.Clear();
                    return;
                }
            }
        }
        else
        {
            const math::Vector3 firstWorld = axisFrameToWorld.Columns[(axis + 1) % AXIS_COUNT];
            math::Vector3 ringXWorld;
            math::Vector3 ringYWorld;
            if (math::TryNormalize(firstWorld, ringXWorld) != math::MathStatus::Success ||
                math::TryNormalize(math::Cross(unitWorld, ringXWorld), ringYWorld) != math::MathStatus::Success)
            {
                continue;
            }
            for (int32 segment = 0; segment < RING_SEGMENTS; ++segment)
            {
                const float32 angle1 = static_cast<float32>(segment) * 2 * math::kPiF / RING_SEGMENTS;
                const float32 angle2 = static_cast<float32>(segment + 1) * 2 * math::kPiF / RING_SEGMENTS;
                QPointF firstPixel;
                QPointF secondPixel;
                const math::Vector3 firstOffsetWorld =
                    (ringXWorld * math::Cos(angle1) + ringYWorld * math::Sin(angle1)) * radiusWorld;
                const math::Vector3 secondOffsetWorld =
                    (ringXWorld * math::Cos(angle2) + ringYWorld * math::Sin(angle2)) * radiusWorld;
                if (project(meshToWorld.Translation + firstOffsetWorld, firstPixel) == true &&
                    project(meshToWorld.Translation + secondOffsetWorld, secondPixel) == true)
                {
                    // The half of the ring farther along the view direction than the pivot is behind the object.
                    const bool isBack = math::Dot(firstOffsetWorld + secondOffsetWorld, forwardWorld) > 0;
                    if (mHandles.TryAdd({QLineF(firstPixel, secondPixel), axis, isBack}) == false)
                    {
                        mHandles.Clear();
                        return;
                    }
                }
            }
        }
    }
    if (mTool == Tool::Move)
    {
        buildPlaneHandles(axisUnitsPixel, hasAxisUnits, forwardWorld);
    }
    mHasUniformHandle = mTool == Tool::Scale;
}
void ViewportObjectInteraction::buildPlaneHandles(const StaticArray<QPointF, AXIS_COUNT>& axisUnitsPixel,
                                                  const StaticArray<bool, AXIS_COUNT>& hasAxisUnits,
                                                  math::Vector3 forwardWorld)
{
    for (int32 normalAxis = 0; normalAxis < AXIS_COUNT; ++normalAxis)
    {
        const PlaneAxes axes = PLANE_AXES[static_cast<usize>(normalAxis)];
        const usize first = static_cast<usize>(axes.First);
        const usize second = static_cast<usize>(axes.Second);
        math::Vector3 normalWorld;
        if (hasAxisUnits[first] == false || hasAxisUnits[second] == false ||
            math::TryNormalize(math::Cross(mParentToWorld.Columns[axes.First], mParentToWorld.Columns[axes.Second]),
                               normalWorld) != math::MathStatus::Success)
        {
            continue;
        }
        // A plane seen edge-on cannot be dragged in, and two axes lying along each other leave no square to show.
        const QPointF firstPixel = axisUnitsPixel[first];
        const QPointF secondPixel = axisUnitsPixel[second];
        const float64 sine = math::Abs(firstPixel.x() * secondPixel.y() - firstPixel.y() * secondPixel.x());
        if (math::Abs(math::Dot(normalWorld, forwardWorld)) < MINIMUM_PLANE_FACING || sine < MINIMUM_PLANE_SINE)
        {
            continue;
        }
        PlaneHandle plane;
        plane.NormalAxis = normalAxis;
        plane.CornersPixel = {mPivotPixel + firstPixel * PLANE_INNER_PIXELS + secondPixel * PLANE_INNER_PIXELS,
                              mPivotPixel + firstPixel * PLANE_OUTER_PIXELS + secondPixel * PLANE_INNER_PIXELS,
                              mPivotPixel + firstPixel * PLANE_OUTER_PIXELS + secondPixel * PLANE_OUTER_PIXELS,
                              mPivotPixel + firstPixel * PLANE_INNER_PIXELS + secondPixel * PLANE_OUTER_PIXELS};
        // Admission is bounded by the three planes of the tool; TryAdd cannot fail.
        static_cast<void>(mPlaneHandles.TryAdd(plane));
    }
}
math::Vector3 ViewportObjectInteraction::getTerrainHandleMesh(int32 axis) const noexcept
{
    const float32 extent = mDragTarget.Kind == HandleKind::TerrainExtent && mDragTarget.Index == axis
                               ? mTerrainDragExtent
                           : axis == 0 ? mTerrainExtent.X
                                       : mTerrainExtent.Y;
    return axis == 0 ? math::Vector3{extent * 0.5F, mTerrainBaseElevation, 0}
                     : math::Vector3{0, mTerrainBaseElevation, extent * 0.5F};
}

ViewportObjectInteraction::HandleTarget ViewportObjectInteraction::pickTerrainHandle(QPointF positionPixel) const
{
    if (mCanEdit == false || mTerrainExtent.X <= 0)
    {
        return {};
    }
    const math::Affine3 meshToWorld = makeTransform(mPlacement);
    for (const int32 axis : {0, 2})
    {
        QPointF handlePixel;
        if (project(math::TransformPoint(meshToWorld, getTerrainHandleMesh(axis)), handlePixel) == true)
        {
            const QPointF deltaPixel = positionPixel - handlePixel;
            if (QPointF::dotProduct(deltaPixel, deltaPixel) <= HANDLE_HIT_PIXELS * HANDLE_HIT_PIXELS)
            {
                return {HandleKind::TerrainExtent, axis};
            }
        }
    }
    return {};
}

bool ViewportObjectInteraction::beginTerrainDrag(QPointF positionPixel)
{
    const math::Affine3 meshToWorld = makeTransform(mPlacement);
    mTerrainDragExtent = mDragTarget.Index == 0 ? mTerrainExtent.X : mTerrainExtent.Y;
    mDragPlaneOriginWorld = math::TransformPoint(meshToWorld, {0, mTerrainBaseElevation, 0});
    math::Ray3 pressRay;
    return math::TryNormalize(math::Cross(meshToWorld.Columns[0], meshToWorld.Columns[2]), mDragPlaneNormalWorld) ==
               math::MathStatus::Success &&
           math::TryInverse(mWorldToClip, mDragClipToWorld) == math::MathStatus::Success &&
           TryMakePixelRay(mDragClipToWorld, mFramePixel, positionPixel, pressRay) == true &&
           TryIntersectPlane(pressRay, mDragPlaneOriginWorld, mDragPlaneNormalWorld, mDragPlaneStartWorld) == true;
}

void ViewportObjectInteraction::dragTerrainExtent(QPointF positionPixel)
{
    math::Ray3 rayWorld;
    math::Vector3 hitWorld;
    if (TryMakePixelRay(mDragClipToWorld, mFramePixel, positionPixel, rayWorld) == false ||
        TryIntersectPlane(rayWorld, mDragPlaneOriginWorld, mDragPlaneNormalWorld, hitWorld) == false)
    {
        return;
    }
    const math::Vector3 axisWorld = makeTransform(mPlacement).Columns[mDragTarget.Index];
    const float32 delta = math::Dot(hitWorld - mDragPlaneStartWorld, axisWorld) / math::Dot(axisWorld, axisWorld);
    const float32 initial = mDragTarget.Index == 0 ? mTerrainExtent.X : mTerrainExtent.Y;
    const float32 extent = initial + 2 * delta;
    if (math::IsFinite(extent) == true && extent > 0)
    {
        mTerrainDragExtent = extent;
        Q_EMIT Changed();
    }
}

void ViewportObjectInteraction::appendTerrainFootprint(ViewportObjectOverlay& inoutOverlay) const
{
    if (mTerrainExtent.X <= 0)
    {
        return;
    }
    const math::Affine3 meshToWorld = makeTransform(IsDragging() == true ? mCandidate : mPlacement);
    const float32 halfWidth = getTerrainHandleMesh(0).X;
    const float32 halfDepth = getTerrainHandleMesh(2).Z;
    const float32 elevation = mTerrainBaseElevation;
    constexpr usize CORNER_COUNT = 4;
    constexpr int32 DASH_COUNT = 10;
    const StaticArray<math::Vector3, CORNER_COUNT> cornersMesh{{{-halfWidth, elevation, -halfDepth},
                                                                {halfWidth, elevation, -halfDepth},
                                                                {halfWidth, elevation, halfDepth},
                                                                {-halfWidth, elevation, halfDepth}}};
    for (usize corner = 0; corner < CORNER_COUNT; ++corner)
    {
        QLineF linePixel;
        const math::Vector3 firstWorld = math::TransformPoint(meshToWorld, cornersMesh[corner]);
        const math::Vector3 secondWorld = math::TransformPoint(meshToWorld, cornersMesh[(corner + 1) % CORNER_COUNT]);
        const SilhouetteSegment segment
        {
            .FirstClip = mWorldToClip * math::Vector4{firstWorld.X, firstWorld.Y, firstWorld.Z, 1},
            .SecondClip = mWorldToClip * math::Vector4{secondWorld.X, secondWorld.Y, secondWorld.Z, 1},
        };
        if (projectSegment(segment, linePixel) == false)
        {
            continue;
        }
        for (int32 dash = 0; dash < DASH_COUNT; ++dash)
        {
            const float64 start = static_cast<float64>(dash) / DASH_COUNT;
            const float64 end = (static_cast<float64>(dash) + 0.5) / DASH_COUNT;
            inoutOverlay.OutlineLinesPixel.Add(QLineF(linePixel.pointAt(start), linePixel.pointAt(end)));
        }
    }
    for (const int32 axis : {0, 2})
    {
        QPointF positionPixel;
        if (project(math::TransformPoint(meshToWorld, getTerrainHandleMesh(axis)), positionPixel) == false)
        {
            continue;
        }
        const HandleTarget target{HandleKind::TerrainExtent, axis};
        const QPointF offset(HANDLE_HIT_PIXELS * 0.5, HANDLE_HIT_PIXELS * 0.5);
        const QPointF topRight(offset.x(), -offset.y());
        ViewportHandleFill fill;
        fill.CornerCount = ViewportHandleFill::MAX_CORNERS;
        fill.CornersPixel = {positionPixel - offset,
                             positionPixel + topRight,
                             positionPixel + offset,
                             positionPixel - topRight};
        fill.Color = getHandleColour(target, axis, false);
        static_cast<void>(inoutOverlay.HandleFills.TryAdd(fill));
    }
}

ViewportObjectInteraction::HandleTarget ViewportObjectInteraction::pickHandle(QPointF positionPixel)
{
    const HandleTarget terrain = pickTerrainHandle(positionPixel);
    if (terrain.IsValid() == true)
    {
        return terrain;
    }
    if (mHasUniformHandle == true && math::Abs(positionPixel.x() - mPivotPixel.x()) <= CENTRE_HIT_HALF_PIXELS &&
        math::Abs(positionPixel.y() - mPivotPixel.y()) <= CENTRE_HIT_HALF_PIXELS)
    {
        return {HandleKind::Uniform, 0};
    }
    float64 nearest = HANDLE_HIT_PIXELS * HANDLE_HIT_PIXELS;
    HandleTarget axisTarget;
    QPointF axisDirectionPixel;
    for (const HandleLine& handle : mHandles)
    {
        const float64 distance = ComputeDistanceSquared(positionPixel, handle.Line);
        if (distance < nearest)
        {
            nearest = distance;
            axisTarget = {HandleKind::Axis, handle.Axis};
            axisDirectionPixel = handle.Line.p2() - handle.Line.p1();
        }
    }
    // The thin shaft keeps its own pixels, so a plane handle never hides the line it sits beside.
    const bool isOnShaft = axisTarget.IsValid() == true && nearest <= SHAFT_PRIORITY_PIXELS * SHAFT_PRIORITY_PIXELS;
    if (isOnShaft == false)
    {
        float64 nearestCentre = HANDLE_HIT_PIXELS * HANDLE_HIT_PIXELS * AXIS_COUNT;
        HandleTarget planeTarget;
        for (const PlaneHandle& plane : mPlaneHandles)
        {
            if (IsInsideQuad(positionPixel, GrowQuad(plane.CornersPixel, PLANE_HIT_MARGIN_PIXELS)) == true)
            {
                const QPointF centrePixel =
                    (plane.CornersPixel[0] + plane.CornersPixel[1] + plane.CornersPixel[2] + plane.CornersPixel[3]) *
                    0.25;
                const float64 distance = QPointF::dotProduct(positionPixel - centrePixel, positionPixel - centrePixel);
                if (distance < nearestCentre)
                {
                    nearestCentre = distance;
                    planeTarget = {HandleKind::Plane, plane.NormalAxis};
                }
            }
        }
        if (planeTarget.IsValid() == true)
        {
            return planeTarget;
        }
    }
    if (axisTarget.IsValid() == true)
    {
        mAxisPixel = axisDirectionPixel;
    }
    return axisTarget;
}
bool ViewportObjectInteraction::Hover(QPointF positionPixel, const QRectF& outerFramePixel)
{
    if (mTerrainBrushKey.isEmpty() == false)
    {
        return updateTerrainBrush(positionPixel, outerFramePixel);
    }
    HandleTarget target;
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (IsDragging() == false && mIsActive == true && framePixel.contains(positionPixel) == true)
    {
        mFramePixel = framePixel;
        if (describe(framePixel, mWorldToClip) == true)
        {
            buildHandles();
            target = pickHandle(positionPixel);
        }
    }
    if (IsDragging() == true || target == mHoverTarget)
    {
        return mHoverTarget.IsValid();
    }
    mHoverTarget = target;
    Q_EMIT Changed();
    return target.IsValid();
}
void ViewportObjectInteraction::ClearHover()
{
    if (mHoverTarget.IsValid() == true || mHasTerrainBrushHit == true)
    {
        mHoverTarget = {};
        mHasTerrainBrushHit = false;
        Q_EMIT Changed();
    }
}
bool ViewportObjectInteraction::beginPlaneDrag(QPointF positionPixel)
{
    const PlaneAxes axes = PLANE_AXES[static_cast<usize>(mDragTarget.Index)];
    mDragPlaneFirstAxis = axes.First;
    mDragPlaneSecondAxis = axes.Second;
    mDragPlaneOriginWorld = makeTransform(mPlacement).Translation;
    math::Ray3 pressRay;
    // The drag keeps the projection it was pressed with, as the axis drags keep their pixel scale.
    return math::TryNormalize(math::Cross(mParentToWorld.Columns[axes.First], mParentToWorld.Columns[axes.Second]),
                              mDragPlaneNormalWorld) == math::MathStatus::Success &&
           math::TryInverse(mWorldToClip, mDragClipToWorld) == math::MathStatus::Success &&
           TryMakePixelRay(mDragClipToWorld, mFramePixel, positionPixel, pressRay) == true &&
           TryIntersectPlane(pressRay, mDragPlaneOriginWorld, mDragPlaneNormalWorld, mDragPlaneStartWorld) == true;
}
bool ViewportObjectInteraction::Press(QPointF positionPixel, const QRectF& outerFramePixel)
{
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (mIsActive == false || framePixel.contains(positionPixel) == false)
    {
        return false;
    }
    if (mTerrainBrushKey.isEmpty() == false)
    {
        if (updateTerrainBrush(positionPixel, outerFramePixel) == true)
        {
            if (mShouldPickTerrainHeight == true)
            {
                Q_EMIT TerrainHeightPicked(mTerrainBrushHitMesh.Y);
            }
            else
            {
                mTerrainStrokeFramePixel = outerFramePixel;
                mHasTerrainStrokeGap = false;
                mHasTerrainStrokeOverflowed = false;
                appendTerrainStrokePoint();
                Q_EMIT Changed();
            }
        }
        return true;
    }
    mFramePixel = framePixel;
    if (describe(framePixel, mWorldToClip) == false)
    {
        return false;
    }
    buildHandles();
    const HandleTarget target = pickHandle(positionPixel);
    if (target.IsValid() == true)
    {
        mDragTarget = target;
        mHoverTarget = {};
        mCandidate = mPlacement;
        mPressPixel = positionPixel;
        mReadoutText.clear();
        mReadoutAnchorPixel = positionPixel;
        mPixelsPerUnit = 0;
        QPointF unitPixel;
        const math::Affine3 meshToWorld = makeTransform(mPlacement);
        mRotationDirection =
            math::Dot(math::Cross(meshToWorld.Columns[0], meshToWorld.Columns[1]), meshToWorld.Columns[2]) < 0 ? -1 : 1;
        if (target.Kind == HandleKind::TerrainExtent)
        {
            if (beginTerrainDrag(positionPixel) == false)
            {
                Cancel();
                return true;
            }
        }
        else if (target.Kind == HandleKind::Plane && mTool == Tool::Move)
        {
            if (beginPlaneDrag(positionPixel) == false)
            {
                Cancel();
                return true;
            }
        }
        else if (target.Kind == HandleKind::Axis && mTool == Tool::Move &&
                 project(meshToWorld.Translation + mParentToWorld.Columns[target.Index], unitPixel) == true)
        {
            mPixelsPerUnit = math::Sqrt(QPointF::dotProduct(unitPixel - mPivotPixel, unitPixel - mPivotPixel));
        }
        if (target.Kind == HandleKind::Axis && mTool == Tool::Move &&
            mPixelsPerUnit < static_cast<float64>(PROJECTION_EPSILON))
        {
            Cancel();
            return true;
        }
        Q_EMIT Changed();
        return true;
    }
    Q_EMIT SelectionRequested(Pick(positionPixel, framePixel));
    return true;
}
bool ViewportObjectInteraction::dragAxis(QPointF deltaPixel, bool shouldSnap, QJsonArray& inoutValues)
{
    const int32 axis = mDragTarget.Index;
    const float64 axisLength = math::Sqrt(QPointF::dotProduct(mAxisPixel, mAxisPixel));
    const float64 distancePixel = QPointF::dotProduct(deltaPixel, mAxisPixel) / axisLength;
    const float64 original = inoutValues[axis].toDouble();
    float64 value = original;
    if (mTool == Tool::Move)
    {
        float64 delta = distancePixel / mPixelsPerUnit;
        if (shouldSnap == true)
        {
            delta = ComputeSnap(delta, MOVE_SNAP_METRES);
        }
        value += delta;
        mReadoutText = FormatMoveReadout({axis, delta}, {});
    }
    else if (mTool == Tool::Rotate)
    {
        float64 delta = distancePixel / PIXELS_PER_DEGREE * mRotationDirection;
        if (shouldSnap == true)
        {
            delta = ComputeSnap(delta, ROTATE_SNAP_DEGREES);
        }
        value += delta;
        value -= static_cast<float64>(static_cast<int64>(value / DEGREES_PER_TURN)) * DEGREES_PER_TURN;
        mReadoutText = FormatRotateReadout(axis, delta);
    }
    else
    {
        float64 factor = 1 + distancePixel / HANDLE_PIXELS;
        if (shouldSnap == true)
        {
            factor = ComputeSnap(factor, SCALE_SNAP);
        }
        factor = math::Max(factor, MINIMUM_SCALE_FACTOR);
        value = original * factor;
        mReadoutText = FormatScaleReadout(axis, factor);
    }
    if (math::IsFinite(value) == false)
    {
        return false;
    }
    inoutValues[axis] = value;
    return true;
}
bool ViewportObjectInteraction::dragPlane(QPointF positionPixel, bool shouldSnap, QJsonArray& inoutValues)
{
    math::Ray3 ray;
    math::Vector3 hitWorld;
    if (TryMakePixelRay(mDragClipToWorld, mFramePixel, positionPixel, ray) == false ||
        TryIntersectPlane(ray, mDragPlaneOriginWorld, mDragPlaneNormalWorld, hitWorld) == false)
    {
        return false;
    }
    // The pointer's travel in the plane, expressed in the parent-local axes the position is stored in.
    const math::Vector3 travelWorld = hitWorld - mDragPlaneStartWorld;
    const math::Vector3 firstWorld = mParentToWorld.Columns[mDragPlaneFirstAxis];
    const math::Vector3 secondWorld = mParentToWorld.Columns[mDragPlaneSecondAxis];
    const float64 firstFirst = static_cast<float64>(math::Dot(firstWorld, firstWorld));
    const float64 firstSecond = static_cast<float64>(math::Dot(firstWorld, secondWorld));
    const float64 secondSecond = static_cast<float64>(math::Dot(secondWorld, secondWorld));
    const float64 firstTravel = static_cast<float64>(math::Dot(firstWorld, travelWorld));
    const float64 secondTravel = static_cast<float64>(math::Dot(secondWorld, travelWorld));
    const float64 determinant = firstFirst * secondSecond - firstSecond * firstSecond;
    if (math::Abs(determinant) < MINIMUM_PLANE_DETERMINANT)
    {
        return false;
    }
    float64 firstDelta = (firstTravel * secondSecond - secondTravel * firstSecond) / determinant;
    float64 secondDelta = (secondTravel * firstFirst - firstTravel * firstSecond) / determinant;
    if (shouldSnap == true)
    {
        firstDelta = ComputeSnap(firstDelta, MOVE_SNAP_METRES);
        secondDelta = ComputeSnap(secondDelta, MOVE_SNAP_METRES);
    }
    const float64 firstValue = inoutValues[mDragPlaneFirstAxis].toDouble() + firstDelta;
    const float64 secondValue = inoutValues[mDragPlaneSecondAxis].toDouble() + secondDelta;
    if (math::IsFinite(firstValue) == false || math::IsFinite(secondValue) == false)
    {
        return false;
    }
    inoutValues[mDragPlaneFirstAxis] = firstValue;
    inoutValues[mDragPlaneSecondAxis] = secondValue;
    mReadoutText = FormatMoveReadout({mDragPlaneFirstAxis, firstDelta}, {mDragPlaneSecondAxis, secondDelta});
    return true;
}
bool ViewportObjectInteraction::dragUniform(QPointF deltaPixel, bool shouldSnap, QJsonArray& inoutValues)
{
    const float64 distancePixel = (deltaPixel.x() - deltaPixel.y()) * UNIFORM_DRAG_COMPONENT;
    float64 factor = 1 + distancePixel / HANDLE_PIXELS;
    if (shouldSnap == true)
    {
        factor = ComputeSnap(factor, SCALE_SNAP);
    }
    factor = math::Max(factor, MINIMUM_SCALE_FACTOR);
    QJsonArray scaled = inoutValues;
    for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float64 value = inoutValues[axis].toDouble() * factor;
        if (math::IsFinite(value) == false)
        {
            return false;
        }
        scaled[axis] = value;
    }
    inoutValues = scaled;
    mReadoutText = FormatScaleReadout(GIZMO_READOUT_NO_AXIS, factor);
    return true;
}
bool ViewportObjectInteraction::Move(QPointF positionPixel, bool shouldSnap)
{
    if (mTerrainStroke.isEmpty() == false)
    {
        const bool hasHit = updateTerrainBrush(positionPixel, mTerrainStrokeFramePixel);
        if (hasHit == false)
        {
            mHasTerrainStrokeGap = true;
        }
        else if (mHasTerrainStrokeGap == false && mHasTerrainStrokeOverflowed == false)
        {
            appendTerrainStrokePoint();
        }
        return true;
    }
    if (IsDragging() == false)
    {
        return false;
    }
    const QPointF deltaPixel = positionPixel - mPressPixel;
    if (math::IsFinite(deltaPixel.x()) == false || math::IsFinite(deltaPixel.y()) == false ||
        math::Abs(deltaPixel.x()) > MAXIMUM_GESTURE_PIXELS || math::Abs(deltaPixel.y()) > MAXIMUM_GESTURE_PIXELS)
    {
        return true;
    }
    const QString key = mTool == Tool::Move     ? QStringLiteral("position")
                        : mTool == Tool::Rotate ? QStringLiteral("rotation")
                                                : QStringLiteral("scale");
    QJsonArray values = mPlacement.value(key).toArray();
    bool isValueUsable = false;
    switch (mDragTarget.Kind)
    {
        case HandleKind::TerrainExtent:
            dragTerrainExtent(positionPixel);
            return true;
        case HandleKind::Plane:
            isValueUsable = dragPlane(positionPixel, shouldSnap, values);
            break;
        case HandleKind::Uniform:
            isValueUsable = dragUniform(deltaPixel, shouldSnap, values);
            break;
        case HandleKind::Axis:
            isValueUsable = dragAxis(deltaPixel, shouldSnap, values);
            break;
    }
    if (isValueUsable == false)
    {
        return true;
    }
    mReadoutAnchorPixel = positionPixel;
    mCandidate = mPlacement;
    mCandidate.insert(key, values);
    Q_EMIT Changed();
    return true;
}
bool ViewportObjectInteraction::Release()
{
    if (mTerrainStroke.isEmpty() == false)
    {
        const QJsonArray points = foundation::Move(mTerrainStroke);
        mTerrainStroke = {};
        if (mHasTerrainStrokeOverflowed == true)
        {
            Q_EMIT TerrainStrokeRejected(
                QStringLiteral("This stroke is too long. Split it into shorter Flatten strokes."));
        }
        else
        {
            Q_EMIT TerrainStrokeFinished(points);
        }
        Q_EMIT Changed();
        return true;
    }
    if (IsDragging() == false)
    {
        return false;
    }
    const HandleTarget target = mDragTarget;
    const QJsonObject candidate = mCandidate;
    mDragTarget = {};
    mReadoutText.clear();
    if (target.Kind == HandleKind::TerrainExtent)
    {
        const float32 initial = target.Index == 0 ? mTerrainExtent.X : mTerrainExtent.Y;
        if (mTerrainDragExtent != initial)
        {
            Q_EMIT TerrainExtentCommitted(target.Index == 0 ? QStringLiteral("width") : QStringLiteral("depth"),
                                          static_cast<float64>(mTerrainDragExtent));
        }
    }
    else if (candidate != mPlacement)
    {
        Q_EMIT PlacementCommitted(mSelected, candidate);
    }
    Q_EMIT Changed();
    return true;
}
void ViewportObjectInteraction::Cancel()
{
    ClearHover();
    if (IsDragging() == true)
    {
        mDragTarget = {};
        mTerrainStroke = {};
        mCandidate = {};
        mReadoutText.clear();
        Q_EMIT Changed();
    }
}
void ViewportObjectInteraction::BuildOverlay(const QRectF& outerFramePixel, ViewportObjectOverlay& outOverlay)
{
    mGpuOutline = {};
    buildOverlayGeometry(outerFramePixel, outOverlay);
    if (mIsGpuOutlineEnabled == true && (mHasReportedGpuOutline == false || mGpuOutline != mGpuOutlineReported))
    {
        mHasReportedGpuOutline = true;
        mGpuOutlineReported = mGpuOutline;
        Q_EMIT GpuOutlineChanged();
    }
}
void ViewportObjectInteraction::SetGpuOutlineEnabled(bool isEnabled)
{
    if (isEnabled == mIsGpuOutlineEnabled)
    {
        return;
    }
    mIsGpuOutlineEnabled = isEnabled;
    // Report afresh on the next build, and redraw with or without the CPU silhouette.
    mHasReportedGpuOutline = false;
    Q_EMIT Changed();
}
bool ViewportObjectInteraction::TryGetGpuOutlineRequest(SelectionOutlineRequest& outRequest) const
{
    if (mIsGpuOutlineEnabled == false || mGpuOutline.Mesh < 0 || mPreviewOrNull.isNull() == true)
    {
        return false;
    }
    const RenderScene& scene = mPreviewOrNull->Scene();
    if (scene.Identity != mGpuOutline.SceneIdentity || mGpuOutline.Mesh >= scene.Meshes.size())
    {
        return false;
    }
    outRequest.SceneIdentity = mGpuOutline.SceneIdentity;
    outRequest.Mesh = mGpuOutline.Mesh;
    outRequest.PositionsMesh = GetRenderSceneMeshPositions(scene, mGpuOutline.Mesh);
    outRequest.Indices = GetRenderSceneMeshIndices(scene, mGpuOutline.Mesh);
    outRequest.MeshToWorld = mGpuOutline.MeshToWorld;
    outRequest.StrokeScale = mGpuOutline.StrokeScale;
    const auto toVector = [](QRgb color, float32 alpha) {
        return math::Vector4{static_cast<float32>(qRed(color)) / COLOR_CHANNEL_MAXIMUM,
                             static_cast<float32>(qGreen(color)) / COLOR_CHANNEL_MAXIMUM,
                             static_cast<float32>(qBlue(color)) / COLOR_CHANNEL_MAXIMUM,
                             alpha};
    };
    outRequest.OutlineColor = toVector(ViewportObjectOverlay::SELECTION_COLOR, 1);
    outRequest.CasingColor =
        toVector(ViewportObjectOverlay::CASING_COLOR,
                 static_cast<float32>(qAlpha(ViewportObjectOverlay::CASING_COLOR)) / COLOR_CHANNEL_MAXIMUM);
    outRequest.OutlineWidth = ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS;
    outRequest.CasingWidth = ViewportObjectOverlay::CASING_WIDTH_PIXELS;
    return true;
}
void ViewportObjectInteraction::buildOverlayGeometry(const QRectF& outerFramePixel, ViewportObjectOverlay& outOverlay)
{
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayBuild);
    outOverlay.OutlineLinesPixel.Clear();
    outOverlay.HandleStrokes.Clear();
    outOverlay.HandleFills.Clear();
    outOverlay.ReadoutText.clear();
    outOverlay.FramePixel = {};
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (describe(framePixel, mWorldToClip) == false || mSelected.isEmpty() == true ||
        outOverlay.OutlineLinesPixel.TryEnsureCapacity(MAX_OUTLINE_LINES) == false ||
        outOverlay.HandleStrokes.TryEnsureCapacity(MAX_HANDLE_STROKES) == false ||
        outOverlay.HandleFills.TryEnsureCapacity(ViewportObjectOverlay::MAX_HANDLE_FILLS) == false)
    {
        return;
    }
    mFramePixel = framePixel;
    outOverlay.FramePixel = framePixel;
    if (IsDragging() == true)
    {
        outOverlay.ReadoutText = mReadoutText;
        outOverlay.ReadoutAnchorPixel = mReadoutAnchorPixel;
    }
    const RenderScene& scene = mPreviewOrNull->Scene();
    for (const PreviewObject& object : scene.Objects)
    {
        if (mTerrainExtent.X > 0 || object.Id != mSelected || object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            continue;
        }
        const math::Affine3 meshToWorld = mDragTarget.IsValid() == true ? makeTransform(mCandidate) : object.Transform;
        if (mIsGpuOutlineEnabled == true)
        {
            mGpuOutline.SceneIdentity = scene.Identity;
            mGpuOutline.Mesh = object.Mesh;
            mGpuOutline.MeshToWorld = meshToWorld;
            mGpuOutline.StrokeScale = mImageSize.isEmpty() == false && framePixel.width() > 0
                                          ? static_cast<float32>(mImageSize.width() / framePixel.width())
                                          : 1;
            continue;
        }
        appendObjectOutline(object, meshToWorld, outOverlay);
    }
    if (mLuminaireHalfExtentWorld > 0 && mPlacement.isEmpty() == false &&
        outOverlay.OutlineLinesPixel.GetSize() + LUMINAIRE_OUTLINE_LINES <= MAX_OUTLINE_LINES)
    {
        // The panel square in the transform's XY plane and its emission along -Z, one half extent long.
        const math::Affine3 panelToWorld = makeTransform(mDragTarget.IsValid() == true ? mCandidate : mPlacement);
        const float32 halfExtent = mLuminaireHalfExtentWorld;
        const StaticArray<math::Vector3, LUMINAIRE_OUTLINE_CORNERS> cornersPanel{{{-halfExtent, -halfExtent, 0},
                                                                                  {halfExtent, -halfExtent, 0},
                                                                                  {halfExtent, halfExtent, 0},
                                                                                  {-halfExtent, halfExtent, 0}}};
        StaticArray<QPointF, LUMINAIRE_OUTLINE_CORNERS> cornersPixel{};
        bool isProjected = true;
        for (usize corner = 0; corner < LUMINAIRE_OUTLINE_CORNERS; ++corner)
        {
            isProjected = isProjected == true && project(math::TransformPoint(panelToWorld, cornersPanel[corner]),
                                                         cornersPixel[corner]) == true;
        }
        QPointF centerPixel;
        QPointF emissionPixel;
        if (isProjected == true && project(math::TransformPoint(panelToWorld, {0, 0, 0}), centerPixel) == true &&
            project(math::TransformPoint(panelToWorld, {0, 0, -halfExtent}), emissionPixel) == true)
        {
            for (usize corner = 0; corner < LUMINAIRE_OUTLINE_CORNERS; ++corner)
            {
                outOverlay.OutlineLinesPixel.Add(
                    QLineF(cornersPixel[corner], cornersPixel[(corner + 1) % LUMINAIRE_OUTLINE_CORNERS]));
            }
            // The two diagonals join opposite corners.
            for (usize corner = 0; corner < LUMINAIRE_OUTLINE_CORNERS / 2; ++corner)
            {
                outOverlay.OutlineLinesPixel.Add(
                    QLineF(cornersPixel[corner], cornersPixel[corner + LUMINAIRE_OUTLINE_CORNERS / 2]));
            }
            outOverlay.OutlineLinesPixel.Add(QLineF(centerPixel, emissionPixel));
        }
    }
    buildHandles();
    for (const HandleLine& handle : mHandles)
    {
        appendHandleGeometry(handle, outOverlay);
    }
    for (const PlaneHandle& plane : mPlaneHandles)
    {
        appendPlaneGeometry(plane, outOverlay);
    }
    appendTerrainFootprint(outOverlay);
    appendTerrainBrush(outOverlay);
    if (mHasUniformHandle == true)
    {
        // The centre handle takes the pivot's place, so the scale tool has one marker at its origin.
        appendUniformGeometry(outOverlay);
    }
    else if (mHandles.IsEmpty() == false)
    {
        // The pivot marker shows where the transform acts; it stays above every shaft.
        ViewportHandleFill pivot;
        pivot.CornerCount = ViewportHandleFill::MAX_CORNERS;
        pivot.Color = PIVOT_COLOR;
        pivot.CornersPixel = {mPivotPixel + QPointF(-PIVOT_HALF_PIXELS, -PIVOT_HALF_PIXELS),
                              mPivotPixel + QPointF(PIVOT_HALF_PIXELS, -PIVOT_HALF_PIXELS),
                              mPivotPixel + QPointF(PIVOT_HALF_PIXELS, PIVOT_HALF_PIXELS),
                              mPivotPixel + QPointF(-PIVOT_HALF_PIXELS, PIVOT_HALF_PIXELS)};
        outOverlay.HandleFills.Add(pivot);
    }
}
void ViewportObjectInteraction::appendTerrainBrush(ViewportObjectOverlay& inoutOverlay) const
{
    if (mHasTerrainBrushHit == false)
    {
        return;
    }
    for (const PreviewObject& object : mPreviewOrNull->Scene().Objects)
    {
        if (object.Id != mTerrainBrushKey)
        {
            continue;
        }
        const float32 radius = mTerrainBrushRadius;
        math::Vector3 previousMesh = mTerrainBrushHitMesh + math::Vector3{radius, 0, 0};
        constexpr int32 BRUSH_SEGMENTS = 32;
        for (int32 segment = 1; segment <= BRUSH_SEGMENTS; ++segment)
        {
            const float32 angle = math::kTauF * static_cast<float32>(segment) / BRUSH_SEGMENTS;
            const math::Vector3 nextMesh =
                mTerrainBrushHitMesh + math::Vector3{radius * math::Cos(angle), 0, radius * math::Sin(angle)};
            QLineF linePixel;
            const math::Vector3 previousWorld = math::TransformPoint(object.Transform, previousMesh);
            const math::Vector3 nextWorld = math::TransformPoint(object.Transform, nextMesh);
            if (projectSegment({mWorldToClip * math::Vector4{previousWorld.X, previousWorld.Y, previousWorld.Z, 1},
                                mWorldToClip * math::Vector4{nextWorld.X, nextWorld.Y, nextWorld.Z, 1}},
                               linePixel) == true)
            {
                inoutOverlay.HandleStrokes.Add(
                    {linePixel, ViewportObjectOverlay::SELECTION_COLOR, ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS});
            }
            previousMesh = nextMesh;
        }
        return;
    }
}
QRgb ViewportObjectInteraction::getHandleColour(HandleTarget target, int32 colourAxis, bool isBack) const
{
    const bool isEmphasized = target == (IsDragging() == true ? mDragTarget : mHoverTarget);
    const bool isReceding = IsDragging() == true && isEmphasized == false;
    QRgb color = AXIS_COLORS[static_cast<usize>(colourAxis)];
    if (isEmphasized == true)
    {
        color = MixColour(color, EMPHASIS_COLOR, EMPHASIS_PERCENT);
    }
    else if (isReceding == true)
    {
        color = MixColour(color, RECEDE_COLOR, RECEDE_AXES_PERCENT);
    }
    else if (isBack == true)
    {
        color = MixColour(color, RECEDE_COLOR, RECEDE_BACK_PERCENT);
    }
    else if (target.Kind == HandleKind::Plane)
    {
        // At rest a plane square is quieter than the axis it sits between.
        color = MixColour(color, RECEDE_COLOR, PLANE_REST_PERCENT);
    }
    return color;
}
void ViewportObjectInteraction::appendPlaneGeometry(const PlaneHandle& plane, ViewportObjectOverlay& inoutOverlay) const
{
    ViewportHandleFill casing;
    casing.CornerCount = ViewportHandleFill::MAX_CORNERS;
    casing.Color = RECEDE_COLOR;
    casing.CornersPixel = GrowQuad(plane.CornersPixel, CASING_GROWTH_PIXELS);
    ViewportHandleFill square;
    square.CornerCount = ViewportHandleFill::MAX_CORNERS;
    square.Color = getHandleColour({HandleKind::Plane, plane.NormalAxis}, plane.NormalAxis, false);
    square.CornersPixel = plane.CornersPixel;
    inoutOverlay.HandleFills.Add(casing);
    inoutOverlay.HandleFills.Add(square);
}
void ViewportObjectInteraction::appendUniformGeometry(ViewportObjectOverlay& inoutOverlay) const
{
    const HandleTarget target{HandleKind::Uniform, 0};
    const bool isEmphasized = target == (IsDragging() == true ? mDragTarget : mHoverTarget);
    const QPointF halfPixel(CENTRE_HALF_PIXELS, CENTRE_HALF_PIXELS);
    ViewportHandleFill cube;
    cube.CornerCount = ViewportHandleFill::MAX_CORNERS;
    cube.Color = isEmphasized == true ? CENTRE_EMPHASIS_COLOR : CENTRE_COLOR;
    cube.CornersPixel = {mPivotPixel + QPointF(-halfPixel.x(), -halfPixel.y()),
                         mPivotPixel + QPointF(halfPixel.x(), -halfPixel.y()),
                         mPivotPixel + halfPixel,
                         mPivotPixel + QPointF(-halfPixel.x(), halfPixel.y())};
    ViewportHandleFill casing = cube;
    casing.Color = RECEDE_COLOR;
    casing.CornersPixel = GrowQuad(cube.CornersPixel, CASING_GROWTH_PIXELS);
    inoutOverlay.HandleFills.Add(casing);
    inoutOverlay.HandleFills.Add(cube);
}
void ViewportObjectInteraction::appendHandleGeometry(const HandleLine& handle,
                                                     ViewportObjectOverlay& inoutOverlay) const
{
    const HandleTarget target{HandleKind::Axis, handle.Axis};
    const bool isEmphasized = target == (IsDragging() == true ? mDragTarget : mHoverTarget);
    const QRgb color = getHandleColour(target, handle.Axis, handle.IsBack);
    const float32 widthPixels = static_cast<float32>(isEmphasized == true ? EMPHASIZED_HANDLE_WIDTH : HANDLE_WIDTH);
    inoutOverlay.HandleStrokes.Add({handle.Line, color, widthPixels});
    if (mTool == Tool::Rotate)
    {
        return;
    }
    const auto appendStroke = [&inoutOverlay, color](QPointF firstPixel, QPointF secondPixel) {
        inoutOverlay.HandleStrokes.Add({QLineF(firstPixel, secondPixel), color, 1});
    };
    // A move handle ends in an arrowhead and a scale handle in a cube, both solid so they read at 80 px.
    const QPointF tipPixel = handle.Line.p2();
    const QPointF directionPixel = tipPixel - handle.Line.p1();
    const float64 lengthPixels = math::Sqrt(QPointF::dotProduct(directionPixel, directionPixel));
    if (lengthPixels > 0)
    {
        const QPointF unitPixel = directionPixel / lengthPixels;
        const QPointF normalPixel(-unitPixel.y(), unitPixel.x());
        ViewportHandleFill head;
        head.Color = color;
        if (mTool == Tool::Move)
        {
            const QPointF basePixel = tipPixel - unitPixel * ARROW_LENGTH_PIXELS;
            head.CornerCount = ViewportHandleFill::TRIANGLE_CORNERS;
            head.CornersPixel = {tipPixel,
                                 basePixel + normalPixel * ARROW_HALF_WIDTH_PIXELS,
                                 basePixel - normalPixel * ARROW_HALF_WIDTH_PIXELS,
                                 tipPixel};
        }
        else
        {
            const QPointF alongPixel = unitPixel * CUBE_HALF_PIXELS;
            const QPointF acrossPixel = normalPixel * CUBE_HALF_PIXELS;
            head.CornerCount = ViewportHandleFill::MAX_CORNERS;
            head.CornersPixel = {tipPixel - alongPixel - acrossPixel,
                                 tipPixel + alongPixel - acrossPixel,
                                 tipPixel + alongPixel + acrossPixel,
                                 tipPixel - alongPixel + acrossPixel};
        }
        inoutOverlay.HandleFills.Add(head);
    }
    // Small stroke glyphs stay geometry in the native overlay, avoiding a full-frame text texture.
    const QPointF topLeftPixel = tipPixel + QPointF(LABEL_OFFSET, -GLYPH_HEIGHT_PIXELS);
    const QPointF topRightPixel = topLeftPixel + QPointF(GLYPH_WIDTH_PIXELS, 0);
    const QPointF bottomLeftPixel = topLeftPixel + QPointF(0, GLYPH_HEIGHT_PIXELS);
    const QPointF bottomRightPixel = topLeftPixel + QPointF(GLYPH_WIDTH_PIXELS, GLYPH_HEIGHT_PIXELS);
    if (handle.Axis == 0)
    {
        appendStroke(topLeftPixel, bottomRightPixel);
        appendStroke(topRightPixel, bottomLeftPixel);
    }
    else if (handle.Axis == 1)
    {
        const QPointF middlePixel = (topLeftPixel + bottomRightPixel) * 0.5;
        appendStroke(topLeftPixel, middlePixel);
        appendStroke(topRightPixel, middlePixel);
        appendStroke(middlePixel, (bottomLeftPixel + bottomRightPixel) * 0.5);
    }
    else
    {
        appendStroke(topLeftPixel, topRightPixel);
        appendStroke(topRightPixel, bottomLeftPixel);
        appendStroke(bottomLeftPixel, bottomRightPixel);
    }
}
void ViewportObjectInteraction::Paint(QPainter& inoutPainter, const QRectF& outerFramePixel)
{
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayPaint);
    BuildOverlay(outerFramePixel, mPaintOverlay);
    inoutPainter.save();
    inoutPainter.setClipRect(mPaintOverlay.FramePixel);
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    // The same casing and amber strokes as the native overlay, with round joins and caps.
    QPen casingPen(QColor::fromRgba(ViewportObjectOverlay::CASING_COLOR),
                   static_cast<float64>(ViewportObjectOverlay::CASING_WIDTH_PIXELS));
    casingPen.setCapStyle(Qt::RoundCap);
    inoutPainter.setPen(casingPen);
    inoutPainter.drawLines(mPaintOverlay.OutlineLinesPixel.GetData(),
                           static_cast<int32>(mPaintOverlay.OutlineLinesPixel.GetSize()));
    QPen outlinePen(QColor::fromRgb(ViewportObjectOverlay::SELECTION_COLOR),
                    static_cast<float64>(ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS));
    outlinePen.setCapStyle(Qt::RoundCap);
    inoutPainter.setPen(outlinePen);
    inoutPainter.drawLines(mPaintOverlay.OutlineLinesPixel.GetData(),
                           static_cast<int32>(mPaintOverlay.OutlineLinesPixel.GetSize()));
    for (const ViewportHandleStroke& stroke : mPaintOverlay.HandleStrokes)
    {
        inoutPainter.setPen(QPen(QColor::fromRgb(stroke.Color), static_cast<float64>(stroke.WidthPixels)));
        inoutPainter.drawLine(stroke.LinePixel);
    }
    inoutPainter.setPen(Qt::NoPen);
    for (const ViewportHandleFill& fill : mPaintOverlay.HandleFills)
    {
        inoutPainter.setBrush(QColor::fromRgb(fill.Color));
        inoutPainter.drawPolygon(fill.CornersPixel.GetData(), static_cast<int32>(fill.CornerCount));
    }
    if (mPaintOverlay.ReadoutText.isEmpty() == false)
    {
        PaintGizmoReadout(inoutPainter,
                          mPaintOverlay.ReadoutText,
                          PlaceGizmoReadout(mPaintOverlay.ReadoutAnchorPixel,
                                            MeasureGizmoReadout(mPaintOverlay.ReadoutText),
                                            mPaintOverlay.FramePixel));
    }
    inoutPainter.restore();
}
} // namespace ludus::editor
