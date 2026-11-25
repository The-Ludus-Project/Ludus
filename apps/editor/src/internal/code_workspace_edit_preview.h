#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_workspace_edit.h"

#include <QDialog>
#include <QList>
#include <QString>

class QTreeWidget;

namespace ludus::editor
{
/// The preview of a workspace edit (code editor M4, CE11): every affected file
/// with each change's line, column and replaced text, then Apply or Cancel. It
/// changes nothing itself; the caller applies on `accepted`. GUI thread only.
class CodeWorkspaceEditPreview final : public QDialog
{
    Q_OBJECT
public:
    /// Rows past this count per file are summarized.
    static constexpr foundation::int32 MAX_ROWS_PER_FILE = 200;
    /// `summary` heads the list, such as "Rename 'a' to 'b'". `sourceName` labels
    /// the change for the edited source, whose File is empty.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the heading, then the edited source's label.
    CodeWorkspaceEditPreview(const QString& summary,
                             const QString& sourceName,
                             const QList<CodeWorkspaceFileChange>& changes,
                             QWidget* parentOrNull = nullptr);
    /// The number of edits listed across files.
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetEditCount() const noexcept
    {
        return mEditCount;
    }

private:
    QTreeWidget* mFiles;
    foundation::int32 mEditCount = 0;
};
} // namespace ludus::editor
