#pragma once

#include <ludus/foundation/base/types.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeEditor;
class CodeDocumentController;

/// Shared find/replace and go-to-line presentation around a CodeEditor.
/// Takes widget ownership of editor; document/file lifetime stays outside.
class CodeEditorPanel final : public QWidget
{
public:
    /// @param controllerOrNull The controller bound to `editor`, so Replace All
    /// can publish its matches as one edit set (CE05); when null, Replace All
    /// falls back to the Qt edit-block path that still lands as one undo step.
    explicit CodeEditorPanel(CodeEditor* editor,
                             CodeDocumentController* controllerOrNull = nullptr,
                             QWidget* parentOrNull = nullptr);

private:
    void showSearch(bool shouldReplace);
    void findText(bool shouldSearchBackward);
    void replaceText();
    void replaceAll();
    [[nodiscard]] bool
    replaceAllThroughEditSet(const QString& findText, const QString& replacement, bool isCaseSensitive);
    CodeEditor* mEditor;
    CodeDocumentController* mControllerOrNull = nullptr;
    QWidget* mSearch;
    QWidget* mReplaceRow;
    QLineEdit* mFindText;
    QLineEdit* mReplacement;
    QCheckBox* mIsCaseSensitive;
    QLabel* mStatus;
};
} // namespace ludus::editor
