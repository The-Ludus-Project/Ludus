#pragma once

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QList>
#include <QString>

namespace ludus::editor
{
/// The negotiated LSP position encoding (LSP 3.17 `PositionEncodingKind`). A
/// position's `character` counts UTF-16 code units or UTF-8 bytes within its line.
enum class LanguagePositionEncoding : foundation::uint8
{
    Utf16,
    Utf8,
};

/// The protocol name of `encoding` ("utf-16" or "utf-8").
[[nodiscard]] QString GetLanguagePositionEncodingName(LanguagePositionEncoding encoding);

/// Reads a server's `positionEncoding` choice. An absent value means UTF-16, as
/// the protocol defines; any other name than "utf-16" or "utf-8" fails and
/// preserves `outEncoding`.
[[nodiscard]] bool ReadLanguagePositionEncoding(const QString& name, LanguagePositionEncoding& outEncoding);

/// The zero-based protocol position of the UTF-16 offset `offset` in `text`.
/// Lines are separated by '\n'. `offset` must lie in [0, text.size()].
[[nodiscard]] QJsonObject
MakeLanguagePosition(const QString& text, foundation::int32 offset, LanguagePositionEncoding encoding);

/// The UTF-16 offset in `text` of a zero-based protocol position. Fails, and
/// preserves `outOffset`, when the line or character is out of range or does not
/// fall on a code point boundary in the negotiated encoding.
[[nodiscard]] bool GetLanguageOffset(const QString& text,
                                     const QJsonObject& position,
                                     LanguagePositionEncoding encoding,
                                     foundation::int32& outOffset);

/// The start offsets of every line in `text`, for resolving many positions in one
/// text with the overload below.
[[nodiscard]] QList<foundation::int32> IndexLanguageLines(const QString& text);

/// GetLanguageOffset with `lineStarts` from IndexLanguageLines(text).
[[nodiscard]] bool GetLanguageOffset(const QString& text,
                                     const QList<foundation::int32>& lineStarts,
                                     const QJsonObject& position,
                                     LanguagePositionEncoding encoding,
                                     foundation::int32& outOffset);
/// GetLanguageOffset for a zero-based `line` and `character` given as numbers, as
/// semantic tokens encode them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): line then character is the protocol's order.
[[nodiscard]] bool GetLanguageOffset(const QString& text,
                                     const QList<foundation::int32>& lineStarts,
                                     foundation::int32 line,
                                     foundation::int32 character,
                                     LanguagePositionEncoding encoding,
                                     foundation::int32& outOffset);
} // namespace ludus::editor
