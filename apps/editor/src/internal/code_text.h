#pragma once

#include "internal/code_document.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
/// Decode valid UTF-8 for Qt, normalizing CRLF/CR paragraph boundaries to LF.
/// Failure preserves output; no other Unicode normalization is performed.
[[nodiscard]] bool DecodeCodeText(const QByteArray& bytes, QString& outText);
/// Map between normalized Qt UTF-16 boundaries and original UTF-8 bytes.
/// @pre bytes contains validated UTF-8.
/// Invalid/interior scalar or CRLF positions are rejected; output survives failure.
/// Walks from the start of bytes; to map many ascending positions, use
/// AdvanceCodeByteOffset so the whole walk stays linear.
[[nodiscard]] bool
GetCodeByteOffset(const QByteArray& bytes, foundation::int32 position, foundation::usize& outOffset) noexcept;
/// A matching pair of coordinates in one text: a normalized Qt UTF-16 position
/// and the original UTF-8 byte offset of the same boundary. The default is the
/// start of the text.
struct CodePositionOffset final
{
    foundation::int32 Position = 0;
    foundation::usize OffsetBytes = 0;
};
/// Map `position` to its byte offset by walking forward from `inoutFrom`, a pair
/// previously produced for the same bytes, instead of from the start. Mapping
/// ascending positions this way (every match of Replace All, every cursor of a
/// selection set) costs one pass over the text in total.
/// @pre bytes contains validated UTF-8 and inoutFrom is a boundary of it.
/// On success, inoutFrom becomes {position, outOffset}. A position before
/// inoutFrom, or inside a scalar or CRLF, is rejected and both outputs survive.
[[nodiscard]] bool AdvanceCodeByteOffset(const QByteArray& bytes,
                                         foundation::int32 position,
                                         CodePositionOffset& inoutFrom,
                                         foundation::usize& outOffset) noexcept;
[[nodiscard]] bool
GetCodeCursorPosition(const QByteArray& bytes, foundation::usize offset, foundation::int32& outPosition) noexcept;
/// Map `offset` to its Qt UTF-16 position by walking forward from `inoutFrom`, a
/// pair previously produced for the same bytes, instead of from the start. This
/// is the byte-to-position mirror of AdvanceCodeByteOffset: mapping ascending
/// offsets this way (every cursor of a selection set the controller restores)
/// costs one pass over the text in total, never one walk per cursor.
/// @pre bytes contains validated UTF-8 and inoutFrom is a boundary of it.
/// On success, inoutFrom becomes {outPosition, offset}. An offset before
/// inoutFrom, or inside a scalar or CRLF, is rejected and both outputs survive.
[[nodiscard]] bool AdvanceCodeCursorPosition(const QByteArray& bytes,
                                             foundation::usize offset,
                                             CodePositionOffset& inoutFrom,
                                             foundation::int32& outPosition) noexcept;
/// Constant-time scalar-boundary check for already validated source bytes.
/// CRLF's interior is a scalar boundary, although it is not a Qt cursor boundary.
[[nodiscard]] bool IsCodeScalarBoundary(const QByteArray& bytes, foundation::usize offset) noexcept;
/// IsCodeScalarBoundary over borrowed bytes; an invalid view has no boundaries.
[[nodiscard]] bool IsCodeScalarBoundary(foundation::BorrowedView<const char> bytes, foundation::usize offset) noexcept;
/// Minimal scalar-aligned replacement between two valid UTF-8 snapshots.
/// Inserted borrows after until its next mutation. No allocation.
[[nodiscard]] CodeTextEdit MakeCodeTextEdit(const QByteArray& before, const QByteArray& after) noexcept;
} // namespace ludus::editor
