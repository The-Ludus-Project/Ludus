#include "internal/code_workspace_edit_preview.h"

#include <ludus/foundation/base/core.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QStringView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
// Replaced and inserted text longer than this is shortened in a row.
constexpr qsizetype MAX_SHOWN_UNITS = 60;

QString Shorten(QStringView text)
{
    const QString line = text.toString().replace(QLatin1Char('\n'), QStringLiteral("⏎"));
    return line.size() > MAX_SHOWN_UNITS ? line.left(MAX_SHOWN_UNITS - 1) + QStringLiteral("…") : line;
}
} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the heading, then the edited source's label.
CodeWorkspaceEditPreview::CodeWorkspaceEditPreview(const QString& summary,
                                                   const QString& sourceName,
                                                   const QList<CodeWorkspaceFileChange>& changes,
                                                   QWidget* parentOrNull)
    : QDialog(parentOrNull), mFiles(new QTreeWidget(this))
{
    setObjectName(QStringLiteral("codeWorkspaceEditPreview"));
    setWindowTitle(QStringLiteral("Preview Changes"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* heading = new QLabel(this);
    heading->setObjectName(QStringLiteral("codeWorkspaceEditSummary"));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    mFiles->setObjectName(QStringLiteral("codeWorkspaceEditFiles"));
    mFiles->setHeaderHidden(true);
    mFiles->setAccessibleName(QStringLiteral("Changes by file"));
    layout->addWidget(mFiles, 1);
    for (const CodeWorkspaceFileChange& change : changes)
    {
        const QString name = change.File.isEmpty() == true ? sourceName : change.File;
        QTreeWidgetItem* file =
            new QTreeWidgetItem(mFiles, {QStringLiteral("%1 (%2)").arg(name).arg(change.Edits.size())});
        file->setExpanded(true);
        const QStringView text(change.Text);
        int32 rows = 0;
        for (const CodeWorkspaceTextEdit& edit : change.Edits)
        {
            ++mEditCount;
            if (++rows > MAX_ROWS_PER_FILE)
            {
                continue;
            }
            const int32 lineStart = static_cast<int32>(text.first(edit.Start).lastIndexOf(QLatin1Char('\n'))) + 1;
            const int32 line = static_cast<int32>(text.first(edit.Start).count(QLatin1Char('\n'))) + 1;
            qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), edit.Start);
            lineEnd = lineEnd < 0 ? text.size() : lineEnd;
            QTreeWidgetItem* row = new QTreeWidgetItem(
                file,
                {QStringLiteral("line %1, column %2: '%3' → '%4'")
                     .arg(line)
                     .arg(edit.Start - lineStart + 1)
                     .arg(Shorten(text.sliced(edit.Start, edit.End - edit.Start)), Shorten(edit.NewText))});
            row->setToolTip(0, text.sliced(lineStart, lineEnd - lineStart).trimmed().toString());
        }
        if (rows > MAX_ROWS_PER_FILE)
        {
            static_cast<void>(
                new QTreeWidgetItem(file, {QStringLiteral("… and %1 more").arg(rows - MAX_ROWS_PER_FILE)}));
        }
    }
    heading->setText(QStringLiteral("%1: %2 changes in %3 files. Nothing is saved; Undo reverts every file at once.")
                         .arg(summary)
                         .arg(mEditCount)
                         .arg(changes.size()));
    QDialogButtonBox* buttons = new QDialogButtonBox(this);
    QPushButton* apply = buttons->addButton(QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
    apply->setObjectName(QStringLiteral("codeWorkspaceEditApply"));
    apply->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
} // namespace ludus::editor
