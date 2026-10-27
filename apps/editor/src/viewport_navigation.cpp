#include "internal/preview_viewport.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/ray_path_inspection.h"
#endif

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/platform/hosted_window.h>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHideEvent>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
// Thanks to Epic Games, "Viewport Controls", Unreal Engine documentation:
// https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-controls-in-unreal-engine
// Adopted RMB fly/look, Alt orbit and F framing; editor input remains outside GameplayCamera.
constexpr float32 NANOSECONDS_PER_SECOND = 1000000000.0F;
constexpr int32 FORWARD_KEY = 0;
constexpr int32 BACKWARD_KEY = 1;
constexpr int32 LEFT_KEY = 2;
constexpr int32 RIGHT_KEY = 3;
constexpr int32 DOWN_KEY = 4;
constexpr int32 UP_KEY = 5;
constexpr int32 BOOST_KEY = 6;
int32 GetMovementKey(int32 key) noexcept
{
    switch (key)
    {
        case Qt::Key_W:
            return FORWARD_KEY;
        case Qt::Key_S:
            return BACKWARD_KEY;
        case Qt::Key_A:
            return LEFT_KEY;
        case Qt::Key_D:
            return RIGHT_KEY;
        case Qt::Key_Q:
            return DOWN_KEY;
        case Qt::Key_E:
            return UP_KEY;
        case Qt::Key_Shift:
            return BOOST_KEY;
        default:
            return -1;
    }
}
} // namespace
void PreviewViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    resizeSurface();
}
void PreviewViewport::mousePressEvent(QMouseEvent* event)
{
    mLastPositionGlobal = event->globalPosition();
    setFocus(Qt::MouseFocusReason);
    if (mPlayEndpointOrNull != nullptr)
    {
        // A click gives the game the keyboard; there is no pointer input to forward yet.
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && pickRayPathPixel(event->globalPosition()) == true)
    {
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton)
    {
        mIsFlying = true;
        mHeldKeys[BOOST_KEY] = event->modifiers().testFlag(Qt::ShiftModifier);
        mNavigationClock.start();
        mNavigationTimer.start();
        const QString platformName = QGuiApplication::platformName();
        if (isVisible() == true && platformName != QStringLiteral("offscreen") &&
            platformName != QStringLiteral("minimal") && platformName != QStringLiteral("wasm"))
        {
            mCaptureStartGlobal = event->globalPosition().toPoint();
            mCanWarpPointer =
                platformName.startsWith(QStringLiteral("wayland")) == false && platformName != QStringLiteral("cocoa");
            grabMouse(Qt::BlankCursor);
            mHasMouseCapture = true;
#if defined(Q_OS_MACOS)
            static_cast<void>(mRelativeMouse.TryBegin());
#endif
            recenterPointer();
        }
    }
    event->accept();
}
void PreviewViewport::mouseReleaseEvent(QMouseEvent* event)
{
    if (mPlayEndpointOrNull != nullptr)
    {
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton)
    {
        stopNavigation();
    }
    event->accept();
}
void PreviewViewport::mouseMoveEvent(QMouseEvent* event)
{
    if (mPlayEndpointOrNull != nullptr)
    {
        event->accept();
        return;
    }
#if defined(Q_OS_MACOS)
    if (mRelativeMouse.IsActive() == true)
    {
        // Native relative motion already navigated; Qt's absolute position is constant.
        event->accept();
        return;
    }
#endif
    const QPointF delta = event->globalPosition() - mLastPositionGlobal;
    mLastPositionGlobal = event->globalPosition();
    const float32 deltaX = static_cast<float32>(delta.x());
    const float32 deltaY = static_cast<float32>(delta.y());
    if (navigateMouse(deltaX, deltaY, event->buttons(), event->modifiers()) == false)
    {
        event->ignore();
        return;
    }
    event->accept();
}
bool PreviewViewport::navigateMouse(float32 deltaX,
                                    float32 deltaY,
                                    Qt::MouseButtons buttons,
                                    Qt::KeyboardModifiers modifiers)
{
    const bool isPrimaryDrag = buttons.testFlag(Qt::LeftButton);
    const bool isOrbitModifier = modifiers.testFlag(Qt::AltModifier);
    const bool isPanModifier = modifiers.testFlag(Qt::ShiftModifier);
    if (buttons.testFlag(Qt::MiddleButton) == true ||
        (isPrimaryDrag == true && isOrbitModifier == true && isPanModifier == true))
    {
        Preview_->Pan(deltaX, deltaY, getDrawableHeight());
    }
    else if (isPrimaryDrag == true && isOrbitModifier == true)
    {
        Preview_->Orbit(deltaX, deltaY);
    }
    else if (mIsFlying == true && buttons.testFlag(Qt::RightButton) == true)
    {
        Preview_->Look(deltaX, deltaY);
        recenterPointer();
    }
    else
    {
        return false;
    }
    return true;
}
void PreviewViewport::wheelEvent(QWheelEvent* event)
{
    if (mPlayEndpointOrNull != nullptr)
    {
        event->accept();
        return;
    }
    // Qt supplies logical pixel deltas for high-resolution trackpads; retain fractional notches otherwise.
    const float32 steps = event->pixelDelta().isNull() == false ? static_cast<float32>(event->pixelDelta().y()) / 80
                                                                : static_cast<float32>(event->angleDelta().y()) / 120;
    if (steps != 0)
    {
        if (mIsFlying == true)
        {
            mSpeedMultiplier =
                math::Clamp(mSpeedMultiplier * math::Clamp(1 + steps * 0.2F, 0.1F, 10.0F), 0.001F, 1000.0F);
        }
        else
        {
            Preview_->Zoom(steps);
        }
    }
    event->accept();
}
void PreviewViewport::keyPressEvent(QKeyEvent* event)
{
    if (handlePlayKey(*event, true) == true)
    {
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        stopNavigation();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        if (mRayPaths.isNull() == false && mRayPaths->GetStage() == RayPathInspectionStage::PickingPixel)
        {
            mRayPaths->Leave();
        }
#endif
        event->accept();
        return;
    }
    const int32 movementKey = GetMovementKey(event->key());
    if (mIsFlying == true && movementKey >= 0)
    {
        mHeldKeys[static_cast<usize>(movementKey)] = true;
        event->accept();
        return;
    }
    if (mIsFlying == false && event->modifiers() == Qt::NoModifier)
    {
        if (event->key() == Qt::Key_F)
        {
            static_cast<void>(Preview_->FrameSelection());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Home)
        {
            Preview_->FrameAll();
            event->accept();
            return;
        }
    }
    // Thanks to the Blender Foundation: numpad view keys follow the Blender Manual, "Viewpoint"
    // (https://docs.blender.org/manual/en/latest/editors/3dview/navigate/viewpoint.html).
    // Ctrl selects the opposite side; numpad 5 switches projection.
    const Qt::KeyboardModifiers viewModifiers = event->modifiers() & ~(Qt::KeypadModifier | Qt::ControlModifier);
    if (mIsFlying == false && event->modifiers().testFlag(Qt::KeypadModifier) == true &&
        viewModifiers == Qt::NoModifier)
    {
        const bool isOpposite = event->modifiers().testFlag(Qt::ControlModifier);
        switch (event->key())
        {
            case Qt::Key_7:
                Preview_->SetAxisView(isOpposite == true ? EditorAxisView::Bottom : EditorAxisView::Top);
                event->accept();
                return;
            case Qt::Key_1:
                Preview_->SetAxisView(isOpposite == true ? EditorAxisView::Back : EditorAxisView::Front);
                event->accept();
                return;
            case Qt::Key_3:
                Preview_->SetAxisView(isOpposite == true ? EditorAxisView::Left : EditorAxisView::Right);
                event->accept();
                return;
            case Qt::Key_5:
                Preview_->SetOrthographic(Preview_->GetVisibleCameraSample().Lens.Projection !=
                                          gameplay::camera::CameraProjection::OrthographicFinite);
                event->accept();
                return;
            default:
                break;
        }
    }
    QWidget::keyPressEvent(event);
}
void PreviewViewport::keyReleaseEvent(QKeyEvent* event)
{
    if (handlePlayKey(*event, false) == true)
    {
        return;
    }
    const int32 movementKey = GetMovementKey(event->key());
    if (movementKey >= 0 && event->isAutoRepeat() == false)
    {
        mHeldKeys[static_cast<usize>(movementKey)] = false;
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}
void PreviewViewport::stopNavigation()
{
#if defined(Q_OS_MACOS)
    mRelativeMouse.Stop();
#endif
    mIsFlying = false;
    mNavigationTimer.stop();
    for (bool& isHeld : mHeldKeys)
    {
        isHeld = false;
    }
    if (mHasMouseCapture == true)
    {
        mHasMouseCapture = false;
        releaseMouse();
        if (mCanWarpPointer == true)
        {
            QCursor::setPos(mCaptureStartGlobal);
        }
    }
}
void PreviewViewport::advanceNavigation()
{
    if (mIsFlying == false)
    {
        return;
    }
    const float32 deltaSeconds = static_cast<float32>(mNavigationClock.nsecsElapsed()) / NANOSECONDS_PER_SECOND;
    mNavigationClock.restart();
    const math::Vector3 movementControls{
        (mHeldKeys[RIGHT_KEY] == true ? 1.0F : 0.0F) - (mHeldKeys[LEFT_KEY] == true ? 1.0F : 0.0F),
        (mHeldKeys[UP_KEY] == true ? 1.0F : 0.0F) - (mHeldKeys[DOWN_KEY] == true ? 1.0F : 0.0F),
        (mHeldKeys[BACKWARD_KEY] == true ? 1.0F : 0.0F) - (mHeldKeys[FORWARD_KEY] == true ? 1.0F : 0.0F)};
    if (math::LengthSquared(movementControls) == 0)
    {
        return;
    }
    const float32 speed = math::Clamp(Preview_->GetFocusDistance() * 0.5F, 0.001F, 10000.0F) * mSpeedMultiplier *
                          (mHeldKeys[BOOST_KEY] == true ? 4.0F : 1.0F);
    Preview_->Fly(movementControls, deltaSeconds, speed);
}
void PreviewViewport::recenterPointer()
{
    if (mHasMouseCapture == true && mCanWarpPointer == true)
    {
        const QPoint centerGlobal = mapToGlobal(rect().center());
        mLastPositionGlobal = centerGlobal;
        QCursor::setPos(centerGlobal);
    }
}
void PreviewViewport::focusOutEvent(QFocusEvent* event)
{
    // Window deactivation, modal dialogs and Shift+F1 all arrive here: the game loses focus.
    sendPlayFocus(false);
    stopNavigation();
    QWidget::focusOutEvent(event);
}
void PreviewViewport::hideEvent(QHideEvent* event)
{
    if (mPlayEndpointOrNull != nullptr)
    {
        // A hidden tab or dock is an occluded window: the game drops to its hidden pacing.
        static_cast<void>(mPlayEndpointOrNull->SendVisibility(platform::WindowVisibility::Occluded));
    }
    stopNavigation();
    QWidget::hideEvent(event);
}
} // namespace ludus::editor
