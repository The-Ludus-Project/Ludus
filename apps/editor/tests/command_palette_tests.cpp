// Command palette and the searchable shortcut list: fuzzy ranking, availability, running a command, and the
// two shortcuts that open the palette.

#include "internal/code_editor.h"
#include "internal/command_palette.h"
#include "internal/controller.h"
#include "internal/editor_commands.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>

using namespace ludus::editor;

namespace
{
constexpr int FADE_SETTLE_MILLISECONDS = 150;
PaletteEntry MakeEntry(const QString& label, const QString& group, bool isEnabled)
{
    PaletteEntry entry;
    entry.Label = label;
    entry.Group = group;
    entry.IsEnabled = isEnabled;
    return entry;
}

QStringList Labels(const QList<PaletteEntry>& entries, const QList<int>& indices)
{
    QStringList labels;
    for (const int index : indices)
    {
        labels.append(entries.at(index).Label);
    }
    return labels;
}
} // namespace

TEST_CASE("Fuzzy matching needs the letters in order and prefers tight matches", "[editor][palette]")
{
    CHECK(ScoreFuzzyMatch(QString(), QStringLiteral("Anything")) == 0);
    CHECK(ScoreFuzzyMatch(QStringLiteral("xyz"), QStringLiteral("Build")) < 0);
    CHECK(ScoreFuzzyMatch(QStringLiteral("dlb"), QStringLiteral("Build")) < 0);
    CHECK(ScoreFuzzyMatch(QStringLiteral("BUILD"), QStringLiteral("build")) >= 0);
    CHECK(ScoreFuzzyMatch(QStringLiteral("b g"), QStringLiteral("Build Graph")) >= 0);
    const int tight = ScoreFuzzyMatch(QStringLiteral("bg"), QStringLiteral("Build Graph"));
    const int loose = ScoreFuzzyMatch(QStringLiteral("bg"), QStringLiteral("Reload Debug Build Settings Ghost"));
    CHECK(tight > loose);
    CHECK(ScoreFuzzyMatch(QStringLiteral("opt"), QStringLiteral("Options...")) >
          ScoreFuzzyMatch(QStringLiteral("opt"), QStringLiteral("Reload Operations Tree")));
}

TEST_CASE("Ranking lists available commands first and can leave the others out", "[editor][palette]")
{
    const QList<PaletteEntry> entries{MakeEntry(QStringLiteral("Build"), QStringLiteral("Build"), false),
                                      MakeEntry(QStringLiteral("Build Graph"), QStringLiteral("Build"), true),
                                      MakeEntry(QStringLiteral("Open Project..."), QStringLiteral("File"), true),
                                      MakeEntry(QStringLiteral("Timing"), QStringLiteral("View"), true)};
    CHECK(Labels(entries, RankPaletteEntries(entries, QString(), false)) ==
          QStringList{QStringLiteral("Build Graph"),
                      QStringLiteral("Open Project..."),
                      QStringLiteral("Timing"),
                      QStringLiteral("Build")});
    CHECK(Labels(entries, RankPaletteEntries(entries, QStringLiteral("build"), false)) ==
          QStringList{QStringLiteral("Build Graph"), QStringLiteral("Build")});
    CHECK(Labels(entries, RankPaletteEntries(entries, QStringLiteral("build"), true)) ==
          QStringList{QStringLiteral("Build Graph")});
    // The menu path finds a command by where it lives.
    CHECK(Labels(entries, RankPaletteEntries(entries, QStringLiteral("viewtim"), false)) ==
          QStringList{QStringLiteral("Timing")});
    CHECK(RankPaletteEntries(entries, QStringLiteral("zzzz"), false).isEmpty() == true);
}

