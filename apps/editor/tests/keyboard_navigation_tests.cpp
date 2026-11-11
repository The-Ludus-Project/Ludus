// Keyboard navigation: Tab walks the main window's panels and comes back round, never sticking in one control, and a
// code editor (where Tab indents) has a documented way out, Edit > Tab Moves Focus (WCAG 2.1.1 and 2.1.2).

#include "internal/code_editor.h"
#include "internal/controller.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSet>
#include <QTabBar>
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
/// Time for an opened project's documents and tool windows to settle.
constexpr int SETTLE_MILLISECONDS = 200;

bool IsInside(const QWidget* widgetOrNull, const QWidget& container)
{
    return widgetOrNull != nullptr && (widgetOrNull == &container || container.isAncestorOf(widgetOrNull) == true);
}

/// True for the kinds of control the editor style draws a focus ring on (editor_style.cpp).
bool HasFocusIndicator(const QWidget& widget)
{
    return qobject_cast<const QAbstractButton*>(&widget) != nullptr ||
           qobject_cast<const QLineEdit*>(&widget) != nullptr || qobject_cast<const QComboBox*>(&widget) != nullptr ||
           qobject_cast<const QAbstractSpinBox*>(&widget) != nullptr ||
           qobject_cast<const QAbstractItemView*>(&widget) != nullptr ||
           qobject_cast<const QPlainTextEdit*>(&widget) != nullptr ||
           qobject_cast<const QTextEdit*>(&widget) != nullptr || qobject_cast<const QTabBar*>(&widget) != nullptr ||
           qobject_cast<const QAbstractSlider*>(&widget) != nullptr ||
           // The viewport lights the strip above its native surface instead of a ring.
           widget.findChild<QWidget*>(QStringLiteral("viewportFocusStrip"), Qt::FindDirectChildrenOnly) != nullptr;
}

bool ActivateWindow(QWidget& inoutWindow)
{
    inoutWindow.show();
    inoutWindow.activateWindow();
    return QTest::qWaitForWindowActive(&inoutWindow);
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

namespace
{
/// Walk window with Tab until focus comes back round, checking each stop, then check the loop covers the visible
/// tool windows and that Shift+Tab walks it backwards.
void CheckTabLoop(MainWindow& inoutWindow)
{
    QApplication::processEvents();
    if (inoutWindow.focusWidget() == nullptr)
    {
        // Start where Tab would: the first control in the window's focus chain that takes Tab focus.
        for (QWidget* widget = inoutWindow.nextInFocusChain(); widget != &inoutWindow;
             widget = widget->nextInFocusChain())
        {
            if (widget->isVisible() == true && widget->isEnabled() == true &&
                (widget->focusPolicy() & Qt::TabFocus) != 0)
            {
                widget->setFocus(Qt::TabFocusReason);
                break;
            }
        }
    }
    REQUIRE(inoutWindow.focusWidget() != nullptr);
    // The window may start on a page container (Welcome) that is not itself a Tab stop; the first Tab leaves it for
    // the first control, which starts the loop.
    if ((inoutWindow.focusWidget()->focusPolicy() & Qt::TabFocus) == 0)
    {
        QTest::keyClick(inoutWindow.focusWidget(), Qt::Key_Tab);
    }
    QWidget* first = inoutWindow.focusWidget();
    REQUIRE(first != nullptr);
    INFO("Started on " << first->metaObject()->className() << " '" << first->objectName().toStdString() << "'");

    QList<QWidget*> visited{first};
    bool hasComeRound = false;
    for (int press = 0; press < MAXIMUM_TAB_PRESSES && hasComeRound == false; ++press)
    {
        QWidget* current = inoutWindow.focusWidget();
        QTest::keyClick(current, Qt::Key_Tab);
        QWidget* next = inoutWindow.focusWidget();
        INFO("Tab from " << current->metaObject()->className() << " '" << current->objectName().toStdString() << "'");
        // A control that keeps focus on Tab traps the keyboard (WCAG 2.1.2).
        REQUIRE(next != current);
        INFO("Tab to " << next->metaObject()->className() << " '" << next->objectName().toStdString() << "'");
        // Every stop is something a person can see, and shows that it has focus.
        CHECK(next->isVisible() == true);
        CHECK(next->isEnabled() == true);
        CHECK(HasFocusIndicator(*next) == true);
        if (qEnvironmentVariableIsSet("LUDUS_PRINT_TAB_ORDER") == true)
        {
            QString path;
            for (const QWidget* widget = next; widget != nullptr && widget != &inoutWindow;
                 widget = widget->parentWidget())
            {
                path.prepend(QStringLiteral("/%1(%2)").arg(QString::fromLatin1(widget->metaObject()->className()),
                                                           widget->objectName()));
            }
            qInfo().noquote() << press << path;
        }
        hasComeRound = next == first;
        if (hasComeRound == false)
        {
            visited.append(next);
        }
    }
    CHECK(hasComeRound == true);
    // No control is reached twice in one round: the order is a single loop, not a cycle inside a panel.
    CHECK(QSet<QWidget*>(visited.begin(), visited.end()).size() == visited.size());

    // The visible tool windows with controls are part of the loop.
    for (QDockWidget* dock : inoutWindow.findChildren<QDockWidget*>())
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
    QTest::keyClick(first, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(inoutWindow.focusWidget() == visited.last());
}
} // namespace

TEST_CASE("Tab walks the main window's panels and comes back round without a trap", "[editor][keyboard][shell]")
{
    SetCodeEditorTabMovesFocus(false);
    QTemporaryDir settings;
    QTemporaryDir project;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    REQUIRE(ActivateWindow(window) == true);
    SECTION("Welcome")
    {
        CheckTabLoop(window);
    }
    SECTION("A project's World with Properties")
    {
        const QString descriptor = QDir(project.path()).filePath(QStringLiteral("ludus.project.json"));
        QFile file(descriptor);
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        file.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
        file.close();
        controller.OpenProject(descriptor);
        REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
        QTest::qWait(SETTLE_MILLISECONDS);
        CheckTabLoop(window);

        // The viewport's native surface hides a ring, so the strip above it lights with the focus accent instead.
        QWidget* viewport = window.findChild<QWidget*>(QStringLiteral("worldViewport"));
        REQUIRE(viewport != nullptr);
        QWidget* strip = viewport->findChild<QWidget*>(QStringLiteral("viewportFocusStrip"));
        REQUIRE(strip != nullptr);
        const QColor accent = strip->palette().color(QPalette::Link);
        const auto isLit = [strip, &accent]() {
            if (strip->isVisible() == false || strip->height() <= 0)
            {
                return false;
            }
            const QImage image = strip->grab().toImage();
            return image.pixelColor(image.width() / 2, image.height() / 2).rgb() == accent.rgb();
        };
        viewport->setFocus(Qt::TabFocusReason);
        REQUIRE(viewport->hasFocus() == true);
        CHECK(isLit() == true);
        window.findChild<QWidget*>(QStringLiteral("worldEntities"))->setFocus(Qt::TabFocusReason);
        CHECK(isLit() == false);
        // The strip floats over the controls' edge: lighting it never resizes the render surface.
        CHECK(viewport->layout()->indexOf(strip) < 0);
    }
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
