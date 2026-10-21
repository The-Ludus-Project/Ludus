#pragma once

#include "internal/code_document.h"

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
[[nodiscard]] bool
GetCodeByteOffset(const QByteArray& bytes, foundation::int32 position, foundation::usize& outOffset) noexcept;
[[nodiscard]] bool
GetCodeCursorPosition(const QByteArray& bytes, foundation::usize offset, foundation::int32& outPosition) noexcept;
/// Constant-time scalar-boundary check for already validated source bytes.
/// CRLF's interior is a scalar boundary, although it is not a Qt cursor boundary.
[[nodiscard]] bool IsCodeScalarBoundary(const QByteArray& bytes, foundation::usize offset) noexcept;
/// Minimal scalar-aligned replacement between two valid UTF-8 snapshots.
/// Inserted borrows after until its next mutation. No allocation.
[[nodiscard]] CodeTextEdit MakeCodeTextEdit(const QByteArray& before, const QByteArray& after) noexcept;
} // namespace ludus::editor
