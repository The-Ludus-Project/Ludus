#pragma once

#include <QString>

namespace ludus::editor
{
class CodeDocumentController;

/// @brief Format the bound LUSL document in process with the LUSL formatter
/// (docs/modules/tooling/shader-language.md#formatting), as one undoable edit.
///
/// The formatter changes only whitespace, so the caret keeps its byte offset,
/// clamped to the new text. A source that does not lex, or whose layout is
/// already canonical, is left untouched. GUI thread only.
/// @return The status-bar message describing the outcome.
[[nodiscard]] QString FormatLuslDocument(CodeDocumentController& inoutDocument);
} // namespace ludus::editor
