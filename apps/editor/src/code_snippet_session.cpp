#include "internal/code_snippet_session.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_editor.h"
#include "internal/code_multi_cursor.h"

#include <QMap>
#include <QTextDocument>

namespace ludus::editor
{
using namespace foundation;

CodeSnippetSession::CodeSnippetSession(CodeEditor& inoutEditor) : QObject(&inoutEditor), mEditor(inoutEditor)
{
    connect(&mEditor, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (IsActive() == false || mIsSelecting == true)
        {
            return;
        }
        // Leaving the inserted text ends the session, as in VS Code.
        const int32 position = mEditor.textCursor().position();
        if (position < mOuterStart.position() || position > mOuterEnd.position())
        {
            End();
        }
    });
}

CodeSnippetSession::Range CodeSnippetSession::makeRange(int32 start, int32 end) const
{
    // Text typed at a stop's start or end extends that stop.
    Range range{ .Start = QTextCursor(mEditor.document()), .End = QTextCursor(mEditor.document()) };
    range.Start.setPosition(start);
    range.Start.setKeepPositionOnInsert(true);
    range.End.setPosition(end);
    return range;
}

bool CodeSnippetSession::Start(int32 start, const CodeSnippet& snippet)
{
    End();
    const int32 end = start + static_cast<int32>(snippet.Text.size());
    // The text must already be in the document; a snippet that does not fit starts nothing.
    if (start < 0 || end > mEditor.document()->characterCount() - 1)
    {
        return false;
    }
    QMap<int32, Group> groups;
    int32 finalPosition = end;
    bool hasFinal = false;
    for (const CodeSnippetStop& stop : snippet.Stops)
    {
        if (stop.Index == 0)
        {
            finalPosition = hasFinal == true ? finalPosition : start + stop.Start;
            hasFinal = true;
            continue;
        }
        Group& group = groups[stop.Index];
        group.Index = stop.Index;
        group.Ranges.append(makeRange(start + stop.Start, start + stop.End));
    }
    mFinal = QTextCursor(mEditor.document());
    mFinal.setPosition(finalPosition);
    if (groups.isEmpty() == true)
    {
        mEditor.setTextCursor(mFinal);
        return false;
    }
    for (Group& group : groups)
    {
        // The first occurrence of a stop is primary; the rest follow it.
        Group ordered;
        ordered.Index = group.Index;
        for (const Range& range : group.Ranges)
        {
            qsizetype index = 0;
            while (index < ordered.Ranges.size() && ordered.Ranges.at(index).Start.position() <= range.Start.position())
            {
                ++index;
            }
            ordered.Ranges.insert(index, range);
        }
        mGroups.append(ordered);
    }
    mOuterStart = QTextCursor(mEditor.document());
    mOuterStart.setPosition(start);
    mOuterStart.setKeepPositionOnInsert(true);
    mOuterEnd = QTextCursor(mEditor.document());
    mOuterEnd.setPosition(end);
    select(0);
    return true;
}

bool CodeSnippetSession::IsActive() const noexcept
{
    return mCurrent >= 0;
}

bool CodeSnippetSession::Next()
{
    if (IsActive() == false)
    {
        return false;
    }
    if (mCurrent + 1 >= mGroups.size())
    {
        finish();
        return true;
    }
    select(mCurrent + 1);
    return true;
}

bool CodeSnippetSession::Previous()
{
    if (IsActive() == false)
    {
        return false;
    }
    select(qMax(0, mCurrent - 1));
    return true;
}

void CodeSnippetSession::End()
{
    const bool wasActive = IsActive();
    mGroups.clear();
    mCurrent = -1;
    mOuterStart = {};
    mOuterEnd = {};
    if (wasActive == true)
    {
        mEditor.viewport()->update();
    }
}

QList<QTextCursor> CodeSnippetSession::GetRanges() const
{
    QList<QTextCursor> ranges;
    for (const Group& group : mGroups)
    {
        for (const Range& range : group.Ranges)
        {
            QTextCursor cursor(mEditor.document());
            cursor.setPosition(range.Start.position());
            cursor.setPosition(qMax(range.Start.position(), range.End.position()), QTextCursor::KeepAnchor);
            ranges.append(cursor);
        }
    }
    return ranges;
}

void CodeSnippetSession::select(int32 group)
{
    mIsSelecting = true;
    mCurrent = group;
    const QList<Range>& ranges = mGroups.at(group).Ranges;
    QList<CodeSecondaryCursor> mirrors;
    for (qsizetype index = 1; index < ranges.size(); ++index)
    {
        mirrors.append({ .Anchor = ranges.at(index).Start.position(), .Caret = ranges.at(index).End.position() });
    }
    QTextCursor primary(mEditor.document());
    primary.setPosition(ranges.constFirst().Start.position());
    primary.setPosition(qMax(ranges.constFirst().Start.position(), ranges.constFirst().End.position()),
                        QTextCursor::KeepAnchor);
    mEditor.RevealTextCursor(primary);
    mEditor.setTextCursor(primary);
    mEditor.GetMultiCursor().AdoptSecondaryCursors(mirrors);
    mEditor.ensureCursorVisible();
    mEditor.viewport()->update();
    mIsSelecting = false;
}

void CodeSnippetSession::finish()
{
    mIsSelecting = true;
    static_cast<void>(mEditor.GetMultiCursor().Collapse());
    QTextCursor caret(mEditor.document());
    caret.setPosition(mFinal.position());
    mEditor.setTextCursor(caret);
    mIsSelecting = false;
    End();
}
} // namespace ludus::editor
