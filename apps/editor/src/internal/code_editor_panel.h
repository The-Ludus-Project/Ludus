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

/// Shared find/replace and go-to-line presentation around a CodeEditor.
/// Takes widget ownership of editor; document/file lifetime stays outside.
class CodeEditorPanel final : public QWidget
{
public:
    explicit CodeEditorPanel(CodeEditor* editor, QWidget* parentOrNull = nullptr);

private:
    void showSearch(bool shouldReplace);
    void findText(bool shouldSearchBackward);
    void replaceText();
    void replaceAll();
    CodeEditor* mEditor;
    QWidget* mSearch;
    QWidget* mReplaceRow;
    QLineEdit* mFindText;
    QLineEdit* mReplacement;
    QCheckBox* mIsCaseSensitive;
    QLabel* mStatus;
};
} // namespace ludus::editor
