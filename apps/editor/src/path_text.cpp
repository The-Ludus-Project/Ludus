#include "internal/path_text.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QToolTip>

namespace ludus::editor
{
namespace
{
/// The dimmed folder keeps this share of the text colour's opacity (percent).
constexpr int FOLDER_OPACITY_PERCENT = 62;
constexpr int PERCENT = 100;
/// A path label asks for no more than this many average characters of width.
constexpr int MINIMUM_LABEL_CHARACTERS = 8;

[[nodiscard]] bool IsSeparator(QChar character)
{
    return character == QLatin1Char('/') || character == QLatin1Char('\\');
}
} // namespace

void SplitPathText(const QString& text, QString& outFolder, QString& outName)
{
    qsizetype split = -1;
    for (qsizetype index = text.size() - 1; index >= 0 && split < 0; --index)
    {
        split = IsSeparator(text.at(index)) == true ? index : split;
    }
    outFolder = split < 0 ? QString() : text.left(split + 1);
    outName = split < 0 ? text : text.mid(split + 1);
}

PathItemDelegate::PathItemDelegate(QObject* parentOrNull) : QStyledItemDelegate(parentOrNull) {}

QString ElidePathText(const QString& text, const QFontMetrics& metrics, int width)
{
    if (metrics.horizontalAdvance(text) <= width)
    {
        return text;
    }
    QString folder;
    QString name;
    SplitPathText(text, folder, name);
    const int nameWidth = metrics.horizontalAdvance(name);
    const int folderWidth = width - nameWidth;
    if (folder.isEmpty() == false &&
        folderWidth >= metrics.horizontalAdvance(QStringLiteral("\u2026") + folder.right(1)))
    {
        return metrics.elidedText(folder, Qt::ElideMiddle, folderWidth) + name;
    }
    if (folder.isEmpty() == false && nameWidth <= width)
    {
        return name;
    }
    return metrics.elidedText(name, Qt::ElideMiddle, width);
}

void DrawPathText(QPainter& painter,
                  const QRect& textRect,
                  const QString& text,
                  const QColor& nameColour,
                  const QColor& folderColour)
{
    const QFontMetrics metrics(painter.font());
    QString folder;
    QString name;
    SplitPathText(text, folder, name);
    const QString shown = ElidePathText(text, metrics, textRect.width());
    // What precedes the whole file name is the folder as it survived eliding.
    const bool isNameWhole = shown.endsWith(name) == true;
    const QString shownName = isNameWhole == true ? name : shown;
    const QString shownFolder = isNameWhole == true ? shown.left(shown.size() - name.size()) : QString();
    int x = textRect.left();
    const int baseline = textRect.top() + (textRect.height() - metrics.height()) / 2 + metrics.ascent();
    painter.save();
    painter.setClipRect(textRect);
    if (shownFolder.isEmpty() == false)
    {
        painter.setPen(folderColour);
        painter.drawText(QPoint(x, baseline), shownFolder);
        x += metrics.horizontalAdvance(shownFolder);
    }
    painter.setPen(nameColour);
    painter.drawText(QPoint(x, baseline), shownName);
    painter.restore();
}

QColor GetFolderColour(const QColor& colour)
{
    QColor dimmed = colour;
    dimmed.setAlpha(dimmed.alpha() * FOLDER_OPACITY_PERCENT / PERCENT);
    return dimmed;
}

void PathItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    const QString text = item.text;
    const QWidget* widget = item.widget;
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    item.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
    if (text.isEmpty() == true)
    {
        return;
    }
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget);
    const bool isSelected = item.state.testFlag(QStyle::State_Selected);
    const QPalette::ColorGroup group =
        item.state.testFlag(QStyle::State_Enabled) == true
            ? (item.state.testFlag(QStyle::State_Active) == true ? QPalette::Active : QPalette::Inactive)
            : QPalette::Disabled;
    const QColor nameColour =
        item.palette.color(group, isSelected == true ? QPalette::HighlightedText : QPalette::Text);
    painter->save();
    painter->setFont(item.font);
    DrawPathText(*painter, textRect, text, nameColour, GetFolderColour(nameColour));
    painter->restore();
}

bool PathItemDelegate::helpEvent(QHelpEvent* event,
                                 QAbstractItemView* view,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index)
{
    if (event == nullptr || view == nullptr || event->type() != QEvent::ToolTip || index.isValid() == false)
    {
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    const QString text = item.text;
    const QWidget* widget = item.widget;
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget);
    // The item's own tooltip wins; otherwise a cut text shows itself in full.
    const QString own = index.data(Qt::ToolTipRole).toString();
    if (own.isEmpty() == false)
    {
        QToolTip::showText(event->globalPos(), own, view, option.rect);
        return true;
    }
    if (QFontMetrics(item.font).horizontalAdvance(text) > textRect.width())
    {
        QToolTip::showText(event->globalPos(), text, view, option.rect);
        return true;
    }
    QToolTip::hideText();
    return true;
}
} // namespace ludus::editor

namespace ludus::editor
{
PathLabel::PathLabel(const QString& text, QWidget* parentOrNull) : QLabel(text, parentOrNull) {}

QSize PathLabel::minimumSizeHint() const
{
    const QFontMetrics metrics(font());
    return {metrics.averageCharWidth() * MINIMUM_LABEL_CHARACTERS, QLabel::sizeHint().height()};
}

QSize PathLabel::sizeHint() const
{
    return QLabel::sizeHint();
}

void PathLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setFont(font());
    const QColor nameColour =
        palette().color(isEnabled() == true ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
    DrawPathText(painter, contentsRect(), text(), nameColour, GetFolderColour(nameColour));
}

bool PathLabel::event(QEvent* event)
{
    // A cut text shows itself in full unless something more specific was set.
    if (event->type() == QEvent::ToolTip && toolTip().isEmpty() == true &&
        QFontMetrics(font()).horizontalAdvance(text()) > contentsRect().width())
    {
        QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), text(), this);
        return true;
    }
    return QLabel::event(event);
}
} // namespace ludus::editor
