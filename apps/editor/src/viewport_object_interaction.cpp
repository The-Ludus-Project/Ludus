#include "internal/viewport_object_interaction.h"
#include "internal/render_preview.h"
#include "internal/world_document.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QColor>
#include <QJsonArray>
#include <QPainter>
#include <QPen>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr int32 AXIS_COUNT = 3;
constexpr int32 TRIANGLE_VERTICES = 3;
constexpr int32 RING_SEGMENTS = 64;
constexpr int32 MAX_WIRE_TRIANGLES = 12000;
constexpr float32 PROJECTION_EPSILON = 0.000001F;
constexpr float64 HANDLE_PIXELS = 80;
constexpr float64 HANDLE_HIT_PIXELS = 9;
constexpr float64 MINIMUM_AXIS_PIXELS = 8;
constexpr float64 HANDLE_TIP_PIXELS = 5;
constexpr float64 MOVE_SNAP_METRES = 0.1;
constexpr float64 ROTATE_SNAP_DEGREES = 15;
constexpr float64 SCALE_SNAP = 0.1;
constexpr float64 MINIMUM_SCALE = 0.000001;
constexpr float64 MAXIMUM_SCALE = 1000000;
constexpr float64 DEGREES_PER_TURN = 360;
constexpr float64 PIXELS_PER_DEGREE = HANDLE_PIXELS * math::DegreesToRadians(1.0);
constexpr float64 WIRE_WIDTH = 1.5;
constexpr float64 HANDLE_WIDTH = 3;
constexpr float64 ACTIVE_HANDLE_WIDTH = 4;
constexpr float64 LABEL_OFFSET = 12;
constexpr float64 MAXIMUM_GESTURE_PIXELS = 10000000;

