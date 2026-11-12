#pragma once

// Path text that stays readable when its field is narrow: PathItemDelegate for item views and
// PathLabel for single lines.
//
// The folder part is dimmed and elided from the middle first, so the file name stays whole and
// visible; only when the file name alone does not fit is it elided, also from the middle. Any item
// whose text has no folder part, and every item that is not elided, is drawn like the default
// delegate. A tooltip with the full text appears for any item whose text is cut, so nothing has
// to be dragged wider to be read. See docs/development/editor-workspace.md (Truncation).
//
// An item that sets DOCUMENT_QUALIFIER_ROLE is a document name instead (a file name without the suffix its
// group already names): the name is drawn whole or elided from the middle, never split at a dot, and a
// non-empty qualifier follows it dimmed as " · qualifier", cut before the name is.

#include <QColor>
#include <QLabel>
#include <QString>
#include <QStringView>
#include <QStyledItemDelegate>

namespace ludus::editor
{
/// Item data role that marks a document name and holds its dimmed qualifier (empty for none). Items that do
/// not set it are drawn as path text.
inline constexpr int DOCUMENT_QUALIFIER_ROLE = Qt::UserRole + 64;

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

/// fileName without knownSuffix when it ends with it (case-insensitively) and something is left; otherwise
/// fileName unchanged. "sponza.world.json" with ".world.json" is "sponza".
[[nodiscard]] QString RemoveKnownSuffix(const QString& fileName, QStringView knownSuffix);

/// A document name and its qualifier as drawn in a field of width pixels. Both are whole when they fit;
/// otherwise the qualifier is elided from the middle, then dropped, and only then is the name elided from
/// the middle, so its head and tail stay ("SliceSampl…Coupled"). A shown qualifier starts with
/// GetDocumentQualifierSeparator(); outShownQualifier is empty when there is none or no room for it.
void ElideDocumentName(const QString& name,
                       const QString& qualifier,
                       const QFontMetrics& metrics,
                       int width,
                       QString& outShownName,
                       QString& outShownQualifier);

/// The text that separates a document name from its qualifier.
[[nodiscard]] QString GetDocumentQualifierSeparator();

/// The dimmed colour a folder part takes beside a file name drawn in nameColour.
[[nodiscard]] QColor GetFolderColour(const QColor& nameColour);

/// Draw text in textRect with its folder dimmed and elided as ElidePathText says.
void DrawPathText(QPainter& inoutPainter,
                  const QRect& textRect,
                  const QString& text,
                  const QColor& nameColour,
                  const QColor& folderColour);

/// Draw a document name in textRect with its qualifier dimmed after it, elided as ElideDocumentName says.
void DrawDocumentNameText(QPainter& inoutPainter,
                          const QRect& textRect,
                          const QString& name,
                          const QString& qualifier,
                          const QColor& nameColour,
                          const QColor& qualifierColour);

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
