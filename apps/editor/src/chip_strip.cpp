#include "internal/chip_strip.h"
#include "design_tokens.h"

#include <algorithm>

#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QResizeEvent>
#include <QToolButton>

namespace ludus::editor
{
namespace
{
using foundation::int32;
/// Space between neighbouring chips, and between the last chip shown and the More button.
constexpr int32 CHIP_SPACING = design::SPACING_EXTRA_SMALL;
} // namespace

ChipStrip::ChipStrip(QWidget* parentOrNull)
    : QWidget(parentOrNull), mOverflow(new QToolButton(this)), mMenu(new QMenu(mOverflow))
{
    setObjectName(QStringLiteral("chipStrip"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mOverflow->setObjectName(QStringLiteral("chipStripMore"));
    mOverflow->setText(QStringLiteral("More"));
    mOverflow->setAccessibleName(QStringLiteral("More filters"));
    mOverflow->setToolTip(QStringLiteral("Filters that do not fit the panel's width."));
    mOverflow->setAutoRaise(true);
    // A toggle, so it fills with the accent while a filter it holds is on; the click itself only opens the menu.
    mOverflow->setProperty("visualRole", QStringLiteral("toggle"));
    mOverflow->setCheckable(true);
    mOverflow->setPopupMode(QToolButton::InstantPopup);
    mOverflow->setMenu(mMenu);
    mOverflow->hide();
    connect(mMenu, &QMenu::aboutToShow, this, &ChipStrip::populateMenu);
}

void ChipStrip::AddChip(QToolButton& inoutChip)
{
    inoutChip.setParent(this);
    mChips.append(&inoutChip);
    connect(&inoutChip, &QToolButton::toggled, this, &ChipStrip::renderOverflowState);
    updateGeometry();
    arrange();
}

QSize ChipStrip::sizeHint() const
{
    int32 width = 0;
    int32 height = mOverflow->sizeHint().height();
    for (const QToolButton* chip : mChips)
    {
        width += chip->sizeHint().width() + (width > 0 ? CHIP_SPACING : 0);
        height = std::max(height, chip->sizeHint().height());
    }
    return {width, height};
}

QSize ChipStrip::minimumSizeHint() const
{
    return {mOverflow->sizeHint().width(), sizeHint().height()};
}

bool ChipStrip::event(QEvent* event)
{
    // A chip whose label changed (its count) asks its parent for a new layout.
    if (event->type() == QEvent::LayoutRequest)
    {
        updateGeometry();
        arrange();
    }
    return QWidget::event(event);
}

void ChipStrip::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    arrange();
}

void ChipStrip::arrange()
{
    const int32 height = sizeHint().height();
    const int32 overflowWidth = mOverflow->sizeHint().width();
    const bool isEverythingFitting = sizeHint().width() <= width();
    // Without every chip, the More button needs its room at the end.
    const int32 limit = isEverythingFitting == true ? width() : width() - overflowWidth - CHIP_SPACING;
    int32 x = 0;
    int32 overflowCount = 0;
    for (QToolButton* chip : mChips)
    {
        const int32 chipWidth = chip->sizeHint().width();
        const bool isFitting = overflowCount == 0 && x + chipWidth <= limit;
        if (isFitting == true)
        {
            chip->setGeometry(x, 0, chipWidth, height);
            x += chipWidth + CHIP_SPACING;
        }
        else
        {
            ++overflowCount;
        }
        chip->setVisible(isFitting);
    }
    mOverflowCount = overflowCount;
    mOverflow->setGeometry(x, 0, overflowWidth, height);
    mOverflow->setVisible(overflowCount > 0);
    renderOverflowState();
}

void ChipStrip::populateMenu()
{
    mMenu->clear();
    for (QToolButton* chip : mChips)
    {
        if (chip->isHidden() == false)
        {
            continue;
        }
        QAction* action = mMenu->addAction(chip->text());
        action->setCheckable(true);
        action->setChecked(chip->isChecked());
        action->setEnabled(chip->isEnabled());
        action->setToolTip(chip->toolTip());
        connect(action, &QAction::triggered, chip, [chip]() { chip->click(); });
    }
}

void ChipStrip::renderOverflowState()
{
    bool isHiddenChipOn = false;
    for (const QToolButton* chip : mChips)
    {
        isHiddenChipOn = isHiddenChipOn == true || (chip->isHidden() == true && chip->isChecked() == true);
    }
    mOverflow->setChecked(isHiddenChipOn);
}
} // namespace ludus::editor
