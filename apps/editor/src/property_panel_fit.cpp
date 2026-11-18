#include "internal/property_panel_fit.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSet>
#include <QWidget>

namespace ludus::editor
{
namespace
{
using foundation::int32;
/// A choice keeps room for this many characters before its text is elided.
constexpr int32 MINIMUM_CHOICE_CHARACTERS = 8;
/// Marks a choice whose tooltip follows its current text, so it is connected only once.
constexpr const char* FOLLOWS_TEXT_PROPERTY = "propertyPanelTooltipFollowsText";

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
} // namespace

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
