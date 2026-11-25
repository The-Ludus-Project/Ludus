#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_snippet.h"

#include <QList>
#include <QObject>
#include <QTextCursor>

namespace ludus::editor
{
class CodeEditor;

/// The tab stops of one inserted snippet in one view (code editor M4, CE10).
/// Tab and Shift+Tab select the next and previous stop; a stop's other
/// occurrences (the same index) are secondary cursors, so typing edits them
/// together. Reaching `$0` (or the snippet's end), Escape, or moving the caret
/// out of the snippet ends the session. Ranges are anchored in the document, so
/// edits move them. Never edits text itself. GUI thread only; the editor owns it.
class CodeSnippetSession final : public QObject
{
public:
    explicit CodeSnippetSession(CodeEditor& inoutEditor);
    /// Begin a session for `snippet`, whose Text was just inserted at document
    /// position `start`, and select its first stop. Without stops to visit the
    /// caret goes to `$0` or the end of the text and no session starts; returns
    /// whether one did. A snippet that does not fit the document starts nothing.
    bool Start(foundation::int32 start, const CodeSnippet& snippet);
    [[nodiscard]] bool IsActive() const noexcept;
    /// Select the next stop, or end at the final position. False when inactive.
    bool Next();
    /// Select the previous stop, staying on the first. False when inactive.
    bool Previous();
    void End();
    /// The current ranges of every stop still to visit or revisit (not `$0`), for
    /// the view's placeholder backgrounds.
    [[nodiscard]] QList<QTextCursor> GetRanges() const;

private:
    struct Range final
    {
        QTextCursor Start;
        QTextCursor End;
    };
    struct Group final
    {
        foundation::int32 Index = 0;
        QList<Range> Ranges;
    };
    void select(foundation::int32 group);
    void finish();
    [[nodiscard]] Range makeRange(foundation::int32 start, foundation::int32 end) const;

    CodeEditor& mEditor;
    QList<Group> mGroups;
    /// The final caret position: `$0`, or the end of the inserted text.
    QTextCursor mFinal;
    QTextCursor mOuterStart;
    QTextCursor mOuterEnd;
    foundation::int32 mCurrent = -1;
    bool mIsSelecting = false;
};
} // namespace ludus::editor
