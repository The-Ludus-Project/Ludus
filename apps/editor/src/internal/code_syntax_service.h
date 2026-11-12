#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_editor.h"

#include <QList>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QTimer>

#if !defined(Q_OS_WASM)
#    include <QFutureWatcher>
#endif

class QTextDocument;

namespace ludus::editor
{
enum class CodeSyntaxRole : foundation::uint8
{
    Default,
    Keyword,
    Control,
    Type,
    Function,
    Method,
    Variable,
    Parameter,
    Property,
    Constant,
    Number,
    String,
    Escape,
    Comment,
    Documentation,
    Preprocessor,
    Macro,
    Namespace,
    Operator,
    Punctuation,
    Invalid,
    Count,
};

enum class CodeFoldKind : foundation::uint8
{
    Syntax,
    Comment,
    Region,
    Indentation,
    Manual,
};

struct CodeSyntaxRange final
{
    foundation::int32 Start = 0;
    foundation::int32 End = 0; ///< Exclusive UTF-16 position in the projection.
    [[nodiscard]] bool operator==(const CodeSyntaxRange& other) const = default;
};

struct CodeSyntaxToken final
{
    CodeSyntaxRange Range;
    CodeSyntaxRole Role = CodeSyntaxRole::Default;
};

struct CodeSyntaxNode final
{
    CodeSyntaxRange Range;
    foundation::int32 Parent = -1; ///< Index into Nodes, or -1 for the document root.
    QString Label;                 ///< Empty for nodes that participate only in selection expansion.
};

struct CodeSyntaxFold final
{
    foundation::int32 FirstLine = 0;
    foundation::int32 LastLine = 0; ///< Inclusive, zero-based.
    CodeFoldKind Kind = CodeFoldKind::Syntax;
    QString Label;
    [[nodiscard]] bool operator==(const CodeSyntaxFold& other) const = default;
};

struct CodeSyntaxResult final
{
    foundation::uint64 Revision = 0;
    QList<CodeSyntaxToken> Tokens;    ///< Ordered and disjoint. Gaps keep the fallback format.
    QList<CodeSyntaxNode> Nodes;      ///< Preorder. Every published parent precedes its children.
    QList<foundation::int32> Outline; ///< Indices of labeled nodes.
    QList<CodeSyntaxFold> Folds;
    bool HasPartialFolds = false;

    [[nodiscard]] foundation::int32 FindContainingNode(CodeSyntaxRange range) const noexcept;
};

class CodeSyntaxParser;

/// Owns one projection's revision and debounce. The parser never reads a live
/// document. A single native worker services snapshots, and Wasm resumes parser
/// work on the GUI event loop. Results are valid until the next text or language change.
class CodeSyntaxService final : public QObject
{
    Q_OBJECT
public:
    explicit CodeSyntaxService(QTextDocument* document);
    ~CodeSyntaxService() override;
    void SetLanguage(CodeLanguage language);
    void SetLargeFileMode(bool isLarge);
    [[nodiscard]] const CodeSyntaxResult* GetCurrentResultOrNull() const noexcept;

Q_SIGNALS:
    void ResultChanged();

private:
    void invalidate();
    void submit();
    void publish(CodeSyntaxResult result);
#if defined(Q_OS_WASM)
    void advance();
    foundation::uint64 mActiveRevision = 0;
#else
    QFutureWatcher<CodeSyntaxResult> mWatcher;
#endif
    QTextDocument* mDocument;
    QTimer mDebounce;
    QSharedPointer<CodeSyntaxParser> mParser;
    CodeSyntaxResult mResult;
    foundation::uint64 mRevision = 1;
    foundation::int32 mDocumentRevision;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    bool mIsLargeFile = false;
    bool mIsRunning = false;
    bool mHasPendingSnapshot = false;
};
} // namespace ludus::editor
