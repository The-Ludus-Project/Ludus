#include "internal/flow_layout.h"

#include <algorithm>

#include <QWidget>

namespace ludus::editor
{
using foundation::int32;

FlowLayout::FlowLayout(QWidget* parentOrNull, int32 spacingPixels) : QLayout(parentOrNull)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(spacingPixels);
}

FlowLayout::~FlowLayout()
{
    for (QLayoutItem* item : mItems)
    {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item)
{
    mItems.append(item);
    invalidate();
}

int FlowLayout::count() const
{
    return static_cast<int>(mItems.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return mItems.value(index, nullptr);
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= mItems.size())
    {
        return nullptr;
    }
    QLayoutItem* item = mItems.takeAt(index);
    invalidate();
    return item;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}

void FlowLayout::setGeometry(const QRect& rectangle)
{
    QLayout::setGeometry(rectangle);
    static_cast<void>(arrange(rectangle, true));
}

QSize FlowLayout::sizeHint() const
{
    int32 width = 0;
    int32 height = 0;
    int32 shown = 0;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        const QSize hint = item->sizeHint();
        width += hint.width() + (shown > 0 ? spacing() : 0);
        height = std::max(height, hint.height());
        ++shown;
    }
    const QMargins margins = contentsMargins();
    return {width + margins.left() + margins.right(), height + margins.top() + margins.bottom()};
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == false)
        {
            size = size.expandedTo(item->minimumSize());
        }
    }
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int32 FlowLayout::arrange(const QRect& area, bool shouldPlace) const
{
    const QMargins margins = contentsMargins();
    const QRect inner = area.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    int32 x = inner.x();
    int32 y = inner.y();
    int32 rowHeight = 0;
    for (QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        const QSize hint = item->sizeHint().boundedTo(QSize(std::max(inner.width(), 0), item->sizeHint().height()));
        if (x > inner.x() && x + hint.width() > inner.right() + 1)
        {
            x = inner.x();
            y += rowHeight + spacing();
            rowHeight = 0;
        }
        if (shouldPlace == true)
        {
            item->setGeometry(QRect(QPoint(x, y), hint));
        }
        x += hint.width() + spacing();
        rowHeight = std::max(rowHeight, hint.height());
    }
    return y + rowHeight - area.y() + margins.bottom();
}
} // namespace ludus::editor