TEST_CASE("The palette runs the chosen command and skips unavailable ones", "[editor][palette]")
{
    QWidget host;
    QMenuBar bar(&host);
    QMenu* menu = bar.addMenu(QStringLiteral("&Tools"));
    int ranAlpha = 0;
    int ranBeta = 0;
    menu->addAction(QStringLiteral("&Alpha Thing"), &host, [&ranAlpha]() { ++ranAlpha; });
    QAction* beta = menu->addAction(QStringLiteral("Beta Thing"), &host, [&ranBeta]() { ++ranBeta; });
    beta->setEnabled(false);
    menu->addSeparator();
    QMenu* sub = menu->addMenu(QStringLiteral("S&ub"));
    sub->addAction(QStringLiteral("Gamma"));
    const QList<PaletteEntry> entries = CollectPaletteEntries(bar);
    REQUIRE(entries.size() == 3);
    CHECK(entries.at(0).Label == QStringLiteral("Alpha Thing"));
    CHECK(entries.at(0).Group == QStringLiteral("Tools"));
    CHECK(entries.at(1).IsEnabled == false);
    CHECK(entries.at(2).Group.contains(QStringLiteral("Sub")) == true);

    CommandPalette* palette = new CommandPalette(entries, false, &host);
    palette->show();
    QLineEdit* search = palette->findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    QListWidget* list = palette->findChild<QListWidget*>(QStringLiteral("commandPaletteList"));
    REQUIRE(search != nullptr);
    REQUIRE(list != nullptr);
    CHECK(palette->focusWidget() == search);
    CHECK(palette->GetShownIndices().size() == 3);
    QTest::keyClicks(search, QStringLiteral("thing"));
    CHECK(palette->GetShownIndices() == QList<int>{0, 1});
    // Enter on the available command closes the palette and runs it.
    QTest::keyClick(search, Qt::Key_Return);
    QTest::qWait(50);
    CHECK(ranAlpha == 1);
    CHECK(ranBeta == 0);

    CommandPalette* contextPalette = new CommandPalette(entries, true, &host);
    contextPalette->show();
    CHECK(contextPalette->GetShownIndices() == QList<int>{0, 2});
    contextPalette->close();
}

TEST_CASE("The palette fades in over 100 ms and appears at once under reduced motion", "[editor][palette][motion]")
{
    QWidget host;
    const QList<PaletteEntry> entries{MakeEntry(QStringLiteral("Alpha"), QStringLiteral("Tools"), true)};
    qputenv("LUDUS_REDUCE_MOTION", QByteArrayLiteral("0"));
    CommandPalette* fading = new CommandPalette(entries, false, &host);
    fading->show();
    CHECK(fading->IsFading() == true);
    CHECK(fading->windowOpacity() < 1.0);
    QTest::qWait(PALETTE_FADE_MILLISECONDS + FADE_SETTLE_MILLISECONDS);
    CHECK(fading->IsFading() == false);
    CHECK(fading->windowOpacity() == 1.0);
    fading->close();

    qputenv("LUDUS_REDUCE_MOTION", QByteArrayLiteral("1"));
    CommandPalette* instant = new CommandPalette(entries, false, &host);
    instant->show();
    CHECK(instant->IsFading() == false);
    CHECK(instant->windowOpacity() == 1.0);
    instant->close();
    qunsetenv("LUDUS_REDUCE_MOTION");
}

TEST_CASE("Ctrl+K and F3 open the palette; F3 lists only what can run", "[editor][palette][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    CHECK(window.findChild<CommandPalette*>() == nullptr);
    QAction* paletteAction = window.findChild<QAction*>(QStringLiteral("tools.commandPalette"));
    QAction* contextAction = window.findChild<QAction*>(QStringLiteral("tools.runInContext"));
    REQUIRE(paletteAction != nullptr);
    REQUIRE(contextAction != nullptr);
    CHECK(paletteAction->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_K));
    CHECK(contextAction->shortcut() == QKeySequence(Qt::Key_F3));

    paletteAction->trigger();
    CommandPalette* all = window.findChild<CommandPalette*>(QStringLiteral("commandPalette"));
    REQUIRE(all != nullptr);
    // The palette is its own window but draws with the window's token palette, not the platform's.
    for (const QPalette::ColorRole role :
         {QPalette::Window, QPalette::Base, QPalette::Text, QPalette::Highlight, QPalette::PlaceholderText})
    {
        CHECK(all->palette().color(role) == window.palette().color(role));
    }
    const int allCount = static_cast<int>(all->GetShownIndices().size());
    CHECK(allCount > 0);
    all->close();
    QApplication::processEvents();
    CHECK(window.findChild<CommandPalette*>() == nullptr);

    contextAction->trigger();
    CommandPalette* available = window.findChild<CommandPalette*>(QStringLiteral("commandPalette"));
    REQUIRE(available != nullptr);
    const int availableCount = static_cast<int>(available->GetShownIndices().size());
    CHECK(availableCount > 0);
    // With no project open, many commands are not available. Both lists stop at PALETTE_MAX_RESULTS, so compare
    // the ranked entries rather than the rows shown.
    const QList<PaletteEntry> entries = CollectWindowPaletteEntries(window);
    CHECK(RankPaletteEntries(entries, QString(), true).size() < RankPaletteEntries(entries, QString(), false).size());
    CHECK(availableCount <= allCount);
    available->close();
}

