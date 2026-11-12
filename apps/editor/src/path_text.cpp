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
/// A stem is kept beside its extension only when it can show this many ellipsis widths of text.
constexpr int MINIMUM_STEM_ELLIPSIS_WIDTHS = 3;
/// A path label asks for no more than this many average characters of width.
constexpr int MINIMUM_LABEL_CHARACTERS = 8;
/// A qualifier is kept beside a whole name only when it can show this many ellipsis widths of text.
constexpr int MINIMUM_QUALIFIER_ELLIPSIS_WIDTHS = 3;

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
    // Keep the extension: it says what the file is.
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
    {
        const QString extension = name.mid(dot);
        const int stemWidth = width - metrics.horizontalAdvance(extension);
        if (stemWidth >= metrics.horizontalAdvance(QStringLiteral("\u2026")) * MINIMUM_STEM_ELLIPSIS_WIDTHS)
        {
            return metrics.elidedText(name.left(dot), Qt::ElideMiddle, stemWidth) + extension;
        }
    }
    return metrics.elidedText(name, Qt::ElideMiddle, width);
}

QString RemoveKnownSuffix(const QString& fileName, QStringView knownSuffix)
{
    if (knownSuffix.isEmpty() == true || fileName.size() <= knownSuffix.size() ||
        fileName.endsWith(knownSuffix, Qt::CaseInsensitive) == false)
    {
        return fileName;
    }
    return fileName.left(fileName.size() - knownSuffix.size());
}

QString GetDocumentQualifierSeparator()
{
    return QStringLiteral(" \u00B7 ");
}

void ElideDocumentName(const QString& name,
                       const QString& qualifier,
                       const QFontMetrics& metrics,
                       int width,
                       QString& outShownName,
                       QString& outShownQualifier)
{
    const QString fullQualifier = qualifier.isEmpty() == true ? QString() : GetDocumentQualifierSeparator() + qualifier;
    const int nameWidth = metrics.horizontalAdvance(name);
    outShownQualifier.clear();
    if (nameWidth > width)
    {
        outShownName = metrics.elidedText(name, Qt::ElideMiddle, width);
        return;
    }
    outShownName = name;
    if (fullQualifier.isEmpty() == true)
    {
        return;
    }
    const int qualifierWidth = width - nameWidth;
    if (metrics.horizontalAdvance(fullQualifier) <= qualifierWidth)
    {
        outShownQualifier = fullQualifier;
        return;
    }
    const int separatorWidth = metrics.horizontalAdvance(GetDocumentQualifierSeparator());
    const int remaining = qualifierWidth - separatorWidth;
    if (remaining >= metrics.horizontalAdvance(QStringLiteral("\u2026")) * MINIMUM_QUALIFIER_ELLIPSIS_WIDTHS)
    {
        outShownQualifier = GetDocumentQualifierSeparator() + metrics.elidedText(qualifier, Qt::ElideMiddle, remaining);
    }
}

void DrawPathText(QPainter& inoutPainter,
                  const QRect& textRect,
                  const QString& text,
                  const QColor& nameColour,
                  const QColor& folderColour)
{
    const QFontMetrics metrics(inoutPainter.font());
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
    inoutPainter.save();
    inoutPainter.setClipRect(textRect);
    if (shownFolder.isEmpty() == false)
    {
        inoutPainter.setPen(folderColour);
        inoutPainter.drawText(QPoint(x, baseline), shownFolder);
        x += metrics.horizontalAdvance(shownFolder);
    }
    inoutPainter.setPen(nameColour);
    inoutPainter.drawText(QPoint(x, baseline), shownName);
    inoutPainter.restore();
}

void DrawDocumentNameText(QPainter& inoutPainter,
                          const QRect& textRect,
                          const QString& name,
                          const QString& qualifier,
                          const QColor& nameColour,
                          const QColor& qualifierColour)
{
    const QFontMetrics metrics(inoutPainter.font());
    QString shownName;
    QString shownQualifier;
    ElideDocumentName(name, qualifier, metrics, textRect.width(), shownName, shownQualifier);
    const int baseline = textRect.top() + (textRect.height() - metrics.height()) / 2 + metrics.ascent();
    inoutPainter.save();
    inoutPainter.setClipRect(textRect);
    inoutPainter.setPen(nameColour);
    inoutPainter.drawText(QPoint(textRect.left(), baseline), shownName);
    if (shownQualifier.isEmpty() == false)
    {
        inoutPainter.setPen(qualifierColour);
        inoutPainter.drawText(QPoint(textRect.left() + metrics.horizontalAdvance(shownName), baseline), shownQualifier);
    }
    inoutPainter.restore();
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
    const QVariant qualifier = index.data(DOCUMENT_QUALIFIER_ROLE);
    if (qualifier.isValid() == true)
    {
        // Selected rows dim the highlighted text; others use the palette's quiet text, which the editor palette
        // takes from the design tokens and their high-contrast values.
        const QColor qualifierColour = isSelected == true || group == QPalette::Disabled
                                           ? GetFolderColour(nameColour)
                                           : item.palette.color(group, QPalette::PlaceholderText);
        DrawDocumentNameText(*painter, textRect, text, qualifier.toString(), nameColour, qualifierColour);
    }
    else
    {
        DrawPathText(*painter, textRect, text, nameColour, GetFolderColour(nameColour));
    }
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
