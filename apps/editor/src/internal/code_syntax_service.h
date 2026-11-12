#pragma once

#include <ludus/foundation/base/compiler.h>
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

/// Two words per token. The low bits of the second word store its role.
class CodeSyntaxToken final
{
public:
    static constexpr foundation::uint32 ROLE_BITS = 5;
    static constexpr foundation::uint32 ROLE_MASK = (1U << ROLE_BITS) - 1U;
    static constexpr foundation::uint32 MAXIMUM_END = ~foundation::uint32{0} >> ROLE_BITS;

    constexpr CodeSyntaxToken() noexcept = default;
    constexpr CodeSyntaxToken(CodeSyntaxRange range, CodeSyntaxRole role) noexcept
        : mStart(range.Start),
          mEndAndRole((static_cast<foundation::uint32>(range.End) << ROLE_BITS) | static_cast<foundation::uint32>(role))
    {
    }
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetStart() const noexcept
    {
        return mStart;
    }
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetEnd() const noexcept
    {
        return static_cast<foundation::int32>(mEndAndRole >> ROLE_BITS);
    }
    [[nodiscard]] LUDUS_INLINE CodeSyntaxRole GetRole() const noexcept
    {
        return static_cast<CodeSyntaxRole>(mEndAndRole & ROLE_MASK);
    }
    [[nodiscard]] bool operator==(const CodeSyntaxToken& other) const = default;

private:
    foundation::int32 mStart = 0;
    foundation::uint32 mEndAndRole = 0;
};

struct CodeSyntaxNode final
{
    CodeSyntaxRange Range;
    foundation::int32 Parent = -1;     ///< Index into Nodes, or -1 for the document root.
    foundation::int32 LabelIndex = -1; ///< A negative index marks a selection-only node.
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
    QList<CodeSyntaxToken> Tokens; ///< Ordered and disjoint. Gaps keep the fallback format.
    QList<CodeSyntaxNode> Nodes;   ///< Preorder. Every published parent precedes its children.
    QList<QString> Labels;
    QList<foundation::int32> Outline; ///< Indices of labeled nodes.
    QList<CodeSyntaxFold> Folds;
    foundation::int64 WorkerMicroseconds = 0;
    bool HasPartialFolds = false;
    bool HasReusedProducts = false;

    [[nodiscard]] QString GetLabel(const CodeSyntaxNode& node) const;
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
