// Keyboard navigation: Tab walks the main window's panels and comes back round, never sticking in one control, and a
// code editor (where Tab indents) has a documented way out, Edit > Tab Moves Focus (WCAG 2.1.1 and 2.1.2).

#include "internal/code_editor.h"
#include "internal/controller.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

using namespace ludus::editor;

namespace
{
/// More Tab presses than the window has focusable controls; a walk that has not come round by then is stuck.
constexpr int MAXIMUM_TAB_PRESSES = 600;

bool IsInside(const QWidget* widgetOrNull, const QWidget& container)
{
    return widgetOrNull != nullptr && (widgetOrNull == &container || container.isAncestorOf(widgetOrNull) == true);
}

bool ActivateWindow(QWidget& window)
{
    window.show();
    QApplication::setActiveWindow(&window);
    window.activateWindow();
    return QTest::qWaitForWindowActive(&window);
}
} // namespace

TEST_CASE("Tab indents in a code editor until Tab Moves Focus lets the keyboard out", "[editor][keyboard]")
{
    SetCodeEditorTabMovesFocus(false);
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    QLineEdit* before = new QLineEdit(&host);
    CodeEditor* editor = new CodeEditor(&host);
    QLineEdit* after = new QLineEdit(&host);
    layout->addWidget(before);
    layout->addWidget(editor);
    layout->addWidget(after);
    REQUIRE(ActivateWindow(host) == true);
    editor->setFocus();
    CHECK(host.focusWidget() == editor);

    // By default Tab indents and focus stays: the code is what Tab is for.
    QTest::keyClick(editor, Qt::Key_Tab);
    CHECK(host.focusWidget() == editor);
    CHECK(editor->toPlainText().isEmpty() == false);

    // Tab Moves Focus turns every open code editor, and every later one, into an ordinary stop in the Tab order.
    SetCodeEditorTabMovesFocus(true);
    CHECK(IsCodeEditorTabMovingFocus() == true);
    const QString text = editor->toPlainText();
    QTest::keyClick(editor, Qt::Key_Tab);
    CHECK(host.focusWidget() == after);
    CHECK(editor->toPlainText() == text);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(host.focusWidget() == before);
    CodeEditor later(&host);
    CHECK(later.tabChangesFocus() == true);

    SetCodeEditorTabMovesFocus(false);
    CHECK(editor->tabChangesFocus() == false);
    CHECK(later.tabChangesFocus() == false);
}

TEST_CASE("Tab walks the main window's panels and comes back round without a trap", "[editor][keyboard][shell]")
{
    SetCodeEditorTabMovesFocus(false);
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    REQUIRE(ActivateWindow(window) == true);
    QApplication::processEvents();
    if (window.focusWidget() == nullptr)
    {
        static_cast<void>(window.focusNextChild());
    }
    QWidget* first = window.focusWidget();
    REQUIRE(first != nullptr);

    QList<QWidget*> visited{first};
    bool hasComeRound = false;
    for (int press = 0; press < MAXIMUM_TAB_PRESSES && hasComeRound == false; ++press)
    {
        QWidget* current = window.focusWidget();
        QTest::keyClick(current, Qt::Key_Tab);
        QWidget* next = window.focusWidget();
        INFO("Tab from " << current->metaObject()->className() << " '" << current->objectName().toStdString() << "'");
        // A control that keeps focus on Tab traps the keyboard (WCAG 2.1.2).
        REQUIRE(next != current);
        // Every stop is something a person can see.
        CHECK(next->isVisible() == true);
        CHECK(next->isEnabled() == true);
        hasComeRound = next == first;
        if (hasComeRound == false)
        {
            visited.append(next);
        }
    }
    CHECK(hasComeRound == true);
    // No control is reached twice in one round: the order is a single loop, not a cycle inside a panel.
    CHECK(QSet<QWidget*>(visited.begin(), visited.end()).size() == visited.size());

    // The visible tool windows are part of the loop.
    for (QDockWidget* dock : window.findChildren<QDockWidget*>())
    {
        if (dock->isVisible() == false || dock->visibleRegion().isEmpty() == true)
        {
            continue;
        }
        bool hasFocusableChild = false;
        for (QWidget* child : dock->findChildren<QWidget*>())
        {
            hasFocusableChild =
                hasFocusableChild || (child->isVisible() == true && child->isEnabled() == true &&
                                      (child->focusPolicy() & Qt::TabFocus) != 0 && child->focusProxy() == nullptr);
        }
        if (hasFocusableChild == false)
        {
            continue;
        }
        INFO("Tool window " << dock->objectName().toStdString());
        const bool isReached = std::any_of(visited.begin(), visited.end(), [dock](const QWidget* widget) {
            return IsInside(widget, *dock);
        });
        CHECK(isReached == true);
    }

    // Shift+Tab walks the same loop backwards.
    window.focusWidget()->clearFocus();
    first->setFocus(Qt::TabFocusReason);
    QTest::keyClick(first, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(window.focusWidget() == visited.last());
}

TEST_CASE("Editable multi-line fields outside code pass Tab on", "[editor][keyboard][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    for (QPlainTextEdit* field : window.findChildren<QPlainTextEdit*>())
    {
        if (qobject_cast<CodeEditor*>(field) != nullptr || field->isReadOnly() == true)
        {
            continue;
        }
        INFO("Plain text field " << field->objectName().toStdString());
        CHECK(field->tabChangesFocus() == true);
    }
    for (QTextEdit* field : window.findChildren<QTextEdit*>())
    {
        if (field->isReadOnly() == true)
        {
            continue;
        }
        INFO("Text field " << field->objectName().toStdString());
        CHECK(field->tabChangesFocus() == true);
    }
}