TEST_CASE("The F1 shortcut list is grouped by menu and searchable", "[editor][palette][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    QAction* shortcuts = window.findChild<QAction*>(QStringLiteral("keyboardShortcutsAction"));
    REQUIRE(shortcuts != nullptr);
    shortcuts->trigger();
    ShortcutList* list = window.findChild<ShortcutList*>(QStringLiteral("shortcutList"));
    REQUIRE(list != nullptr);
    QTreeWidget* tree = list->findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
    QLineEdit* search = list->findChild<QLineEdit*>(QStringLiteral("shortcutsSearch"));
    REQUIRE(tree != nullptr);
    REQUIRE(search != nullptr);
    const int groups = tree->topLevelItemCount();
    CHECK(groups > 1);
    const auto findLeaf = [tree](const QString& label) {
        for (int group = 0; group < tree->topLevelItemCount(); ++group)
        {
            for (int row = 0; row < tree->topLevelItem(group)->childCount(); ++row)
            {
                if (tree->topLevelItem(group)->child(row)->text(0) == label)
                {
                    return tree->topLevelItem(group)->child(row);
                }
            }
        }
        return static_cast<QTreeWidgetItem*>(nullptr);
    };
    REQUIRE(findLeaf(QStringLiteral("Command Palette...")) == nullptr); // the palette's own commands are not listed
    QTreeWidgetItem* open = findLeaf(QStringLiteral("Open Project..."));
    REQUIRE(open != nullptr);
    CHECK(open->text(1).isEmpty() == false);
    QTest::keyClicks(search, QStringLiteral("zzzzzz"));
    CHECK(tree->topLevelItemCount() == 0);
    search->clear();
    QTest::keyClicks(search, QStringLiteral("open"));
    CHECK(findLeaf(QStringLiteral("Open Project...")) != nullptr);
    CHECK(tree->topLevelItemCount() < groups);
    list->close();
}

TEST_CASE("Fuzzy match positions mark the matched letters in order", "[editor][palette]")
{
    CHECK(FindFuzzyMatchPositions(QStringLiteral("bg"), QStringLiteral("Build Graph")) == QList<int>{0, 6});
    CHECK(FindFuzzyMatchPositions(QStringLiteral("B G"), QStringLiteral("build graph")) == QList<int>{0, 6});
    CHECK(FindFuzzyMatchPositions(QString(), QStringLiteral("Build Graph")).isEmpty() == true);
    CHECK(FindFuzzyMatchPositions(QStringLiteral("gb"), QStringLiteral("Build Graph")).isEmpty() == true);
    // A long label still matches: a match never scores below zero, which means no match.
    const QString longLabel = QStringLiteral("a") + QString(400, QLatin1Char('z'));
    CHECK(ScoreFuzzyMatch(QStringLiteral("a"), longLabel) >= 0);
    CHECK(FindFuzzyMatchPositions(QStringLiteral("a"), longLabel) == QList<int>{0});
}

TEST_CASE("Commands only on a toolbar or the window are listed once, after the menus", "[editor][palette]")
{
    QMainWindow window;
    QMenu* menu = window.menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction* inMenu = menu->addAction(QStringLiteral("&Alpha"));
    QAction* palette = menu->addAction(QStringLiteral("&Command Palette..."));
    palette->setObjectName(QStringLiteral("tools.commandPalette"));
    QToolBar* toolbar = window.addToolBar(QStringLiteral("Game"));
    toolbar->addAction(inMenu);
    toolbar->addSeparator();
    toolbar->addAction(QStringLiteral("Toolbar Only"));
    QAction* windowOnly = new QAction(QStringLiteral("Window &Only"), &window);
    windowOnly->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
    window.addAction(windowOnly);
    window.addAction(inMenu);
    const QList<PaletteEntry> entries = CollectWindowPaletteEntries(window);
    REQUIRE(entries.size() == 3);
    CHECK(entries.at(0).Label == QStringLiteral("Alpha"));
    CHECK(entries.at(0).Group == QStringLiteral("Tools"));
    CHECK(entries.at(1).Label == QStringLiteral("Toolbar Only"));
    CHECK(entries.at(1).Group == QStringLiteral("Game"));
    CHECK(entries.at(2).Label == QStringLiteral("Window Only"));
    CHECK(entries.at(2).Group == QStringLiteral("Window"));
    CHECK(entries.at(2).Shortcut == windowOnly->shortcut().toString(QKeySequence::NativeText));
}

