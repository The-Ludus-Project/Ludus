#include "internal/command_palette.h"
#include "design_tokens.h"

#include "internal/theme_preferences.h"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPropertyAnimation>
#include <QSet>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QToolBar>
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
constexpr int PALETTE_PADDING = design::SPACING_SMALL;
constexpr int ROW_PADDING = 6;
constexpr int SHORTCUT_DIALOG_WIDTH = 520;
constexpr int SHORTCUT_DIALOG_HEIGHT = 480;
constexpr char PALETTE_COMMAND_ID[] = "tools.commandPalette";
constexpr char RUN_IN_CONTEXT_COMMAND_ID[] = "tools.runInContext";
const QChar GROUP_SEPARATOR = QChar(0x203A);
constexpr qreal HALF = 0.5;
/// Item data roles of a palette row: its entry index, its group and shortcut text, and the matched letters.
constexpr int ENTRY_INDEX_ROLE = Qt::UserRole;
constexpr int SECONDARY_TEXT_ROLE = Qt::UserRole + 1;
constexpr int MATCHED_POSITIONS_ROLE = Qt::UserRole + 2;
constexpr char WINDOW_GROUP[] = "Window";

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

/// The greedy in-order match behind ScoreFuzzyMatch, also reporting where each query letter landed.
int MatchFuzzy(const QString& query, const QString& candidate, QList<int>& outPositions)
{
    outPositions.clear();
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
        outPositions.append(index);
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
        outPositions.clear();
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
    // Shorter candidates win ties: the same letters cover more of them. A match never scores below zero, which
    // means no match.
    return std::max(0, score - static_cast<int>(haystack.size() / MATCH_SCORE));
}

/// Paints the label on the left, with the letters the search matched in bold, and the group and shortcut in the
/// secondary text colour on the right. A label too long for the room left is elided rather than run under them.
class PaletteDelegate final : public QStyledItemDelegate
{
public:
    explicit PaletteDelegate(QObject* parentOrNull) : QStyledItemDelegate(parentOrNull) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem copy = option;
        initStyleOption(&copy, index);
        const QString label = copy.text;
        const QString secondary = index.data(SECONDARY_TEXT_ROLE).toString();
        const QList<int> matched = index.data(MATCHED_POSITIONS_ROLE).value<QList<int>>();
        // The style paints the row (selection, hover and focus); the text is drawn here.
        copy.text.clear();
        QStyle* style = copy.widget != nullptr ? copy.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &copy, painter, copy.widget);
        const bool isEnabled = copy.state.testFlag(QStyle::State_Enabled);
        const bool isSelected = copy.state.testFlag(QStyle::State_Selected);
        const QRect textRect = copy.rect.adjusted(ROW_PADDING, 0, -ROW_PADDING, 0);
        const QFontMetrics metrics(copy.font);
        int labelRight = textRect.right();
        painter->save();
        if (secondary.isEmpty() == false)
        {
            painter->setPen(copy.palette.color(QPalette::Active, QPalette::PlaceholderText));
            painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, secondary);
            labelRight -= metrics.horizontalAdvance(secondary) + 2 * ROW_PADDING;
        }
        const QRect labelRect(textRect.left(),
                              textRect.top(),
                              std::max(0, labelRight - textRect.left()),
                              textRect.height());
        QColor labelColour = copy.palette.color(QPalette::Active, QPalette::Text);
        if (isEnabled == false)
        {
            labelColour = copy.palette.color(QPalette::Disabled, QPalette::Text);
        }
        else if (isSelected == true)
        {
            labelColour = copy.palette.color(QPalette::Active, QPalette::HighlightedText);
        }
        painter->setPen(labelColour);
        const QString elided = metrics.elidedText(label, Qt::ElideRight, labelRect.width());
        if (elided != label || matched.isEmpty() == true)
        {
            painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
            painter->restore();
            return;
        }
        QTextLayout layout(label, copy.font);
        QList<QTextLayout::FormatRange> ranges;
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        for (const int position : matched)
        {
            if (position >= 0 && position < label.size())
            {
                ranges.append({position, 1, bold});
            }
        }
        layout.setFormats(ranges);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(static_cast<qreal>(labelRect.width()));
        layout.endLayout();
        const qreal top =
            static_cast<qreal>(labelRect.top()) + (static_cast<qreal>(labelRect.height()) - line.height()) * HALF;
        layout.draw(painter, QPointF(static_cast<qreal>(labelRect.left()), top));
        painter->restore();
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

QList<PaletteEntry> CollectWindowPaletteEntries(const QMainWindow& window)
{
    QList<PaletteEntry> entries = CollectPaletteEntries(*window.menuBar());
    QSet<const QAction*> seen;
    for (const PaletteEntry& entry : entries)
    {
        seen.insert(entry.Action.data());
    }
    const auto append = [&entries, &seen](QAction* action, const QString& group) {
        if (action->isSeparator() == true || action->menu() != nullptr || seen.contains(action) == true)
        {
            return;
        }
        const QString id = action->objectName();
        if (id == QLatin1String(PALETTE_COMMAND_ID) || id == QLatin1String(RUN_IN_CONTEXT_COMMAND_ID))
        {
            return;
        }
        PaletteEntry entry;
        entry.Label = StripMnemonic(action->text());
        if (entry.Label.isEmpty() == true)
        {
            return;
        }
        entry.Group = group;
        entry.Shortcut = action->shortcut().toString(QKeySequence::NativeText);
        entry.IsEnabled = action->isEnabled();
        entry.Action = action;
        seen.insert(action);
        entries.append(entry);
    };
    for (QToolBar* toolbar : window.findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly))
    {
        for (QAction* action : toolbar->actions())
        {
            append(action, toolbar->windowTitle());
        }
    }
    for (QAction* action : window.actions())
    {
        append(action, QString::fromLatin1(WINDOW_GROUP));
    }
    return entries;
}

