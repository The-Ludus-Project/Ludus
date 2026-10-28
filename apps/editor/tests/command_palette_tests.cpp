// Command palette and the searchable shortcut list: fuzzy ranking, availability, running a command, and the
// two shortcuts that open the palette.

#include "internal/command_palette.h"
#include "internal/controller.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace ludus::editor;

namespace
{
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
    // With no project open, many commands are not available.
    CHECK(availableCount < allCount);
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
