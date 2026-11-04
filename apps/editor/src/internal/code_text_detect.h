#pragma once

#include "internal/code_text_profile.h"

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Detect the encoding, byte-order mark, line endings and indentation of a
/// loaded byte buffer and classify its size mode (CE04, CE22). Pure over the
/// bytes: no Qt, no file I/O, no allocation of the input. Runs on the background
/// pool before the host is created, so typing never waits on it.
///
/// Invalid UTF-8 sets Encoding to InvalidUtf8 and IsReadOnly to true; the caller
/// opens the document read-only with an explanation. A UTF-8 BOM is detected and
/// recorded but is NOT part of the editable text the host stores: the caller
/// strips the three BOM bytes from the stored buffer and re-emits them on save,
/// so the BOM is preserved byte for byte while interior offsets stay BOM-free.
///
/// @param bytes The full file contents; may be null only when byteCount is zero.
/// @param byteCount Number of bytes in `bytes`.
/// @param outProfile Receives the detected profile; overwritten wholesale.
/// @return The number of leading bytes that are a BOM (0 or 3), so the caller
///   knows how many bytes to drop from the stored buffer.
[[nodiscard]] foundation::usize
DetectCodeTextProfile(const char* bytes, foundation::usize byteCount, CodeTextProfile& outProfile) noexcept;

/// The three bytes of the UTF-8 byte-order mark, so the save path can re-emit it.
inline constexpr char CODE_UTF8_BOM_0 = static_cast<char>(0xEF);
inline constexpr char CODE_UTF8_BOM_1 = static_cast<char>(0xBB);
inline constexpr char CODE_UTF8_BOM_2 = static_cast<char>(0xBF);
inline constexpr foundation::usize CODE_UTF8_BOM_LENGTH = 3;
} // namespace ludus::editor
