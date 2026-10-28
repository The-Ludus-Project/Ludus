#pragma once

// Command palette and searchable shortcut list (docs/development/editor-workspace.md).
//
// Both read the commands from the menu bar, which is where every catalogue command (editor_commands.h) and
// every hand-built menu action already lives, so a command is added once and appears in the menus, on its
// toolbar, in the palette and in the F1 list with the same label, shortcut and availability. Nothing is built
// or kept while the palette is closed: entries are collected when it opens.

#include <ludus/foundation/base/types.h>

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

class QAction;
class QListWidget;
class QLineEdit;
class QMenuBar;
class QTreeWidget;

namespace ludus::editor
{
/// One command as the palette and the shortcut list show it.
struct PaletteEntry final
{
    /// Menu label without mnemonic.
    QString Label;
    /// Menu path above the command, such as "Build" or "View › Toolbars".
    QString Group;
    /// Shortcut in the platform's text, or empty.
    QString Shortcut;
    /// False when the command's availability rules forbid it right now.
    bool IsEnabled = false;
    /// The command itself; runs when the entry is chosen.
    QPointer<QAction> Action;
};

/// Every command under menuBar's menus, in menu order, skipping separators, submenu headers, commands without a
/// label and the palette's own two commands.
[[nodiscard]] QList<PaletteEntry> CollectPaletteEntries(const QMenuBar& menuBar);

/// How well query matches candidate: -1 when its letters do not appear in order, otherwise higher is better.
/// Case and spaces are ignored. Consecutive letters, letters at the start of a word and a match at the start
/// score higher; an empty query scores 0.
[[nodiscard]] int ScoreFuzzyMatch(const QString& query, const QString& candidate);

/// Indices into a list of entries.
using PaletteIndexList = QList<int>;

/// Indices into entries that match query, best first (equal scores keep menu order). With isEnabledOnly,
/// commands that are not available now are left out; otherwise they follow the available ones.
[[nodiscard]] PaletteIndexList
RankPaletteEntries(const QList<PaletteEntry>& entries, const QString& query, bool isEnabledOnly);

/// Most commands the palette lists at once.
inline constexpr int PALETTE_MAX_RESULTS = 50;

/// A search box over the commands. Enter runs the chosen command after the palette closes.
class CommandPalette final : public QDialog
{
    Q_OBJECT
public:
    /// isEnabledOnly lists only the commands that can run now ("run in context").
    CommandPalette(QList<PaletteEntry> entries, bool isEnabledOnly, QWidget* parentOrNull);

    /// Entry indices currently listed, best first.
    [[nodiscard]] PaletteIndexList GetShownIndices() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void runCurrent();

    QList<PaletteEntry> mEntries;
    bool mIsEnabledOnly = false;
    QLineEdit* mSearchOrNull = nullptr;
    QListWidget* mListOrNull = nullptr;
};

/// The F1 list: commands that have a shortcut, grouped by menu, filtered by the search box.
class ShortcutList final : public QDialog
{
    Q_OBJECT
public:
    ShortcutList(const QList<PaletteEntry>& entries, QWidget* parentOrNull);

private:
    void refresh();

    QList<PaletteEntry> mEntries;
    QLineEdit* mSearchOrNull = nullptr;
    QTreeWidget* mTreeOrNull = nullptr;
};
} // namespace ludus::editor