int ScoreFuzzyMatch(const QString& query, const QString& candidate)
{
    QList<int> positions;
    return MatchFuzzy(query, candidate, positions);
}

QList<int> FindFuzzyMatchPositions(const QString& query, const QString& candidate)
{
    QList<int> positions;
    static_cast<void>(MatchFuzzy(query, candidate, positions));
    return positions;
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
    // Keyboard first: say how to drive it, in the quiet secondary style.
    QLabel* hint = new QLabel(QStringLiteral("↑ ↓ choose   Enter run   Esc close"), this);
    hint->setObjectName(QStringLiteral("commandPaletteHint"));
    hint->setProperty("visualRole", QStringLiteral("secondary"));
    layout->addWidget(hint);
    setFixedWidth(PALETTE_WIDTH);
    connect(mListOrNull, &QListWidget::currentRowChanged, this, [this]() { describeCurrent(); });
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
        const QVariant value = mListOrNull->item(row)->data(ENTRY_INDEX_ROLE);
        if (value.isValid() == true)
        {
            indices.append(value.toInt());
        }
    }
    return indices;
}

bool CommandPalette::IsFading() const
{
    return mFadeOrNull != nullptr && mFadeOrNull->state() == QAbstractAnimation::Running;
}

void CommandPalette::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    const int duration = GetMotionDurationMilliseconds(GetThemePreferences(), PALETTE_FADE_MILLISECONDS);
    if (duration <= 0)
    {
        // Reduced motion: appear at once, never half transparent.
        if (mFadeOrNull != nullptr)
        {
            mFadeOrNull->stop();
        }
        setWindowOpacity(1.0);
        return;
    }
    if (mFadeOrNull == nullptr)
    {
        mFadeOrNull = new QPropertyAnimation(this, QByteArrayLiteral("windowOpacity"), this);
        mFadeOrNull->setStartValue(0.0);
        mFadeOrNull->setEndValue(1.0);
        mFadeOrNull->setEasingCurve(QEasingCurve::OutCubic);
    }
    mFadeOrNull->setDuration(duration);
    setWindowOpacity(0.0);
    mFadeOrNull->start();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mSearchOrNull && event->type() == QEvent::KeyPress)
    {
        // Focus stays in the search box; the arrows and page keys move the chosen command.
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up)
        {
            moveCurrent(key == Qt::Key_Down ? 1 : -1);
            return true;
        }
        if (key == Qt::Key_PageDown || key == Qt::Key_PageUp)
        {
            moveCurrent(key == Qt::Key_PageDown ? PALETTE_VISIBLE_ROWS : -PALETTE_VISIBLE_ROWS);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::moveCurrent(int step)
{
    const int count = mListOrNull->count();
    if (count == 0)
    {
        return;
    }
    const int direction = step > 0 ? 1 : -1;
    const bool isPage = step > 1 || step < -1;
    int row = mListOrNull->currentRow();
    if (isPage == true)
    {
        // A page stops at the ends instead of wrapping, then settles on the nearest available command.
        row = std::clamp(row + step, 0, count - 1);
        for (int tried = 0; tried < count; ++tried)
        {
            if ((mListOrNull->item(row)->flags() & Qt::ItemIsEnabled) != 0)
            {
                mListOrNull->setCurrentRow(row);
                return;
            }
            row = std::clamp(row - direction, 0, count - 1);
        }
        return;
    }
    for (int tried = 0; tried < count; ++tried)
    {
        row = (row + direction + count) % count;
        if ((mListOrNull->item(row)->flags() & Qt::ItemIsEnabled) != 0)
        {
            mListOrNull->setCurrentRow(row);
            return;
        }
    }
}

void CommandPalette::describeCurrent()
{
    // The list never takes focus, so the search box tells assistive technology which command Enter runs.
    const QListWidgetItem* item = mListOrNull->currentItem();
    if (item == nullptr || item->data(ENTRY_INDEX_ROLE).isValid() == false)
    {
        mSearchOrNull->setAccessibleDescription(QString());
        return;
    }
    const PaletteEntry& entry = mEntries.at(item->data(ENTRY_INDEX_ROLE).toInt());
    mSearchOrNull->setAccessibleDescription(
        entry.Shortcut.isEmpty() == true ? entry.Label : entry.Label + QStringLiteral(", ") + entry.Shortcut);
}

void CommandPalette::refresh()
{
    mListOrNull->clear();
    const QString query = mSearchOrNull->text();
    const QList<int> indices = RankPaletteEntries(mEntries, query, mIsEnabledOnly);
    const int shown = std::min(static_cast<int>(indices.size()), PALETTE_MAX_RESULTS);
    for (int row = 0; row < shown; ++row)
    {
        const PaletteEntry& entry = mEntries.at(indices.at(row));
        QListWidgetItem* item = new QListWidgetItem(entry.Label, mListOrNull);
        item->setData(ENTRY_INDEX_ROLE, indices.at(row));
        item->setData(SECONDARY_TEXT_ROLE, GetSecondaryText(entry));
        item->setData(MATCHED_POSITIONS_ROLE, QVariant::fromValue(FindFuzzyMatchPositions(query, entry.Label)));
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
    if (item == nullptr || item->data(ENTRY_INDEX_ROLE).isValid() == false)
    {
        return;
    }
    const PaletteEntry& entry = mEntries.at(item->data(ENTRY_INDEX_ROLE).toInt());
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