constexpr StaticArray<QRgb, AXIS_COUNT> AXIS_COLORS{0xffff6060, 0xff68df85, 0xff639fff};
constexpr QRgb SELECTION_COLOR = 0xffffbd59;
math::Vector3 ReadVector(const QJsonArray& values) noexcept
{
    return {static_cast<float32>(values[0].toDouble()),
            static_cast<float32>(values[1].toDouble()),
            static_cast<float32>(values[2].toDouble())};
}
BorrowedView<const math::Vector3> GetPositions(const RenderScene& scene, int32 meshIndex) noexcept
{
    if (scene.Definition != nullptr)
    {
        const resources::SceneMesh& mesh = scene.Definition->Meshes[static_cast<usize>(meshIndex)];
        return {mesh.Positions.GetData(), mesh.Positions.GetSize()};
    }
    const PreviewMesh& mesh = scene.Meshes[meshIndex];
    return {mesh.Positions.constData(), static_cast<usize>(mesh.Positions.size())};
}
BorrowedView<const uint32> GetIndices(const RenderScene& scene, int32 meshIndex) noexcept
{
    if (scene.Definition != nullptr)
    {
        const resources::SceneMesh& mesh = scene.Definition->Meshes[static_cast<usize>(meshIndex)];
        return {mesh.Indices.GetData(), mesh.Indices.GetSize()};
    }
    const PreviewMesh& mesh = scene.Meshes[meshIndex];
    return {mesh.Indices.constData(), static_cast<usize>(mesh.Indices.size())};
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
bool IntersectTriangle(const math::Ray3& ray,
                       math::Vector3 a,
                       math::Vector3 b,
                       math::Vector3 c,
                       float64& outDistance) noexcept
{
    // Mesh units need not match world metres. Double products avoid a fixed
    // determinant threshold rejecting tiny meshes whose instance scales them up.
    const auto widen = [](math::Vector3 value) noexcept -> math::Vector3d {
        return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
    };
    const math::Vector3d edge1 = widen(b) - widen(a);
    const math::Vector3d edge2 = widen(c) - widen(a);
    const math::Vector3d cross = math::Cross(widen(ray.Direction), edge2);
    const float64 determinant = math::Dot(edge1, cross);
    if (determinant == 0 || math::IsFinite(determinant) == false)
    {
        return false;
    }
    const math::Vector3d offset = widen(ray.Origin) - widen(a);
    const float64 u = math::Dot(offset, cross) / determinant;
    const math::Vector3d secondCross = math::Cross(offset, edge1);
    const float64 v = math::Dot(widen(ray.Direction), secondCross) / determinant;
    const float64 distance = math::Dot(edge2, secondCross) / determinant;
    if (u < 0 || v < 0 || u + v > 1 || distance < 0 || math::IsFinite(distance) == false)
    {
        return false;
    }
    outDistance = distance;
    return true;
}
} // namespace
ViewportObjectInteraction::ViewportObjectInteraction(RenderPreview& inoutPreview, QObject* parentOrNull)
    : QObject(parentOrNull), mPreviewOrNull(&inoutPreview)
{
    connect(&inoutPreview, &RenderPreview::NavigationChanged, this, &ViewportObjectInteraction::Changed);
    connect(&inoutPreview, &RenderPreview::Changed, this, &ViewportObjectInteraction::Changed);
    connect(&inoutPreview, &QObject::destroyed, this, [this]() {
        Cancel();
        mCanEdit = false;
        Q_EMIT Changed();
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
    if (mPlacement.isEmpty() == true)
    {
        const QJsonObject binding = mPreviewOrNull->Scene().TransformBinding;
        const QJsonObject values = world.Entity(selected)
                                       .value(QStringLiteral("components"))
                                       .toObject()
                                       .value(binding.value(QStringLiteral("component")).toString())
                                       .toObject();
        if (binding.isEmpty() == false && values.isEmpty() == false)
        {
            mPlacement.insert(QStringLiteral("basis"), QJsonArray{1, 0, 0, 0, 1, 0, 0, 0, 1});
            mPlacement.insert(QStringLiteral("parent"), QString());
            const QStringList keys{QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")};
            const QStringList prefixes{QString(), QStringLiteral("r"), QStringLiteral("s")};
            for (int32 kind = 0; kind < AXIS_COUNT; ++kind)
            {
                QJsonArray axes;
                for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
                {
                    const QString name = prefixes[kind] + QChar('x' + axis);
                    axes.append(values.value(binding.value(name).toString()));
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
    outWorldToClip = view.WorldToClip;
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
QString ViewportObjectInteraction::Pick(QPointF positionPixel, const QRectF& outerFramePixel) const
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
        const BorrowedView<const math::Vector3> positions = GetPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetIndices(scene, object.Mesh);
        for (usize index = 0; index + TRIANGLE_VERTICES <= indices.GetSize(); index += TRIANGLE_VERTICES)
        {
            const uint32 a = indices[index];
            const uint32 b = indices[index + 1];
            const uint32 c = indices[index + 2];
            float64 distance = 0;
            if (a >= positions.GetSize() || b >= positions.GetSize() || c >= positions.GetSize() ||
                IntersectTriangle(rayMesh, positions[a], positions[b], positions[c], distance) == false ||
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
    math::Affine3 basis;
    const QJsonArray values = placement.value(QStringLiteral("basis")).toArray();
    for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const int32 offset = axis * AXIS_COUNT;
        basis.Columns[axis] = {static_cast<float32>(values[offset].toDouble()),
                               static_cast<float32>(values[offset + 1].toDouble()),
                               static_cast<float32>(values[offset + 2].toDouble())};
    }
    const math::Vector3 rotation = ReadVector(placement.value(QStringLiteral("rotation")).toArray());
    math::Quaternion rotationX;
    math::Quaternion rotationY;
    math::Quaternion rotationZ;
    static_cast<void>(math::TryFromAxisAngle({1, 0, 0}, math::DegreesToRadians(rotation.X), rotationX));
    static_cast<void>(math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(rotation.Y), rotationY));
    static_cast<void>(math::TryFromAxisAngle({0, 0, 1}, math::DegreesToRadians(rotation.Z), rotationZ));
    math::TransformTRS adjustment;
    static_cast<void>(math::TryNormalize(rotationZ * rotationY * rotationX, adjustment.Rotation));
    adjustment.Scale = ReadVector(placement.value(QStringLiteral("scale")).toArray());
    math::Affine3 meshToParent = math::Compose(basis, math::ToAffine3(adjustment));
    meshToParent.Translation = ReadVector(placement.value(QStringLiteral("position")).toArray());
    return math::Compose(mParentToWorld, meshToParent);
}
void ViewportObjectInteraction::buildHandles()
{
    mHandles.clear();
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
    const float32 radius = static_cast<float32>(HANDLE_PIXELS / pixelsPerMetre);
    for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        math::Affine3 axisFrame = meshToWorld;
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
            axisFrame = makeTransform(axisPlacement);
        }
        math::Vector3 directionWorld = mTool == Tool::Move ? mParentToWorld.Columns[axis] : axisFrame.Columns[axis];
        math::Vector3 unitWorld;
        if (math::TryNormalize(directionWorld, unitWorld) != math::MathStatus::Success)
        {
            continue;
        }
        QPointF endPixel;
        if (project(meshToWorld.Translation + unitWorld * radius, endPixel) == false)
        {
            continue;
        }
        const QPointF directionPixel = endPixel - mPivotPixel;
        const float64 length = math::Sqrt(QPointF::dotProduct(directionPixel, directionPixel));
        if (mTool != Tool::Rotate)
        {
            if (length >= MINIMUM_AXIS_PIXELS)
            {
                mHandles.append({QLineF(mPivotPixel, mPivotPixel + directionPixel * (HANDLE_PIXELS / length)), axis});
            }
        }
        else
        {
            const math::Vector3 firstWorld = axisFrame.Columns[(axis + 1) % AXIS_COUNT];
            math::Vector3 ringX;
            math::Vector3 ringY;
            if (math::TryNormalize(firstWorld, ringX) != math::MathStatus::Success ||
                math::TryNormalize(math::Cross(unitWorld, ringX), ringY) != math::MathStatus::Success)
            {
                continue;
            }
            for (int32 segment = 0; segment < RING_SEGMENTS; ++segment)
            {
                const float32 angle1 = static_cast<float32>(segment) * 2 * math::kPiF / RING_SEGMENTS;
                const float32 angle2 = static_cast<float32>(segment + 1) * 2 * math::kPiF / RING_SEGMENTS;
                QPointF firstPixel;
                QPointF secondPixel;
                if (project(meshToWorld.Translation + (ringX * math::Cos(angle1) + ringY * math::Sin(angle1)) * radius,
                            firstPixel) == true &&
                    project(meshToWorld.Translation + (ringX * math::Cos(angle2) + ringY * math::Sin(angle2)) * radius,
                            secondPixel) == true)
                {
                    mHandles.append({QLineF(firstPixel, secondPixel), axis});
                }
            }
        }
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
    if (axis >= 0)
    {
        mDragAxis = axis;
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
        value = math::Clamp(original * factor, MINIMUM_SCALE, MAXIMUM_SCALE);
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
    if (mDragAxis >= 0)
    {
        mDragAxis = -1;
        mCandidate = {};
        Q_EMIT Changed();
    }
}
void ViewportObjectInteraction::Paint(QPainter& inoutPainter, const QRectF& outerFramePixel)
{
    const QRectF framePixel = resolveFrame(outerFramePixel);
    if (describe(framePixel, mWorldToClip) == false || mSelected.isEmpty() == true)
    {
        return;
    }
    mFramePixel = framePixel;
    inoutPainter.save();
    inoutPainter.setClipRect(framePixel);
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(QPen(QColor::fromRgb(SELECTION_COLOR), WIRE_WIDTH));
    const RenderScene& scene = mPreviewOrNull->Scene();
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id != mSelected || object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            continue;
        }
        const math::Affine3 meshToWorld = mDragAxis >= 0 ? makeTransform(mCandidate) : object.Transform;
        const BorrowedView<const math::Vector3> positions = GetPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetIndices(scene, object.Mesh);
        const usize triangleCount = indices.GetSize() / TRIANGLE_VERTICES;
        const usize stride = (triangleCount / MAX_WIRE_TRIANGLES + 1) * TRIANGLE_VERTICES;
        for (usize index = 0; index + TRIANGLE_VERTICES <= indices.GetSize(); index += stride)
        {
            for (usize edge = 0; edge < TRIANGLE_VERTICES; ++edge)
            {
                const uint32 first = indices[index + edge];
                const uint32 second = indices[index + (edge + 1) % TRIANGLE_VERTICES];
                QPointF firstPixel;
                QPointF secondPixel;
                if (first < positions.GetSize() && second < positions.GetSize() &&
                    project(math::TransformPoint(meshToWorld, positions[first]), firstPixel) == true &&
                    project(math::TransformPoint(meshToWorld, positions[second]), secondPixel) == true)
                {
                    inoutPainter.drawLine(firstPixel, secondPixel);
                }
            }
        }
    }
    buildHandles();
    for (const HandleLine& handle : mHandles)
    {
        inoutPainter.setPen(QPen(QColor::fromRgb(AXIS_COLORS[static_cast<usize>(handle.Axis)]),
                                 handle.Axis == mDragAxis ? ACTIVE_HANDLE_WIDTH : HANDLE_WIDTH));
        inoutPainter.drawLine(handle.Line);
        if (mTool != Tool::Rotate)
        {
            inoutPainter.drawEllipse(handle.Line.p2(), HANDLE_TIP_PIXELS, HANDLE_TIP_PIXELS);
            inoutPainter.drawText(handle.Line.p2() + QPointF(LABEL_OFFSET, 0), QString(QChar('X' + handle.Axis)));
        }
    }
    inoutPainter.restore();
}
} // namespace ludus::editor
