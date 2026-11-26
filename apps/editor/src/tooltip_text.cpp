#include "internal/tooltip_text.h"

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QMenu>
#include <QStringList>
#include <QTextDocument>
#include <QToolTip>
#include <QWidget>

namespace ludus::editor
{
namespace
{
/// One paragraph broken greedily at spaces so that no line is wider than maximumWidth.
[[nodiscard]] QStringList WrapParagraph(const QString& paragraph, const QFontMetrics& metrics, int maximumWidth)
{
    QStringList lines;
    QString line;
    for (const QString& word : paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts))
    {
        const QString candidate = line.isEmpty() == true ? word : line + QLatin1Char(' ') + word;
        if (line.isEmpty() == false && metrics.horizontalAdvance(candidate) > maximumWidth)
        {
            lines.append(line);
            line = word;
        }
        else
        {
            line = candidate;
        }
    }
    lines.append(line);
    return lines;
}

/// The tooltip text a menu shows for the action under positionLocal, when the menu shows tooltips at all.
[[nodiscard]] QString GetMenuToolTip(const QMenu& menu, const QPoint& positionLocal)
{
    if (menu.toolTipsVisible() == false)
    {
        return {};
    }
    const QAction* actionOrNull = menu.actionAt(positionLocal);
    return actionOrNull == nullptr ? QString() : actionOrNull->toolTip();
}
} // namespace

QString WrapTooltipText(const QString& text, const QFont& font, int maximumWidth)
{
    if (text.isEmpty() == true || Qt::mightBeRichText(text) == true || maximumWidth <= 0)
    {
        return text;
    }
    const QFontMetrics metrics(font);
    QStringList lines;
    for (const QString& paragraph : text.split(QLatin1Char('\n')))
    {
        lines.append(WrapParagraph(paragraph, metrics, maximumWidth));
    }
    return lines.join(QLatin1Char('\n'));
}

void ShowEditorToolTip(const QPoint& positionGlobal, const QString& text, QWidget* widgetOrNull)
{
    QToolTip::showText(positionGlobal,
                       WrapTooltipText(text, QToolTip::font(), design::SIZE_TOOLTIP_MAXIMUM_WIDTH),
                       widgetOrNull);
}

TooltipWidthFilter::TooltipWidthFilter(QObject* parentOrNull) : QObject(parentOrNull) {}

bool TooltipWidthFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::ToolTip)
    {
        return false;
    }
    QWidget* widgetOrNull = qobject_cast<QWidget*>(watched);
    if (widgetOrNull == nullptr)
    {
        return false;
    }
    const QHelpEvent* helpEvent = static_cast<const QHelpEvent*>(event);
    const QMenu* menuOrNull = qobject_cast<const QMenu*>(widgetOrNull);
    const QString text =
        menuOrNull != nullptr ? GetMenuToolTip(*menuOrNull, helpEvent->pos()) : widgetOrNull->toolTip();
    if (text.isEmpty() == true)
    {
        return false;
    }
    const QString wrapped = WrapTooltipText(text, QToolTip::font(), design::SIZE_TOOLTIP_MAXIMUM_WIDTH);
    if (wrapped == text)
    {
        return false;
    }
    QToolTip::showText(helpEvent->globalPos(), wrapped, widgetOrNull, QRect(), widgetOrNull->toolTipDuration());
    event->accept();
    return true;
}
} // namespace ludus::editor
