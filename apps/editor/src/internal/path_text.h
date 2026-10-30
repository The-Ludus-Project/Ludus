#pragma once

// Path text that stays readable when its field is narrow: PathItemDelegate for item views and
// PathLabel for single lines.
//
// The folder part is dimmed and elided from the middle first, so the file name stays whole and
// visible; only when the file name alone does not fit is it elided, also from the middle. Any item
// whose text has no folder part, and every item that is not elided, is drawn like the default
// delegate. A tooltip with the full text appears for any item whose text is cut, so nothing has
// to be dragged wider to be read. See docs/development/editor-workspace.md (Truncation).

#include <QColor>
#include <QLabel>
#include <QString>
#include <QStyledItemDelegate>

namespace ludus::editor
{
/// Split text at the last `/` (or `\`) into a folder part, including its separator, and a name.
/// A text with no separator has an empty folder.
void SplitPathText(const QString& text, QString& outFolder, QString& outName);

/// Folder-dimmed, middle-eliding path text. GUI thread only.
class PathItemDelegate final : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit PathItemDelegate(QObject* parentOrNull = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event,
                   QAbstractItemView* view,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;
};

/// The text as drawn in a field of width pixels: whole when it fits, else the folder is elided
/// from the middle and the name kept, else the name is elided from the middle.
[[nodiscard]] QString ElidePathText(const QString& text, const QFontMetrics& metrics, int width);

/// The dimmed colour a folder part takes beside a file name drawn in nameColour.
[[nodiscard]] QColor GetFolderColour(const QColor& nameColour);

/// Draw text in textRect with its folder dimmed and elided as ElidePathText says.
void DrawPathText(QPainter& inoutPainter,
                  const QRect& textRect,
                  const QString& text,
                  const QColor& nameColour,
                  const QColor& folderColour);

/// One line of path text: elided from the middle with the file name kept, dimmed folder, and the full
/// text as its tooltip whenever it is cut (an explicit tooltip wins). It never asks for more width than
/// a short minimum, so a long path cannot push the controls beside it. GUI thread only.
class PathLabel final : public QLabel
{
    Q_OBJECT
public:
    explicit PathLabel(const QString& text, QWidget* parentOrNull = nullptr);

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
};
} // namespace ludus::editor
