#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_editor.h"

#include <QPalette>
#include <QSyntaxHighlighter>
#include <QTextBlockUserData>
#include <QTextCharFormat>
#include <QVector>

namespace ludus::editor
{
struct CodeBracket final
{
    foundation::int32 Position = 0;
    QChar Character;
};

/// Only lexical brackets outside comments and strings, cached per text block.
struct CodeBlockData final : QTextBlockUserData
{
    QVector<CodeBracket> Brackets;
};

/// Whether a palette's base is light, selecting the light code color set.
[[nodiscard]] bool IsLightCodePalette(const QPalette& palette);

/// Bounded single-pass block lexer. Qt propagates changed multiline state to
/// subsequent blocks; no compiler or whole-document regular expression runs.
class CodeSyntaxHighlighter final : public QSyntaxHighlighter
{
public:
    explicit CodeSyntaxHighlighter(QTextDocument* document);
    void SetLanguage(CodeLanguage language);
    void SetPalette(const QPalette& palette);

private:
    void highlightBlock(const QString& text) override;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    QTextCharFormat mKeywordFormat;
    QTextCharFormat mTypeFormat;
    QTextCharFormat mCommentFormat;
    QTextCharFormat mStringFormat;
    QTextCharFormat mNumberFormat;
    QTextCharFormat mDirectiveFormat;
};
} // namespace ludus::editor
