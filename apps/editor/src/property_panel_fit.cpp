#include "internal/property_panel_fit.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QSet>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTableView>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace ludus::editor
{
namespace
{
using foundation::int32;
/// A choice keeps room for this many characters before its text is elided.
constexpr int32 MINIMUM_CHOICE_CHARACTERS = 8;
/// Marks a choice whose tooltip follows its current text, so it is connected only once.
constexpr const char* FOLLOWS_TEXT_PROPERTY = "propertyPanelTooltipFollowsText";
/// A fitted column keeps room for this many characters (or its whole content, when that is narrower).
constexpr int32 MINIMUM_COLUMN_CHARACTERS = 6;

bool IsInsideItemView(const QWidget& widget)
{
    for (const QWidget* ancestorOrNull = widget.parentWidget(); ancestorOrNull != nullptr;
         ancestorOrNull = ancestorOrNull->parentWidget())
    {
        if (qobject_cast<const QAbstractItemView*>(ancestorOrNull) != nullptr)
        {
            return true;
        }
    }
    return false;
}

void FitChoice(QComboBox& inoutChoice)
{
    inoutChoice.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    inoutChoice.setMinimumContentsLength(MINIMUM_CHOICE_CHARACTERS);
    inoutChoice.setSizePolicy(QSizePolicy::Expanding, inoutChoice.sizePolicy().verticalPolicy());
    // A choice with its own explanation keeps it; otherwise its tooltip carries the full, possibly elided, text.
    const bool isTooltipFree =
        inoutChoice.toolTip().isEmpty() == true || inoutChoice.toolTip() == inoutChoice.currentText();
    if (isTooltipFree == true && inoutChoice.property(FOLLOWS_TEXT_PROPERTY).toBool() == false)
    {
        inoutChoice.setProperty(FOLLOWS_TEXT_PROPERTY, true);
        inoutChoice.setToolTip(inoutChoice.currentText());
        QObject::connect(&inoutChoice, &QComboBox::currentTextChanged, &inoutChoice, &QWidget::setToolTip);
    }
}
/// The width a choice needs to show its current text in full, rather than its longest item.
int32 GetChoiceWidth(const QComboBox& choice)
{
    QStyleOptionComboBox option;
    option.initFrom(&choice);
    option.currentText = choice.currentText();
    const QSize text(choice.fontMetrics().horizontalAdvance(choice.currentText()), choice.fontMetrics().height());
    return choice.style()->sizeFromContents(QStyle::CT_ComboBox, &option, text, &choice).width();
}

class ColumnFitter final : public QObject
{
public:
    explicit ColumnFitter(QTableView& inoutTable) : QObject(&inoutTable), mTable(inoutTable)
    {
        QHeaderView& header = *mTable.horizontalHeader();
        header.setStretchLastSection(false);
        header.setSectionResizeMode(QHeaderView::Fixed);
        mTable.viewport()->installEventFilter(this);
        QAbstractItemModel* modelOrNull = mTable.model();
        if (modelOrNull != nullptr)
        {
            // Cell editors arrive after their row, so the refit waits for the current event to finish.
            const auto refit = [this]() { QTimer::singleShot(0, this, [this]() { fit(); }); };
            connect(modelOrNull, &QAbstractItemModel::rowsInserted, this, refit);
            connect(modelOrNull, &QAbstractItemModel::rowsRemoved, this, refit);
            connect(modelOrNull, &QAbstractItemModel::dataChanged, this, refit);
            connect(modelOrNull, &QAbstractItemModel::modelReset, this, refit);
        }
        fit();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == mTable.viewport() && event->type() == QEvent::Resize)
        {
            fit();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    [[nodiscard]] int32 getPreferredWidth(int32 column) const
    {
        const QAbstractItemModel& model = *mTable.model();
        int32 width = mTable.horizontalHeader()->sectionSizeHint(column);
        for (int32 row = 0; row < model.rowCount(); ++row)
        {
            const QModelIndex index = model.index(row, column);
            const QWidget* cellOrNull = mTable.indexWidget(index);
            const QComboBox* choiceOrNull = qobject_cast<const QComboBox*>(cellOrNull);
            width = std::max(width,
                             choiceOrNull != nullptr ? GetChoiceWidth(*choiceOrNull)
                             : cellOrNull != nullptr ? cellOrNull->sizeHint().width()
                                                     : mTable.sizeHintForIndex(index).width());
        }
        // The grid line between columns takes a pixel of each section.
        return width + (mTable.showGrid() == true ? 1 : 0);
    }

    /// Spare width goes to the last column of plain text (a name, a program), so a choice or a check box does
    /// not stretch into a wide empty cell. A table without such a column, or without rows, stretches its last.
    [[nodiscard]] int32 getStretchColumn(int32 last) const
    {
        const QAbstractItemModel& model = *mTable.model();
        if (model.rowCount() == 0)
        {
            return last;
        }
        for (int32 column = last; column >= 0; --column)
        {
            const QModelIndex index = model.index(0, column);
            // Items are user-checkable by default, so a check box column is told apart by holding a check state.
            const bool isText = mTable.isColumnHidden(column) == false && mTable.indexWidget(index) == nullptr &&
                                index.data(Qt::CheckStateRole).isValid() == false;
            if (isText == true)
            {
                return column;
            }
        }
        return last;
    }

    void fit()
    {
        const QAbstractItemModel* modelOrNull = mTable.model();
        if (modelOrNull == nullptr || modelOrNull->columnCount() == 0)
        {
            return;
        }
        const int32 columns = modelOrNull->columnCount();
        const int32 floor = mTable.fontMetrics().averageCharWidth() * MINIMUM_COLUMN_CHARACTERS;
        QList<int32> widths(columns, 0);
        QList<int32> slacks(columns, 0);
        int32 total = 0;
        int32 slack = 0;
        int32 last = -1;
        for (int32 column = 0; column < columns; ++column)
        {
            if (mTable.isColumnHidden(column) == true)
            {
                continue;
            }
            widths[column] = getPreferredWidth(column);
            slacks[column] = std::max(0, widths[column] - floor);
            total += widths[column];
            slack += slacks[column];
            last = column;
        }
        if (last < 0)
        {
            return;
        }
        const int32 available = mTable.viewport()->width();
        if (total <= available)
        {
            widths[getStretchColumn(last)] += available - total;
        }
        else if (slack > 0)
        {
            // The widest columns give way first, down to a shared cap, so short columns (a name, a check box) keep
            // their whole text and only long ones elide. No column goes below its floor.
            const auto getWidth = [&preferred = std::as_const(widths), &slacks](int32 column, int32 cap) {
                const int32 floorWidth = preferred[column] - slacks[column];
                return std::max(floorWidth, std::min(preferred[column], cap));
            };
            int32 low = 0;
            int32 high = *std::max_element(widths.cbegin(), widths.cend());
            while (low < high)
            {
                const int32 cap = (low + high + 1) / 2;
                int32 sum = 0;
                for (int32 column = 0; column <= last; ++column)
                {
                    sum += getWidth(column, cap);
                }
                if (sum <= available)
                {
                    low = cap;
                }
                else
                {
                    high = cap - 1;
                }
            }
            QList<int32> fitted(columns, 0);
            int32 used = 0;
            for (int32 column = 0; column <= last; ++column)
            {
                fitted[column] = getWidth(column, low);
                used += fitted[column];
            }
            // The cap is whole pixels; the last column takes what is left over.
            fitted[last] += std::max(0, available - used);
            widths = fitted;
        }
        for (int32 column = 0; column <= last; ++column)
        {
            if (mTable.isColumnHidden(column) == false &&
                mTable.horizontalHeader()->sectionSize(column) != widths[column])
            {
                mTable.horizontalHeader()->resizeSection(column, widths[column]);
            }
        }
    }

    QTableView& mTable;
};
} // namespace

void FitColumnsToWidth(QTableView& inoutTable)
{
    new ColumnFitter(inoutTable);
}

void FitPropertiesToWidth(QWidget& inoutPanel)
{
    // A form's row labels stay on one line: the row puts its field underneath instead when both do not fit.
    QSet<const QWidget*> rowLabels;
    for (QFormLayout* form : inoutPanel.findChildren<QFormLayout*>())
    {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (int32 row = 0; row < form->rowCount(); ++row)
        {
            const QLayoutItem* labelOrNull = form->itemAt(row, QFormLayout::LabelRole);
            if (labelOrNull != nullptr && labelOrNull->widget() != nullptr)
            {
                rowLabels.insert(labelOrNull->widget());
            }
        }
    }
    for (QWidget* child : inoutPanel.findChildren<QWidget*>())
    {
        // Cell editors live inside their table's own scrolling viewport and never widen the panel.
        if (IsInsideItemView(*child) == true)
        {
            continue;
        }
        if (QComboBox* choiceOrNull = qobject_cast<QComboBox*>(child); choiceOrNull != nullptr)
        {
            FitChoice(*choiceOrNull);
        }
        else if (QLabel* labelOrNull = qobject_cast<QLabel*>(child); labelOrNull != nullptr)
        {
            if (rowLabels.contains(labelOrNull) == false && labelOrNull->wordWrap() == false &&
                labelOrNull->text().contains(QLatin1Char(' ')) == true && labelOrNull->pixmap().isNull() == true)
            {
                labelOrNull->setWordWrap(true);
            }
        }
        else if (QAbstractItemView* viewOrNull = qobject_cast<QAbstractItemView*>(child); viewOrNull != nullptr)
        {
            // The table keeps its height from its rows but takes its width from the panel, scrolling its columns.
            viewOrNull->setSizePolicy(QSizePolicy::Ignored, viewOrNull->sizePolicy().verticalPolicy());
            viewOrNull->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        }
    }
}
} // namespace ludus::editor
