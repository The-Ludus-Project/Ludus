#pragma once

#include <ludus/foundation/base/compiler.h>

#include "internal/code_editor.h"

namespace ludus::editor
{
/// Completes a view operation after Qt has finished applying its text changes.
class CodeEditScope final
{
public:
    LUDUS_INLINE explicit CodeEditScope(CodeEditor& inoutEditor, CodeEditKind kind = CodeEditKind::Command)
        : mEditor(inoutEditor)
    {
        mEditor.BeginEditOperation(kind);
    }
    LUDUS_INLINE ~CodeEditScope()
    {
        mEditor.EndEditOperation();
    }
    CodeEditScope(const CodeEditScope&) = delete;
    CodeEditScope& operator=(const CodeEditScope&) = delete;

private:
    CodeEditor& mEditor;
};
} // namespace ludus::editor
