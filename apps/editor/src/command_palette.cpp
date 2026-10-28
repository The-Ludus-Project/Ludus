#include "internal/command_palette.h"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ludus::editor
{
namespace
{
constexpr int MATCH_SCORE = 10;
constexpr int CONSECUTIVE_BONUS = 15;
constexpr int WORD_START_BONUS = 12;
constexpr int SUBSTRING_BONUS = 30;
constexpr int PREFIX_BONUS = 20;
constexpr int PALETTE_WIDTH = 560;
constexpr int PALETTE_VISIBLE_ROWS = 10;
constexpr int PALETTE_TOP_MARGIN = 80;
constexpr int PALETTE_PADDING = 8;
constexpr int ROW_PADDING = 6;
constexpr int SHORTCUT_DIALOG_WIDTH = 520;
constexpr int SHORTCUT_DIALOG_HEIGHT = 480;
constexpr char PALETTE_COMMAND_ID[] = "tools.commandPalette";
constexpr char RUN_IN_CONTEXT_COMMAND_ID[] = "tools.runInContext";
const QChar GROUP_SEPARATOR = QChar(0x203A);

QString StripMnemonic(QString text)
{
    // "&&" is a literal ampersand; any other "&" marks a mnemonic.
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QLatin1Char('\x01'), QLatin1Char('&'));
    return text;
}

void CollectFromMenu(const QMenu& menu, const QString& group, QList<PaletteEntry>& outEntries)
{
    for (QAction* action : menu.actions())
    {
        if (action->isSeparator() == true)
        {
            continue;
        }
        if (action->menu() != nullptr)
        {
            const QString name = StripMnemonic(action->text());
            CollectFromMenu(*action->menu(),
                            group.isEmpty() == true
                                ? name
                                : group + QStringLiteral(" ") + GROUP_SEPARATOR + QStringLiteral(" ") + name,
                            outEntries);
            continue;
        }
        const QString id = action->objectName();
        if (id == QLatin1String(PALETTE_COMMAND_ID) || id == QLatin1String(RUN_IN_CONTEXT_COMMAND_ID))
        {
            continue;
        }
        PaletteEntry entry;
        entry.Label = StripMnemonic(action->text());
        if (entry.Label.isEmpty() == true)
        {
            continue;
        }
        entry.Group = group;
        entry.Shortcut = action->shortcut().toString(QKeySequence::NativeText);
        entry.IsEnabled = action->isEnabled();
        entry.Action = action;
        outEntries.append(entry);
    }
}

/// Paints the label on the left and the group and shortcut, dimmer, on the right.
class PaletteDelegate final : public QStyledItemDelegate
{
public:
    explicit PaletteDelegate(QObject* parentOrNull) : QStyledItemDelegate(parentOrNull) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem copy = option;
        initStyleOption(&copy, index);
        const QString secondary = index.data(Qt::UserRole + 1).toString();
        copy.text = index.data(Qt::DisplayRole).toString();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &copy, painter, copy.widget);
        if (secondary.isEmpty() == false)
        {
            painter->save();
            painter->setPen(copy.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(copy.rect.adjusted(ROW_PADDING, 0, -ROW_PADDING, 0),
                              Qt::AlignRight | Qt::AlignVCenter,
                              secondary);
            painter->restore();
        }
    }
};

QString GetSecondaryText(const PaletteEntry& entry)
{
    QStringList parts;
    if (entry.Group.isEmpty() == false)
    {
        parts.append(entry.Group);
    }
    if (entry.Shortcut.isEmpty() == false)
    {
        parts.append(entry.Shortcut);
    }
    return parts.join(QStringLiteral("   "));
}
} // namespace

QList<PaletteEntry> CollectPaletteEntries(const QMenuBar& menuBar)
{
    QList<PaletteEntry> entries;
    for (QAction* top : menuBar.actions())
    {
        if (top->menu() != nullptr)
        {
            CollectFromMenu(*top->menu(), StripMnemonic(top->text()), entries);
        }
    }
    return entries;
}

int ScoreFuzzyMatch(const QString& query, const QString& candidate)
{
    QString needle = query.toLower();
    needle.remove(QLatin1Char(' '));
    if (needle.isEmpty() == true)
    {
        return 0;
    }
    const QString haystack = candidate.toLower();
    int score = 0;
    int queryIndex = 0;
    int previousMatch = -2;
    for (int index = 0; index < haystack.size() && queryIndex < needle.size(); ++index)
    {
        if (haystack.at(index) != needle.at(queryIndex))
        {
            continue;
        }
        score += MATCH_SCORE;
        if (index == previousMatch + 1)
        {
            score += CONSECUTIVE_BONUS;
        }
        if (index == 0 || haystack.at(index - 1).isLetterOrNumber() == false)
        {
            score += WORD_START_BONUS;
        }
        previousMatch = index;
        ++queryIndex;
    }
    if (queryIndex < needle.size())
    {
        return -1;
    }
    if (haystack.contains(needle) == true)
    {
        score += SUBSTRING_BONUS;
    }
    if (haystack.startsWith(needle) == true)
    {
        score += PREFIX_BONUS;
    }
    // Shorter candidates win ties: the same letters cover more of them.
    return score - static_cast<int>(haystack.size() / MATCH_SCORE);
}

