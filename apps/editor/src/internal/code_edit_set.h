#pragma once

#include "internal/code_document.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QList>

namespace ludus::editor
{
/// One document state recorded in the undo history: the full UTF-8 draft plus
/// the selection set that was active in that state (CE05). The selection set is
/// kept so one undo step restores both the text and every cursor, as the seam
/// requires. Equality compares the TEXT ONLY, so a selection-only change never
/// becomes an undo step: DocumentHistory::Commit treats equal states as a no-op,
/// and a bare caret move must not push history.
struct CodeHistoryState final
{
    QByteArray Text;
    QList<CodeTextSelection> Selections;

    [[nodiscard]] bool operator==(const CodeHistoryState& other) const noexcept
    {
        return Text == other.Text;
    }
};

/// Copy a borrowed selection set into an owned list. An empty source yields an
/// empty list; the caller then leaves the view's current selection untouched.
[[nodiscard]] QList<CodeTextSelection> CopyCodeSelectionSet(const CodeSelectionSet& selections);

/// Validate a whole edit set against `original` without changing anything, and
/// on Ok write the resulting bytes to outResult (CE05). The checks mirror the
/// single-edit validation and add the two the set adds:
///   - every edit's StartBytes/RemovedBytes stay within the running buffer,
///   - the set is strictly ascending with no overlap (edit i+1 starts at or
///     after edit i's removed end),
///   - every edit's StartBytes, removed end and inserted bytes land on UTF-8
///     scalar boundaries,
///   - the cumulative result stays within limitBytes (computed without ever
///     building an oversized buffer),
/// and either the whole set applies or none of it does. outResult is only
/// written when the status is Ok; on any failure it is left unchanged.
[[nodiscard]] CodeEditStatus ValidateAndApplyEditSet(foundation::BorrowedView<const char> original,
                                                     foundation::usize limitBytes,
                                                     const CodeEditSet& edits,
                                                     QByteArray& outResult);
} // namespace ludus::editor
