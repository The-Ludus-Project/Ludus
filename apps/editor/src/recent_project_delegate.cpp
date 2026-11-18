#include "internal/recent_project_delegate.h"

#include "design_tokens.h"
#include "internal/editor_style.h"
#include "internal/path_text.h"
#include "internal/theme_colour.h"
#include "internal/theme_fonts.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include <algorithm>

namespace ludus::editor
{
namespace
{
/// Edge length of the project icon.
constexpr int RECENT_PROJECT_ICON_SIZE = 32;
/// Gap between the name line and the path line.
constexpr int NAME_PATH_GAP = 2;

struct RowFonts
{
    QFont Name;
    QFont Detail;
};

[[nodiscard]] RowFonts MakeRowFonts(const QFont& base)
{
    QFont name = MakeTypeFont(base, TypeRole::Body);
    name.setBold(true);
    return { .Name = name, .Detail = MakeTypeFont(base, TypeRole::Caption) };
}

[[nodiscard]] int GetSectionHeadingHeight(const QFont& detail)
{
    return design::SPACING_LARGE + QFontMetrics(detail).height() + design::SPACING_EXTRA_SMALL;
}

[[nodiscard]] QColor GetSecondaryColour(const QStyleOptionViewItem& option, bool isSelected)
{
    return isSelected == true ? option.palette.color(QPalette::HighlightedText)
                              : option.palette.color(QPalette::PlaceholderText);
}
} // namespace

QString GetMissingRecentProjectsHeading()
{
    return QStringLiteral("Missing — these project files were moved or deleted");
}

RecentProjectDelegate::RecentProjectDelegate(QObject* parentOrNull) : QStyledItemDelegate(parentOrNull) {}

void RecentProjectDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem row = option;
    initStyleOption(&row, index);
    const QWidget* widgetOrNull = row.widget;
    const QStyle* style = widgetOrNull != nullptr ? widgetOrNull->style() : QApplication::style();
    const RowFonts fonts = MakeRowFonts(row.font);
    const QFontMetrics nameMetrics(fonts.Name);
    const QFontMetrics detailMetrics(fonts.Detail);
    const bool isMissing = index.data(RECENT_PROJECT_IS_MISSING_ROLE).toBool();
    const bool isSelected = (row.state & QStyle::State_Selected) != 0;

    QRect rowRect = row.rect;
    painter->save();
    if (index.data(RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE).toBool() == true)
    {
        // The heading is part of the first missing row so the list keeps one item per project.
        const int headingHeight = GetSectionHeadingHeight(fonts.Detail);
        const QRect heading(rowRect.left(), rowRect.top(), rowRect.width(), headingHeight);
        painter->setPen(row.palette.color(QPalette::Mid));
        const int dividerY = heading.top() + design::SPACING_SMALL;
        painter->drawLine(heading.left(), dividerY, heading.right(), dividerY);
        painter->setFont(fonts.Detail);
        painter->setPen(row.palette.color(QPalette::PlaceholderText));
        const QRect headingText = heading.adjusted(RECENT_PROJECT_ITEM_PADDING, 0, -RECENT_PROJECT_ITEM_PADDING, 0);
        painter->drawText(
            headingText,
            Qt::AlignLeft | Qt::AlignBottom,
            detailMetrics.elidedText(GetMissingRecentProjectsHeading(), Qt::ElideRight, headingText.width()));
        rowRect.setTop(heading.bottom() + 1);
    }

    QStyleOptionViewItem panel = row;
    panel.rect = rowRect;
    panel.text.clear();
    panel.icon = QIcon();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &panel, painter, widgetOrNull);

    const QRect content = rowRect.adjusted(RECENT_PROJECT_ITEM_PADDING,
                                           RECENT_PROJECT_ITEM_PADDING,
                                           -RECENT_PROJECT_ITEM_PADDING,
                                           -RECENT_PROJECT_ITEM_PADDING);
    int textLeft = content.left();
    if (row.icon.isNull() == false)
    {
        const QRect iconRect(content.left(),
                             content.center().y() - RECENT_PROJECT_ICON_SIZE / 2,
                             RECENT_PROJECT_ICON_SIZE,
                             RECENT_PROJECT_ICON_SIZE);
        row.icon.paint(painter, iconRect, Qt::AlignCenter, isMissing == true ? QIcon::Disabled : QIcon::Normal);
        textLeft = iconRect.right() + 1 + design::SPACING_MEDIUM;
    }

    int textRight = content.right();
    const int textHeight = nameMetrics.height() + NAME_PATH_GAP + detailMetrics.height();
    const int nameTop = content.center().y() - textHeight / 2;
    if (isMissing == true)
    {
        const QString badge = QStringLiteral("Missing");
        const int badgeWidth = detailMetrics.horizontalAdvance(badge) + design::SPACING_SMALL * 2;
        const int badgeHeight = detailMetrics.height() + design::SPACING_EXTRA_SMALL;
        const QRect badgeRect(textRight - badgeWidth + 1,
                              nameTop + (nameMetrics.height() - badgeHeight) / 2,
                              badgeWidth,
                              badgeHeight);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(GetThemeQColour(design::ThemeColour::StatusWarningSurface));
        painter->drawRoundedRect(badgeRect, design::RADIUS_CONTROL, design::RADIUS_CONTROL);
        painter->setFont(fonts.Detail);
        painter->setPen(GetThemeQColour(design::ThemeColour::StatusWarning));
        painter->drawText(badgeRect, Qt::AlignCenter, badge);
        textRight = badgeRect.left() - design::SPACING_SMALL;
    }

    const int textWidth = std::max(0, textRight - textLeft + 1);
    const QRect nameRect(textLeft, nameTop, textWidth, nameMetrics.height());
    QColor nameColour =
        isSelected == true ? row.palette.color(QPalette::HighlightedText) : row.palette.color(QPalette::Text);
    if (isMissing == true && isSelected == false)
    {
        nameColour = row.palette.color(QPalette::Disabled, QPalette::Text);
    }
    painter->setFont(fonts.Name);
    painter->setPen(nameColour);
    const QString name = index.data(RECENT_PROJECT_NAME_ROLE).toString();
    painter->drawText(nameRect,
                      Qt::AlignLeft | Qt::AlignVCenter,
                      nameMetrics.elidedText(name, Qt::ElideRight, textWidth));

    const int pathWidth = std::max(0, content.right() - textLeft + 1);
    const QRect pathRect(textLeft, nameRect.bottom() + 1 + NAME_PATH_GAP, pathWidth, detailMetrics.height());
    painter->setFont(fonts.Detail);
    painter->setPen(GetSecondaryColour(row, isSelected));
    painter->drawText(pathRect,
                      Qt::AlignLeft | Qt::AlignVCenter,
                      ElidePathText(index.data(Qt::UserRole).toString(), detailMetrics, pathWidth));
    painter->restore();
}

QSize RecentProjectDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const RowFonts fonts = MakeRowFonts(option.font);
    const int textHeight = QFontMetrics(fonts.Name).height() + NAME_PATH_GAP + QFontMetrics(fonts.Detail).height();
    int height = std::max(textHeight, RECENT_PROJECT_ICON_SIZE) + RECENT_PROJECT_ITEM_PADDING * 2;
    if (index.data(RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE).toBool() == true)
    {
        height += GetSectionHeadingHeight(fonts.Detail);
    }
    return {QStyledItemDelegate::sizeHint(option, index).width(), height};
}
} // namespace ludus::editor
