#pragma once

#include <QPoint>
#include <QPointer>
#include <QTest>
#include <QWidget>

/// @file
/// Mouse input helpers that keep QTest's process-wide button state balanced between test cases.
namespace ludus::editor::tests
{
/// Double-click button on widget at position (widget coordinates; the null point means its centre),
/// ending with the release a window system sends after the double-click event.
///
/// QTest::mouseDClick on a QWidget sends only the double-click event and marks button as held in
/// QTest's process-wide button state (QTestPrivate::qtestMouseButtons) without ever clearing it. Every
/// later QTest mouse event, in this and following test cases, then reports that button in
/// QMouseEvent::buttons(), so handlers that ignore presses while another button is held (for example
/// the Back/Forward routing in MouseHistoryButtons) stop responding. When the double-click destroys
/// widget, as a confirming dialog list may, the release has no receiver and only the held state is
/// cleared.
inline void DoubleClick(QWidget* widget,
                        Qt::MouseButton button,
                        Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                        QPoint position = QPoint())
{
    const QPointer<QWidget> target(widget);
    QTest::mouseDClick(widget, button, modifiers, position);
    if (target.isNull() == false)
    {
        QTest::mouseRelease(target.data(), button, modifiers, position);
    }
    else
    {
        QTestPrivate::qtestMouseButtons.setFlag(button, false);
    }
}
} // namespace ludus::editor::tests
