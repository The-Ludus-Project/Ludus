#include "internal/code_edit_set.h"

#include "internal/code_text.h"

#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <string_view>
#include <utility>

// Whole-set validation and application for the edit-set seam (CE05). Qt types
// are used for the owned result buffer and selection list only; the validation
// itself works over BorrowedView<const char> and never touches a widget.
//
// Cost is linear in the original plus the inserted bytes, whatever the number
// of edits or selections: hosts hold validated UTF-8 (checked on bind and
// admission), so a scalar boundary is a constant-time check on one byte, and
// only the inserted bytes are validated. Validating the prefix up to every
// offset instead made Replace All and multi-cursor edits quadratic.

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::isize;
using foundation::StringStatus;
using foundation::usize;
using foundation::Utf8View;
using foundation::ValidateUtf8;

namespace
{
[[nodiscard]] bool IsValidUtf8Bytes(BorrowedView<const char> bytes) noexcept
{
    if (bytes.IsValid() == false)
    {
        return false;
    }
    if (bytes.IsEmpty() == true)
    {
        return true;
    }
    Utf8View proof;
    usize errorOffset = 0;
    return ValidateUtf8({bytes.GetDataOrNull(), bytes.GetSize()}, proof, errorOffset) == StringStatus::Ok;
}

[[nodiscard]] bool areSelectionsWithin(const CodeSelectionSet& selections, usize size, BorrowedView<const char> bytes)
{
    const BorrowedView<const CodeTextSelection> view = selections.Selections;
    if (view.IsValid() == false)
    {
        return false;
    }
    for (usize index = 0; index < view.GetSize(); ++index)
    {
        const CodeTextSelection selection = view[index];
        if (selection.AnchorBytes > size || selection.CaretBytes > size ||
            IsCodeScalarBoundary(bytes, selection.AnchorBytes) == false ||
            IsCodeScalarBoundary(bytes, selection.CaretBytes) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace

QList<CodeTextSelection> CopyCodeSelectionSet(const CodeSelectionSet& selections)
{
    QList<CodeTextSelection> copy;
    const BorrowedView<const CodeTextSelection> view = selections.Selections;
    if (view.IsValid() == false)
    {
        return copy;
    }
    copy.reserve(static_cast<isize>(view.GetSize()));
    for (usize index = 0; index < view.GetSize(); ++index)
    {
        copy.append(view[index]);
    }
    return copy;
}

CodeEditStatus ValidateAndApplyEditSet(BorrowedView<const char> original,
                                       usize limitBytes,
                                       const CodeEditSet& edits,
                                       QByteArray& outResult)
{
    const BorrowedView<const CodeTextEdit> view = edits.Edits;
    if (view.IsValid() == false)
    {
        return CodeEditStatus::InvalidText;
    }
    const usize originalSize = original.GetSize();

    // Pass one: order, overlap, ranges, UTF-8 boundaries and running capacity.
    // Nothing is mutated until every edit in the set has been accepted.
    usize previousEnd = 0;
    usize resultSize = originalSize;
    bool isFirst = true;
    for (usize index = 0; index < view.GetSize(); ++index)
    {
        const CodeTextEdit edit = view[index];
        if (edit.Inserted.IsValid() == false || edit.StartBytes > originalSize ||
            edit.RemovedBytes > originalSize - edit.StartBytes)
        {
            return CodeEditStatus::InvalidText;
        }
        // Ascending with no overlap: each edit begins at or after the removed end
        // of the previous edit, against the ORIGINAL buffer's offsets.
        if (isFirst == false && edit.StartBytes < previousEnd)
        {
            return CodeEditStatus::Conflict;
        }
        isFirst = false;
        previousEnd = edit.StartBytes + edit.RemovedBytes;
        if (IsCodeScalarBoundary(original, edit.StartBytes) == false ||
            IsCodeScalarBoundary(original, edit.StartBytes + edit.RemovedBytes) == false ||
            IsValidUtf8Bytes(edit.Inserted) == false)
        {
            return CodeEditStatus::InvalidText;
        }
        // Capacity across the whole set, without ever forming an oversized buffer.
        // resultSize cannot underflow: RemovedBytes was bounded above.
        resultSize -= edit.RemovedBytes;
        if (resultSize > limitBytes || edit.Inserted.GetSize() > limitBytes - resultSize)
        {
            return CodeEditStatus::Capacity;
        }
        resultSize += edit.Inserted.GetSize();
    }

    // Before selections are mapped against the original buffer; After against the
    // result. Both must land on scalar boundaries, like the single-edit path.
    if (areSelectionsWithin(edits.Before, originalSize, original) == false)
    {
        return CodeEditStatus::InvalidText;
    }

    // Pass two: build the result. Copy the spans between edits verbatim and
    // splice each edit's inserted bytes, walking the original once.
    QByteArray candidate;
    candidate.reserve(static_cast<isize>(resultSize));
    usize cursor = 0;
    for (usize index = 0; index < view.GetSize(); ++index)
    {
        const CodeTextEdit edit = view[index];
        candidate.append(original.GetDataOrNull() + cursor, static_cast<isize>(edit.StartBytes - cursor));
        if (edit.Inserted.IsEmpty() == false)
        {
            candidate.append(edit.Inserted.GetDataOrNull(), static_cast<isize>(edit.Inserted.GetSize()));
        }
        cursor = edit.StartBytes + edit.RemovedBytes;
    }
    candidate.append(original.GetDataOrNull() + cursor, static_cast<isize>(originalSize - cursor));

    const BorrowedView<const char> result{candidate.constData(), static_cast<usize>(candidate.size())};
    if (areSelectionsWithin(edits.After, static_cast<usize>(candidate.size()), result) == false)
    {
        return CodeEditStatus::InvalidText;
    }
    outResult = std::move(candidate);
    return CodeEditStatus::Ok;
}
} // namespace ludus::editor
