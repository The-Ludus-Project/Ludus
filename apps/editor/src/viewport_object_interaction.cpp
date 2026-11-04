#include "internal/viewport_object_interaction.h"
#include "design_tokens.h"
#include "internal/render_preview.h"
#include "internal/world_document.h"

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
constexpr usize MAX_WIRE_LINES = ViewportObjectOverlay::MAX_WIRE_LINES;
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

/// Axis colours from the design tokens ("always with the letter X/Y/Z", hence the glyph at each tip).
constexpr StaticArray<QRgb, AXIS_COUNT> AXIS_COLORS{design::GetThemeArgb(design::ThemeColour::AxisX, true),
                                                    design::GetThemeArgb(design::ThemeColour::AxisY, true),
                                                    design::GetThemeArgb(design::ThemeColour::AxisZ, true)};
/// Colour that receding handles fade towards, so they never turn translucent over the frame.
constexpr QRgb RECEDE_COLOR = design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true);
constexpr QRgb EMPHASIS_COLOR = design::GetThemeArgb(design::ThemeColour::TextHud, true);
constexpr QRgb PIVOT_COLOR = design::GetThemeArgb(design::ThemeColour::TextHud, true);
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
bool ViewportObjectInteraction::project(math::Vector3 positionWorld, QPointF& outPositionPixel) const
{
    const math::Vector4 clip = mWorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
    if (math::IsFinite(clip) == false || clip.W <= PROJECTION_EPSILON || clip.Z < 0 || clip.Z > clip.W)
    {
        return false;
    }
    outPositionPixel = {mFramePixel.left() + (static_cast<float64>(clip.X / clip.W) + 1) * mFramePixel.width() * 0.5,
                        mFramePixel.top() + (1 - static_cast<float64>(clip.Y / clip.W)) * mFramePixel.height() * 0.5};
    return true;
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
    const QRectF framePixel = resolveFrame(outerFramePixel);
    math::Matrix4 worldToClip;
    math::Matrix4 clipToWorld;
    if (framePixel.contains(positionPixel) == false || describe(framePixel, worldToClip) == false ||
        math::TryInverse(worldToClip, clipToWorld) != math::MathStatus::Success)
    {
        return {};
    }
    const float32 x = static_cast<float32>((positionPixel.x() - framePixel.left()) / framePixel.width() * 2 - 1);
    const float32 y = static_cast<float32>(1 - (positionPixel.y() - framePixel.top()) / framePixel.height() * 2);
    math::Vector3 nearWorld;
    math::Vector3 middleWorld;
    if (math::TryPerspectiveDivide(clipToWorld * math::Vector4{x, y, 1, 1}, 0, nearWorld) !=
            math::MathStatus::Success ||
        math::TryPerspectiveDivide(clipToWorld * math::Vector4{x, y, 0.5F, 1}, 0, middleWorld) !=
            math::MathStatus::Success)
    {
        return {};
    }
    const math::Ray3 rayWorld{nearWorld, middleWorld - nearWorld};
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
                result = object.Id;
                hasHit = true;
            }
        }
    }
    return result;
}
math::Affine3 ViewportObjectInteraction::makeTransform(const QJsonObject& placement) const
{
    // The World's own placement composition, so a released object stays where the cook puts it.
    return math::Compose(mParentToWorld, WorldDocument::MakeSceneMeshToParent(placement));
}
void ViewportObjectInteraction::buildHandles()
{
    mHandles.Clear();
    if (mCanEdit == false || mTool == Tool::Select)
    {
        return;
    }
    const math::Affine3 meshToWorld = makeTransform(mDragAxis >= 0 ? mCandidate : mPlacement);
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
    for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        math::Affine3 axisFrameToWorld = meshToWorld;
        if (mTool == Tool::Rotate)
        {
            QJsonObject axisPlacement = mDragAxis >= 0 ? mCandidate : mPlacement;
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
}
int32 ViewportObjectInteraction::pickHandle(QPointF positionPixel)
{
    float64 nearest = HANDLE_HIT_PIXELS * HANDLE_HIT_PIXELS;
    int32 axis = -1;
    for (const HandleLine& handle : mHandles)
    {
        const float64 distance = ComputeDistanceSquared(positionPixel, handle.Line);
        if (distance < nearest)
        {
            nearest = distance;
            axis = handle.Axis;
            mAxisPixel = handle.Line.p2() - handle.Line.p1();
        }
    }
    return axis;
}
bool ViewportObjectInteraction::Hover(QPointF positionPixel, const QRectF& outerFramePixel)
{
    int32 axis = -1;
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (mDragAxis < 0 && mIsActive == true && framePixel.contains(positionPixel) == true)
    {
        mFramePixel = framePixel;
        if (describe(framePixel, mWorldToClip) == true)
        {
            buildHandles();
            axis = pickHandle(positionPixel);
        }
    }
    if (mDragAxis >= 0 || axis == mHoverAxis)
    {
        return mHoverAxis >= 0;
    }
    mHoverAxis = axis;
    Q_EMIT Changed();
    return axis >= 0;
}
void ViewportObjectInteraction::ClearHover()
{
    if (mHoverAxis >= 0)
    {
        mHoverAxis = -1;
        Q_EMIT Changed();
    }
}
bool ViewportObjectInteraction::Press(QPointF positionPixel, const QRectF& outerFramePixel)
{
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (mIsActive == false || framePixel.contains(positionPixel) == false)
    {
        return false;
    }
    mFramePixel = framePixel;
    if (describe(framePixel, mWorldToClip) == false)
    {
        return false;
    }
    buildHandles();
    const int32 axis = pickHandle(positionPixel);
    if (axis >= 0)
    {
        mDragAxis = axis;
        mHoverAxis = -1;
        mCandidate = mPlacement;
        mPressPixel = positionPixel;
        mPixelsPerUnit = 0;
        const math::Vector3 directionWorld = mParentToWorld.Columns[axis];
        QPointF unitPixel;
        const math::Affine3 meshToWorld = makeTransform(mPlacement);
        mRotationDirection =
            math::Dot(math::Cross(meshToWorld.Columns[0], meshToWorld.Columns[1]), meshToWorld.Columns[2]) < 0 ? -1 : 1;
        if (mTool == Tool::Move && project(meshToWorld.Translation + directionWorld, unitPixel) == true)
        {
            mPixelsPerUnit = math::Sqrt(QPointF::dotProduct(unitPixel - mPivotPixel, unitPixel - mPivotPixel));
        }
        if (mTool == Tool::Move && mPixelsPerUnit < static_cast<float64>(PROJECTION_EPSILON))
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
bool ViewportObjectInteraction::Move(QPointF positionPixel, bool shouldSnap)
{
    if (mDragAxis < 0)
    {
        return false;
    }
    const QPointF deltaPixel = positionPixel - mPressPixel;
    if (math::IsFinite(deltaPixel.x()) == false || math::IsFinite(deltaPixel.y()) == false ||
        math::Abs(deltaPixel.x()) > MAXIMUM_GESTURE_PIXELS || math::Abs(deltaPixel.y()) > MAXIMUM_GESTURE_PIXELS)
    {
        return true;
    }
    const float64 axisLength = math::Sqrt(QPointF::dotProduct(mAxisPixel, mAxisPixel));
    const float64 distancePixel = QPointF::dotProduct(deltaPixel, mAxisPixel) / axisLength;
    const QString key = mTool == Tool::Move     ? QStringLiteral("position")
                        : mTool == Tool::Rotate ? QStringLiteral("rotation")
                                                : QStringLiteral("scale");
    QJsonArray values = mPlacement.value(key).toArray();
    const float64 original = values[mDragAxis].toDouble();
    float64 value = original;
    if (mTool == Tool::Move)
    {
        float64 delta = distancePixel / mPixelsPerUnit;
        if (shouldSnap == true)
        {
            delta = ComputeSnap(delta, MOVE_SNAP_METRES);
        }
        value += delta;
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
    }
    else
    {
        float64 factor = 1 + distancePixel / HANDLE_PIXELS;
        if (shouldSnap == true)
        {
            factor = ComputeSnap(factor, SCALE_SNAP);
        }
        value = original * math::Max(factor, MINIMUM_SCALE_FACTOR);
    }
    if (math::IsFinite(value) == false)
    {
        return true;
    }
    values[mDragAxis] = value;
    mCandidate = mPlacement;
    mCandidate.insert(key, values);
    Q_EMIT Changed();
    return true;
}
bool ViewportObjectInteraction::Release()
{
    if (mDragAxis < 0)
    {
        return false;
    }
    const QJsonObject candidate = mCandidate;
    mDragAxis = -1;
    if (candidate != mPlacement)
    {
        Q_EMIT PlacementCommitted(mSelected, candidate);
    }
    Q_EMIT Changed();
    return true;
}
void ViewportObjectInteraction::Cancel()
{
    ClearHover();
    if (mDragAxis >= 0)
    {
        mDragAxis = -1;
        mCandidate = {};
        Q_EMIT Changed();
    }
}
void ViewportObjectInteraction::BuildOverlay(const QRectF& outerFramePixel, ViewportObjectOverlay& outOverlay)
{
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayBuild);
    outOverlay.WireLinesPixel.Clear();
    outOverlay.HandleStrokes.Clear();
    outOverlay.HandleFills.Clear();
    outOverlay.FramePixel = {};
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (describe(framePixel, mWorldToClip) == false || mSelected.isEmpty() == true ||
        outOverlay.WireLinesPixel.TryEnsureCapacity(MAX_WIRE_LINES) == false ||
        outOverlay.HandleStrokes.TryEnsureCapacity(MAX_HANDLE_STROKES) == false ||
        outOverlay.HandleFills.TryEnsureCapacity(ViewportObjectOverlay::MAX_HANDLE_FILLS) == false)
    {
        return;
    }
    mFramePixel = framePixel;
    outOverlay.FramePixel = framePixel;
    const RenderScene& scene = mPreviewOrNull->Scene();
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id != mSelected || object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            continue;
        }
        const math::Affine3 meshToWorld = mDragAxis >= 0 ? makeTransform(mCandidate) : object.Transform;
        const BorrowedView<const math::Vector3> positionsMesh = GetRenderSceneMeshPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetRenderSceneMeshIndices(scene, object.Mesh);
        const usize triangleCount = indices.GetSize() / TRIANGLE_VERTICES;
        const usize triangleStride =
            math::Max(usize{1},
                      triangleCount / ViewportObjectOverlay::MAX_WIRE_TRIANGLES +
                          (triangleCount % ViewportObjectOverlay::MAX_WIRE_TRIANGLES > 0 ? usize{1} : usize{0}));
        const usize stride = triangleStride * TRIANGLE_VERTICES;
        for (usize index = 0; index + TRIANGLE_VERTICES <= indices.GetSize() &&
                              outOverlay.WireLinesPixel.GetSize() + TRIANGLE_VERTICES <= MAX_WIRE_LINES;
             index += stride)
        {
            for (usize edge = 0; edge < TRIANGLE_VERTICES; ++edge)
            {
                const uint32 first = indices[index + edge];
                const uint32 second = indices[index + (edge + 1) % TRIANGLE_VERTICES];
                QPointF firstPixel;
                QPointF secondPixel;
                if (first < positionsMesh.GetSize() && second < positionsMesh.GetSize() &&
                    project(math::TransformPoint(meshToWorld, positionsMesh[first]), firstPixel) == true &&
                    project(math::TransformPoint(meshToWorld, positionsMesh[second]), secondPixel) == true)
                {
                    // Admission above reserves every possible edge; Add cannot allocate here.
                    outOverlay.WireLinesPixel.Add(QLineF(firstPixel, secondPixel));
                }
            }
        }
    }
    if (mLuminaireHalfExtentWorld > 0 && mPlacement.isEmpty() == false &&
        outOverlay.WireLinesPixel.GetSize() + LUMINAIRE_OUTLINE_LINES <= MAX_WIRE_LINES)
    {
        // The panel square in the transform's XY plane and its emission along -Z, one half extent long.
        const math::Affine3 panelToWorld = makeTransform(mDragAxis >= 0 ? mCandidate : mPlacement);
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
                outOverlay.WireLinesPixel.Add(
                    QLineF(cornersPixel[corner], cornersPixel[(corner + 1) % LUMINAIRE_OUTLINE_CORNERS]));
            }
            // The two diagonals join opposite corners.
            for (usize corner = 0; corner < LUMINAIRE_OUTLINE_CORNERS / 2; ++corner)
            {
                outOverlay.WireLinesPixel.Add(
                    QLineF(cornersPixel[corner], cornersPixel[corner + LUMINAIRE_OUTLINE_CORNERS / 2]));
            }
            outOverlay.WireLinesPixel.Add(QLineF(centerPixel, emissionPixel));
        }
    }
    buildHandles();
    for (const HandleLine& handle : mHandles)
    {
        appendHandleGeometry(handle, outOverlay);
    }
    if (mHandles.IsEmpty() == false)
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
void ViewportObjectInteraction::appendHandleGeometry(const HandleLine& handle,
                                                     ViewportObjectOverlay& inoutOverlay) const
{
    const bool isEmphasized = handle.Axis == mDragAxis || (mDragAxis < 0 && handle.Axis == mHoverAxis);
    const bool isReceding = mDragAxis >= 0 && handle.Axis != mDragAxis;
    QRgb color = AXIS_COLORS[static_cast<usize>(handle.Axis)];
    if (isEmphasized == true)
    {
        color = MixColour(color, EMPHASIS_COLOR, EMPHASIS_PERCENT);
    }
    else if (isReceding == true)
    {
        color = MixColour(color, RECEDE_COLOR, RECEDE_AXES_PERCENT);
    }
    else if (handle.IsBack == true)
    {
        color = MixColour(color, RECEDE_COLOR, RECEDE_BACK_PERCENT);
    }
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
    inoutPainter.setPen(QPen(QColor::fromRgb(ViewportObjectOverlay::SELECTION_COLOR),
                             static_cast<float64>(ViewportObjectOverlay::WIRE_WIDTH_PIXELS)));
    inoutPainter.drawLines(mPaintOverlay.WireLinesPixel.GetData(),
                           static_cast<int32>(mPaintOverlay.WireLinesPixel.GetSize()));
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
    inoutPainter.restore();
}
} // namespace ludus::editor
