#include "internal/action_strip.h"
#include "design_tokens.h"

#include <algorithm>

#include <QAbstractButton>
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
/// Space between neighbouring actions, and between the last action shown and the More button.
constexpr int32 ACTION_SPACING = design::SPACING_EXTRA_SMALL;
} // namespace

ActionStrip::ActionStrip(QWidget* parentOrNull)
    : QWidget(parentOrNull), mOverflow(new QToolButton(this)), mMenu(new QMenu(mOverflow))
{
    setObjectName(QStringLiteral("actionStrip"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mOverflow->setObjectName(QStringLiteral("actionStripMore"));
    mOverflow->setText(QStringLiteral("More"));
    mOverflow->setAccessibleName(QStringLiteral("More actions"));
    mOverflow->setToolTip(QStringLiteral("Actions that do not fit the panel's width."));
    mOverflow->setAutoRaise(true);
    mOverflow->setPopupMode(QToolButton::InstantPopup);
    mOverflow->setMenu(mMenu);
    mOverflow->hide();
    connect(mMenu, &QMenu::aboutToShow, this, &ActionStrip::populateMenu);
}

void ActionStrip::AddAction(QAbstractButton& inoutAction)
{
    inoutAction.setParent(this);
    mItems.append(Item{&inoutAction, true});
    updateGeometry();
    arrange();
}

void ActionStrip::SetActionVisible(QAbstractButton& inoutAction, bool isVisible)
{
    Item* itemOrNull = findItemOrNull(inoutAction);
    if (itemOrNull == nullptr || itemOrNull->IsVisible == isVisible)
    {
        return;
    }
    itemOrNull->IsVisible = isVisible;
    updateGeometry();
    arrange();
}

QSize ActionStrip::sizeHint() const
{
    int32 width = 0;
    int32 height = mOverflow->sizeHint().height();
    for (const Item& item : mItems)
    {
        if (item.IsVisible == false)
        {
            continue;
        }
        width += item.ButtonOrNull->sizeHint().width() + (width > 0 ? ACTION_SPACING : 0);
        height = std::max(height, item.ButtonOrNull->sizeHint().height());
    }
    return {width, height};
}

QSize ActionStrip::minimumSizeHint() const
{
    return {mOverflow->sizeHint().width(), sizeHint().height()};
}

bool ActionStrip::event(QEvent* event)
{
    if (event->type() == QEvent::LayoutRequest)
    {
        updateGeometry();
        arrange();
    }
    return QWidget::event(event);
}

void ActionStrip::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    arrange();
}

void ActionStrip::arrange()
{
    const int32 height = sizeHint().height();
    const int32 overflowWidth = mOverflow->sizeHint().width();
    const bool isEverythingFitting = sizeHint().width() <= width();
    const int32 limit = isEverythingFitting == true ? width() : width() - overflowWidth - ACTION_SPACING;
    int32 x = 0;
    int32 overflowCount = 0;
    for (Item& item : mItems)
    {
        if (item.IsVisible == false)
        {
            item.ButtonOrNull->setVisible(false);
            continue;
        }
        const int32 actionWidth = item.ButtonOrNull->sizeHint().width();
        const bool isFitting = overflowCount == 0 && x + actionWidth <= limit;
        if (isFitting == true)
        {
            item.ButtonOrNull->setGeometry(x, 0, actionWidth, height);
            x += actionWidth + ACTION_SPACING;
        }
        else
        {
            ++overflowCount;
        }
        item.ButtonOrNull->setVisible(isFitting);
    }
    mOverflowCount = overflowCount;
    mOverflow->setGeometry(x, 0, overflowWidth, height);
    mOverflow->setVisible(overflowCount > 0);
}

void ActionStrip::populateMenu()
{
    mMenu->clear();
    for (const Item& item : mItems)
    {
        if (item.IsVisible == false || item.ButtonOrNull->isHidden() == false)
        {
            continue;
        }
        QAction* action = mMenu->addAction(item.ButtonOrNull->text());
        action->setEnabled(item.ButtonOrNull->isEnabled());
        action->setToolTip(item.ButtonOrNull->toolTip());
        QAbstractButton* button = item.ButtonOrNull;
        connect(action, &QAction::triggered, button, [button]() { button->click(); });
    }
}

ActionStrip::Item* ActionStrip::findItemOrNull(QAbstractButton& inoutAction) noexcept
{
    for (Item& item : mItems)
    {
        if (item.ButtonOrNull == &inoutAction)
        {
            return &item;
        }
    }
    return nullptr;
}
} // namespace ludus::editor