PaletteIndexList RankPaletteEntries(const QList<PaletteEntry>& entries, const QString& query, bool isEnabledOnly)
{
    struct Ranked final
    {
        int Index;
        int Score;
        bool IsEnabled;
    };
    QList<Ranked> ranked;
    for (int index = 0; index < entries.size(); ++index)
    {
        const PaletteEntry& entry = entries.at(index);
        if (isEnabledOnly == true && entry.IsEnabled == false)
        {
            continue;
        }
        // The label is what people type; the menu path lets "view timing" find View ▸ Timing.
        const int labelScore = ScoreFuzzyMatch(query, entry.Label);
        const int pathScore = ScoreFuzzyMatch(query, entry.Group + QStringLiteral(" ") + entry.Label);
        const int score = std::max(labelScore, pathScore);
        if (score >= 0)
        {
            ranked.append({index, score, entry.IsEnabled});
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        if (left.IsEnabled != right.IsEnabled)
        {
            return left.IsEnabled == true;
        }
        return left.Score > right.Score;
    });
    QList<int> indices;
    for (const Ranked& item : ranked)
    {
        indices.append(item.Index);
    }
    return indices;
}

CommandPalette::CommandPalette(QList<PaletteEntry> entries, bool isEnabledOnly, QWidget* parentOrNull)
    : QDialog(parentOrNull, Qt::Dialog | Qt::FramelessWindowHint), mEntries(std::move(entries)),
      mIsEnabledOnly(isEnabledOnly)
{
    setObjectName(QStringLiteral("commandPalette"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(true);
    setAccessibleName(isEnabledOnly == true ? QStringLiteral("Run command in context")
                                            : QStringLiteral("Command palette"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(PALETTE_PADDING, PALETTE_PADDING, PALETTE_PADDING, PALETTE_PADDING);
    mSearchOrNull = new QLineEdit(this);
    mSearchOrNull->setObjectName(QStringLiteral("commandPaletteSearch"));
    mSearchOrNull->setAccessibleName(QStringLiteral("Search commands"));
    mSearchOrNull->setPlaceholderText(isEnabledOnly == true ? QStringLiteral("Run a command available here...")
                                                            : QStringLiteral("Search commands..."));
    mSearchOrNull->installEventFilter(this);
    layout->addWidget(mSearchOrNull);
    mListOrNull = new QListWidget(this);
    mListOrNull->setObjectName(QStringLiteral("commandPaletteList"));
    mListOrNull->setAccessibleName(QStringLiteral("Commands"));
    mListOrNull->setFocusPolicy(Qt::NoFocus);
    mListOrNull->setItemDelegate(new PaletteDelegate(mListOrNull));
    mListOrNull->setUniformItemSizes(true);
    layout->addWidget(mListOrNull);
    setFixedWidth(PALETTE_WIDTH);
    connect(mSearchOrNull, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(mListOrNull, &QListWidget::itemActivated, this, [this]() { runCurrent(); });
    connect(mSearchOrNull, &QLineEdit::returnPressed, this, [this]() { runCurrent(); });
    refresh();
    const int rowHeight =
        mListOrNull->sizeHintForRow(0) > 0 ? mListOrNull->sizeHintForRow(0) : mSearchOrNull->sizeHint().height();
    mListOrNull->setFixedHeight(rowHeight * PALETTE_VISIBLE_ROWS + 2 * mListOrNull->frameWidth());
    adjustSize();
    if (parentOrNull != nullptr)
    {
        const QWidget* window = parentOrNull->window();
        move(window->x() + (window->width() - width()) / 2, window->y() + PALETTE_TOP_MARGIN);
    }
    mSearchOrNull->setFocus();
}

PaletteIndexList CommandPalette::GetShownIndices() const
{
    QList<int> indices;
    for (int row = 0; row < mListOrNull->count(); ++row)
    {
        const QVariant value = mListOrNull->item(row)->data(Qt::UserRole);
        if (value.isValid() == true)
        {
            indices.append(value.toInt());
        }
    }
    return indices;
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mSearchOrNull && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up)
        {
            const int step = key == Qt::Key_Down ? 1 : -1;
            int row = mListOrNull->currentRow();
            const int count = mListOrNull->count();
            for (int tried = 0; tried < count; ++tried)
            {
                row = (row + step + count) % count;
                if ((mListOrNull->item(row)->flags() & Qt::ItemIsEnabled) != 0)
                {
                    mListOrNull->setCurrentRow(row);
                    break;
                }
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::refresh()
{
    mListOrNull->clear();
    const QList<int> indices = RankPaletteEntries(mEntries, mSearchOrNull->text(), mIsEnabledOnly);
    const int shown = std::min(static_cast<int>(indices.size()), PALETTE_MAX_RESULTS);
    for (int row = 0; row < shown; ++row)
    {
        const PaletteEntry& entry = mEntries.at(indices.at(row));
        QListWidgetItem* item = new QListWidgetItem(entry.Label, mListOrNull);
        item->setData(Qt::UserRole, indices.at(row));
        item->setData(Qt::UserRole + 1, GetSecondaryText(entry));
        item->setToolTip(entry.Action != nullptr ? entry.Action->statusTip() : QString());
        if (entry.IsEnabled == false)
        {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
    }
    if (shown == 0)
    {
        QListWidgetItem* none = new QListWidgetItem(QStringLiteral("No matching commands."), mListOrNull);
        none->setFlags(Qt::NoItemFlags);
    }
    else
    {
        for (int row = 0; row < mListOrNull->count(); ++row)
        {
            if ((mListOrNull->item(row)->flags() & Qt::ItemIsEnabled) != 0)
            {
                mListOrNull->setCurrentRow(row);
                break;
            }
        }
    }
}

void CommandPalette::runCurrent()
{
    const QListWidgetItem* item = mListOrNull->currentItem();
    if (item == nullptr || item->data(Qt::UserRole).isValid() == false)
    {
        return;
    }
    const PaletteEntry& entry = mEntries.at(item->data(Qt::UserRole).toInt());
    if (entry.IsEnabled == false || entry.Action == nullptr)
    {
        return;
    }
    const QPointer<QAction> action = entry.Action;
    // Close first so the command sees the window that had focus, not this dialog.
    accept();
    QTimer::singleShot(0, qApp, [action]() {
        if (action != nullptr && action->isEnabled() == true)
        {
            action->trigger();
        }
    });
}

ShortcutList::ShortcutList(const QList<PaletteEntry>& entries, QWidget* parentOrNull) : QDialog(parentOrNull)
{
    for (const PaletteEntry& entry : entries)
    {
        if (entry.Shortcut.isEmpty() == false)
        {
            mEntries.append(entry);
        }
    }
    setObjectName(QStringLiteral("shortcutList"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    mSearchOrNull = new QLineEdit(this);
    mSearchOrNull->setObjectName(QStringLiteral("shortcutsSearch"));
    mSearchOrNull->setAccessibleName(QStringLiteral("Search shortcuts"));
    mSearchOrNull->setPlaceholderText(QStringLiteral("Search shortcuts..."));
    mSearchOrNull->setClearButtonEnabled(true);
    layout->addWidget(mSearchOrNull);
    mTreeOrNull = new QTreeWidget(this);
    mTreeOrNull->setObjectName(QStringLiteral("shortcutsTree"));
    mTreeOrNull->setAccessibleName(QStringLiteral("Shortcuts"));
    mTreeOrNull->setColumnCount(2);
    mTreeOrNull->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut")});
    mTreeOrNull->setRootIsDecorated(false);
    layout->addWidget(mTreeOrNull, 1);
    QLabel* note = new QLabel(QStringLiteral("Shortcuts follow the same availability rules as menu actions. "
                                             "Save and Undo follow the active document tab. Undo edits focused "
                                             "text first; Project Settings also has document history. Stop "
                                             "active work before closing a project."),
                              this);
    note->setWordWrap(true);
    layout->addWidget(note);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(mSearchOrNull, &QLineEdit::textChanged, this, [this]() { refresh(); });
    resize(SHORTCUT_DIALOG_WIDTH, SHORTCUT_DIALOG_HEIGHT);
    refresh();
    mSearchOrNull->setFocus();
}

void ShortcutList::refresh()
{
    mTreeOrNull->clear();
    const QList<int> indices = RankPaletteEntries(mEntries, mSearchOrNull->text(), false);
    // Menu order, grouped by menu; a search only filters, so every group appears once.
    QList<int> ordered = indices;
    std::stable_sort(ordered.begin(), ordered.end());
    QTreeWidgetItem* groupItemOrNull = nullptr;
    QString currentGroup;
    for (const int index : ordered)
    {
        const PaletteEntry& entry = mEntries.at(index);
        if (groupItemOrNull == nullptr || entry.Group != currentGroup)
        {
            currentGroup = entry.Group;
            groupItemOrNull = new QTreeWidgetItem(mTreeOrNull, {entry.Group});
            QFont font = groupItemOrNull->font(0);
            font.setBold(true);
            groupItemOrNull->setFont(0, font);
            groupItemOrNull->setFlags(Qt::ItemIsEnabled);
            groupItemOrNull->setFirstColumnSpanned(true);
        }
        new QTreeWidgetItem(groupItemOrNull, {entry.Label, entry.Shortcut});
    }
    mTreeOrNull->expandAll();
    mTreeOrNull->resizeColumnToContents(0);
}
} // namespace ludus::editor
