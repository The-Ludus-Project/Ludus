#pragma once

#include <ludus/foundation/base/types.h>

#include <QWidget>

namespace ludus::editor
{
class CodeEditor;
class CodeDocumentController;

/// Shared editing toolbar around a CodeEditor, with the find/replace overlay
/// (CE07) floating over the view. Takes widget ownership of editor;
/// document/file lifetime stays outside.
class CodeEditorPanel final : public QWidget
{
public:
    /// @param controllerOrNull The controller bound to `editor`, so the find
    /// overlay's Replace All can publish its matches as one edit set (CE05); when
    /// null, Replace All falls back to the Qt edit-block path that still lands as
    /// one undo step.
    explicit CodeEditorPanel(CodeEditor* editor,
                             CodeDocumentController* controllerOrNull = nullptr,
                             QWidget* parentOrNull = nullptr);

private:
    CodeEditor* mEditor;
};
} // namespace ludus::editor