TEST_CASE("Page keys move a page and stop at the ends; the search box names the chosen command",
          "[editor][palette][keyboard]")
{
    constexpr int ENTRY_COUNT = 30;
    constexpr int PAGE_ROWS = 10;
    QWidget host;
    QList<PaletteEntry> entries;
    for (int index = 0; index < ENTRY_COUNT; ++index)
    {
        entries.append(MakeEntry(QStringLiteral("Command %1").arg(index), QStringLiteral("Tools"), true));
    }
    entries[0].Shortcut = QStringLiteral("F4");
    CommandPalette* palette = new CommandPalette(entries, false, &host);
    palette->show();
    QLineEdit* search = palette->findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    QListWidget* list = palette->findChild<QListWidget*>(QStringLiteral("commandPaletteList"));
    REQUIRE(search != nullptr);
    REQUIRE(list != nullptr);
    CHECK(list->currentRow() == 0);
    CHECK(search->accessibleDescription() == QStringLiteral("Command 0, F4"));
    QTest::keyClick(search, Qt::Key_PageDown);
    CHECK(list->currentRow() == PAGE_ROWS);
    QTest::keyClick(search, Qt::Key_PageDown);
    QTest::keyClick(search, Qt::Key_PageDown);
    CHECK(list->currentRow() == ENTRY_COUNT - 1);
    QTest::keyClick(search, Qt::Key_PageUp);
    CHECK(list->currentRow() == ENTRY_COUNT - 1 - PAGE_ROWS);
    CHECK(search->accessibleDescription() == QStringLiteral("Command 19"));
    QTest::keyClick(search, Qt::Key_PageUp);
    QTest::keyClick(search, Qt::Key_PageUp);
    CHECK(list->currentRow() == 0);
    // The arrows wrap; the page keys do not.
    QTest::keyClick(search, Qt::Key_Up);
    CHECK(list->currentRow() == ENTRY_COUNT - 1);
    // Focus never leaves the search box, and the hint says how to drive it.
    CHECK(palette->focusWidget() == search);
    CHECK(palette->findChild<QLabel*>(QStringLiteral("commandPaletteHint")) != nullptr);
    palette->close();
}

TEST_CASE("The palette finds Tab Moves Focus by name, shows its shortcut and runs it with Enter",
          "[editor][palette][keyboard][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    QAction* tabMovesFocus = window.findChild<QAction*>(QStringLiteral("editor.tabMovesFocus"));
    REQUIRE(tabMovesFocus != nullptr);
    CHECK(tabMovesFocus->shortcut() == QKeySequence(QString::fromLatin1(TAB_MOVES_FOCUS_SHORTCUT)));
    CHECK(tabMovesFocus->isChecked() == false);
    CHECK(IsCodeEditorTabMovingFocus() == false);

    // Every command the palette lists is a real QAction of the window: it reuses them rather than copying.
    const QList<PaletteEntry> entries = CollectWindowPaletteEntries(window);
    const QList<QAction*> actions = window.findChildren<QAction*>();
    for (const PaletteEntry& entry : entries)
    {
        INFO(entry.Label.toStdString());
        CHECK(actions.contains(entry.Action.data()) == true);
    }

    window.findChild<QAction*>(QStringLiteral("tools.commandPalette"))->trigger();
    CommandPalette* palette = window.findChild<CommandPalette*>(QStringLiteral("commandPalette"));
    REQUIRE(palette != nullptr);
    QLineEdit* search = palette->findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    REQUIRE(search != nullptr);
    QTest::keyClicks(search, QStringLiteral("tab moves"));
    const QList<int> shown = palette->GetShownIndices();
    REQUIRE(shown.isEmpty() == false);
    const QList<PaletteEntry> paletteEntries = CollectWindowPaletteEntries(window);
    CHECK(paletteEntries.at(shown.first()).Label == QStringLiteral("Tab Moves Focus"));
    CHECK(paletteEntries.at(shown.first()).Shortcut.isEmpty() == false);
    QTest::keyClick(search, Qt::Key_Return);
    QTest::qWait(50);
    CHECK(tabMovesFocus->isChecked() == true);
    CHECK(IsCodeEditorTabMovingFocus() == true);
    tabMovesFocus->trigger();
    CHECK(IsCodeEditorTabMovingFocus() == false);
}
