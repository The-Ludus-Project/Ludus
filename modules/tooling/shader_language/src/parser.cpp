// Recursive-descent parser for the LUSL grammar (docs/architecture/shading-language.md). Binary
// expressions use precedence climbing with C's operator precedence. Recursion is bounded by
// MAX_SYNTAX_DEPTH so the browser build stays within its stack.

#include <ludus/tooling/shader_language/syntax.hpp>

#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/diagnostic_sink.hpp"
#include "internal/fix_builder.hpp"
#include "internal/keywords.hpp"

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::usize;
using internal::ComposeMessage;

/// Binary operator precedences from C; higher binds tighter.
constexpr uint32 PRECEDENCE_LOGICAL_OR = 1;
constexpr uint32 PRECEDENCE_LOGICAL_AND = 2;
constexpr uint32 PRECEDENCE_BITWISE_OR = 3;
constexpr uint32 PRECEDENCE_BITWISE_XOR = 4;
constexpr uint32 PRECEDENCE_BITWISE_AND = 5;
constexpr uint32 PRECEDENCE_EQUALITY = 6;
constexpr uint32 PRECEDENCE_RELATIONAL = 7;
constexpr uint32 PRECEDENCE_SHIFT = 8;
constexpr uint32 PRECEDENCE_ADDITIVE = 9;
constexpr uint32 PRECEDENCE_MULTIPLICATIVE = 10;

/// Precedence of a binary operator, or zero for a token that is not one.
[[nodiscard]] uint32 GetPrecedence(Punctuator symbol) noexcept
{
    switch (symbol)
    {
        case Punctuator::PipePipe:
            return PRECEDENCE_LOGICAL_OR;
        case Punctuator::AmpersandAmpersand:
            return PRECEDENCE_LOGICAL_AND;
        case Punctuator::Pipe:
            return PRECEDENCE_BITWISE_OR;
        case Punctuator::Caret:
            return PRECEDENCE_BITWISE_XOR;
        case Punctuator::Ampersand:
            return PRECEDENCE_BITWISE_AND;
        case Punctuator::EqualEqual:
        case Punctuator::ExclamationEqual:
            return PRECEDENCE_EQUALITY;
        case Punctuator::Less:
        case Punctuator::LessEqual:
        case Punctuator::Greater:
        case Punctuator::GreaterEqual:
            return PRECEDENCE_RELATIONAL;
        case Punctuator::LessLess:
        case Punctuator::GreaterGreater:
            return PRECEDENCE_SHIFT;
        case Punctuator::Plus:
        case Punctuator::Minus:
            return PRECEDENCE_ADDITIVE;
        case Punctuator::Star:
        case Punctuator::Slash:
        case Punctuator::Percent:
            return PRECEDENCE_MULTIPLICATIVE;
        default:
            return 0;
    }
}

[[nodiscard]] bool IsAssignmentOperator(Punctuator symbol) noexcept
{
    switch (symbol)
    {
        case Punctuator::Equal:
        case Punctuator::PlusEqual:
        case Punctuator::MinusEqual:
        case Punctuator::StarEqual:
        case Punctuator::SlashEqual:
        case Punctuator::PercentEqual:
        case Punctuator::AmpersandEqual:
        case Punctuator::PipeEqual:
        case Punctuator::CaretEqual:
        case Punctuator::LessLessEqual:
        case Punctuator::GreaterGreaterEqual:
            return true;
        default:
            return false;
    }
}

/// Where a declaration appears, which decides what it may declare.
enum class DeclarationScope : foundation::uint8
{
    TopLevel,
    Struct,
    ExternBlock,
    Statement,
};

class Parser final
{
public:
    Parser(const PreprocessRequest& request, const PreprocessResult& input, SyntaxTree& outTree) noexcept
        : mRequest(request), mInput(input), mTree(outTree), mSink(outTree.Diagnostics, outTree.DroppedDiagnosticCount)
    {
    }

    [[nodiscard]] Status Run() noexcept
    {
        if (mInput.Outcome != Status::Ready)
        {
            mSink.ReportError(DiagnosticCode::InvalidRequest,
                              SourceLocation{},
                              SourceLocation{},
                              ComposeMessage("Parse needs a Ready preprocessing result"));
            mTree.Outcome = Status::InvalidRequest;
            return mTree.Outcome;
        }
        const uint32 root = createNode(SyntaxKind::TranslationUnit);
        while (mIsFatal == false && isAtEnd() == false)
        {
            const usize before = mPosition;
            const uint32 declaration = parseDeclaration(DeclarationScope::TopLevel);
            addChild(root, declaration);
            if (mIsRecovering == true)
            {
                synchronizeDeclaration();
            }
            if (mPosition == before && isAtEnd() == false)
            {
                // Guarantee progress after an error that consumed nothing.
                ++mPosition;
            }
        }
        if (root != NO_NODE)
        {
            mTree.Nodes[root].LastToken = getLastConsumed();
        }
        Status outcome = Status::Ready;
        if (mSink.HasOutOfMemory() == true)
        {
            outcome = Status::OutOfMemory;
        }
        else if (mSink.HasErrors() == true)
        {
            outcome = Status::Failed;
        }
        mTree.Outcome = outcome;
        return outcome;
    }

private:
    // ----- Tokens --------------------------------------------------------------------------------

    [[nodiscard]] bool isAtEnd() const noexcept
    {
        return mPosition >= mInput.Tokens.GetSize();
    }

    [[nodiscard]] const Token* getTokenOrNull(usize offset) const noexcept
    {
        const usize index = mPosition + offset;
        return index < mInput.Tokens.GetSize() ? &mInput.Tokens[index] : nullptr;
    }

    [[nodiscard]] std::string_view spell(usize index) const noexcept
    {
        return index < mInput.Tokens.GetSize() ? GetTokenSpelling(mRequest, mInput, mInput.Tokens[index])
                                               : std::string_view{};
    }

    [[nodiscard]] std::string_view spellCurrent() const noexcept
    {
        return isAtEnd() == true ? std::string_view("the end of the file") : spell(mPosition);
    }

    [[nodiscard]] bool isPunctuator(Punctuator symbol, usize offset = 0) const noexcept
    {
        const Token* tokenOrNull = getTokenOrNull(offset);
        return tokenOrNull != nullptr && tokenOrNull->Kind == TokenKind::Punctuator && tokenOrNull->Symbol == symbol;
    }

    [[nodiscard]] bool isWord(std::string_view word, usize offset = 0) const noexcept
    {
        const Token* tokenOrNull = getTokenOrNull(offset);
        return tokenOrNull != nullptr && tokenOrNull->Kind == TokenKind::Identifier &&
               spell(mPosition + offset) == word;
    }

    /// True for an identifier that is not a keyword.
    [[nodiscard]] bool isName(usize offset = 0) const noexcept
    {
        const Token* tokenOrNull = getTokenOrNull(offset);
        return tokenOrNull != nullptr && tokenOrNull->Kind == TokenKind::Identifier &&
               internal::IsKeyword(spell(mPosition + offset)) == false;
    }

    [[nodiscard]] bool consumePunctuator(Punctuator symbol) noexcept
    {
        if (isPunctuator(symbol) == false)
        {
            return false;
        }
        ++mPosition;
        return true;
    }

    [[nodiscard]] uint32 getLastConsumed() const noexcept
    {
        return mPosition == 0 ? NO_TOKEN : static_cast<uint32>(mPosition - 1);
    }

    // ----- Diagnostics ---------------------------------------------------------------------------

    [[nodiscard]] SourceLocation getExpansionLocation(const Token& token) const noexcept
    {
        if (token.ExpansionIndex == NO_EXPANSION || token.ExpansionIndex >= mInput.Expansions.GetSize())
        {
            return SourceLocation{};
        }
        return mInput.Expansions[token.ExpansionIndex];
    }

    /// Report at the current token; suppressed while recovering from an earlier error.
    void report(DiagnosticCode code, const DiagnosticMessage& message) noexcept
    {
        reportWithFix(code, message, DiagnosticFix{});
    }

    void reportWithFix(DiagnosticCode code, const DiagnosticMessage& message, const DiagnosticFix& fix) noexcept
    {
        if (mIsRecovering == true)
        {
            return;
        }
        mIsRecovering = true;
        const usize index = isAtEnd() == true && mPosition != 0 ? mPosition - 1 : mPosition;
        if (index >= mInput.Tokens.GetSize())
        {
            mSink.Report(Severity::Error, code, SourceLocation{}, SourceLocation{}, message, fix);
            return;
        }
        const Token& token = mInput.Tokens[index];
        mSink.Report(Severity::Error, code, token.Location, getExpansionLocation(token), message, fix);
    }

    /// Index just past a balanced bracket group that opens at `open`, or NO_TOKEN when unbalanced.
    [[nodiscard]] usize skipBalanced(usize open) const noexcept
    {
        usize nesting = 0;
        for (usize i = open; i < mInput.Tokens.GetSize(); ++i)
        {
            const Token& token = mInput.Tokens[i];
            if (token.Kind != TokenKind::Punctuator)
            {
                continue;
            }
            if (token.Symbol == Punctuator::LeftParenthesis || token.Symbol == Punctuator::LeftBracket)
            {
                ++nesting;
            }
            else if (token.Symbol == Punctuator::RightParenthesis || token.Symbol == Punctuator::RightBracket)
            {
                --nesting;
                if (nesting == 0)
                {
                    return i + 1;
                }
            }
        }
        return NO_TOKEN;
    }

    /// @brief The fix that rewrites `!operand` at `bang` as `operand == false`.
    ///
    /// The operand is the postfix expression after `!`. Parentheses are added unless the operand is
    /// followed by a token that binds more loosely than `==`.
    [[nodiscard]] DiagnosticFix makeLogicalNotFix(usize bang) const noexcept
    {
        usize end = bang + 1;
        if (end >= mInput.Tokens.GetSize())
        {
            return DiagnosticFix{};
        }
        const Token& first = mInput.Tokens[end];
        if (first.Kind == TokenKind::Identifier || first.Kind == TokenKind::Number)
        {
            ++end;
        }
        else if (first.Kind == TokenKind::Punctuator && first.Symbol == Punctuator::LeftParenthesis)
        {
            end = skipBalanced(end);
        }
        else
        {
            return DiagnosticFix{};
        }
        while (end != NO_TOKEN && end < mInput.Tokens.GetSize())
        {
            const Token& next = mInput.Tokens[end];
            const bool isMember = next.Kind == TokenKind::Punctuator &&
                                  (next.Symbol == Punctuator::Period || next.Symbol == Punctuator::ColonColon) &&
                                  end + 1 < mInput.Tokens.GetSize() &&
                                  mInput.Tokens[end + 1].Kind == TokenKind::Identifier;
            if (isMember == true)
            {
                end += 2;
                continue;
            }
            if (next.Kind == TokenKind::Punctuator &&
                (next.Symbol == Punctuator::LeftParenthesis || next.Symbol == Punctuator::LeftBracket))
            {
                end = skipBalanced(end);
                continue;
            }
            break;
        }
        if (end == NO_TOKEN)
        {
            return DiagnosticFix{};
        }
        bool isLooseContext = end >= mInput.Tokens.GetSize();
        if (isLooseContext == false && mInput.Tokens[end].Kind == TokenKind::Punctuator)
        {
            switch (mInput.Tokens[end].Symbol)
            {
                case Punctuator::RightParenthesis:
                case Punctuator::RightBracket:
                case Punctuator::RightBrace:
                case Punctuator::Semicolon:
                case Punctuator::Comma:
                case Punctuator::AmpersandAmpersand:
                case Punctuator::PipePipe:
                case Punctuator::Question:
                case Punctuator::Colon:
                    isLooseContext = true;
                    break;
                default:
                    break;
            }
        }
        internal::FixBuilder fix(mRequest, mInput);
        if (isLooseContext == true)
        {
            fix.Remove(static_cast<uint32>(bang));
            fix.InsertAfter(static_cast<uint32>(end - 1), " == false");
        }
        else
        {
            fix.Replace(static_cast<uint32>(bang), "(");
            fix.InsertAfter(static_cast<uint32>(end - 1), " == false)");
        }
        return fix.GetFix();
    }

    void reportExpected(std::string_view expected) noexcept
    {
        const bool isIncrement =
            isPunctuator(Punctuator::PlusPlus) == true || isPunctuator(Punctuator::MinusMinus) == true;
        if (isIncrement == true || (isAtEnd() == false && mInput.Tokens[mPosition].Kind == TokenKind::Punctuator &&
                                    IsAssignmentOperator(mInput.Tokens[mPosition].Symbol) == true))
        {
            report(
                DiagnosticCode::AssignmentInExpression,
                ComposeMessage("'", spellCurrent(), "' is an assignment or increment, which is a statement in LUSL"));
            return;
        }
        report(DiagnosticCode::UnexpectedToken,
               ComposeMessage("expected ", expected, ", found '", spellCurrent(), "'"));
    }

    [[nodiscard]] bool expect(Punctuator symbol, std::string_view expected) noexcept
    {
        if (consumePunctuator(symbol) == true)
        {
            return true;
        }
        reportExpected(expected);
        return false;
    }

    void fail(DiagnosticCode code, const DiagnosticMessage& message) noexcept
    {
        mIsRecovering = false;
        report(code, message);
        mIsFatal = true;
    }

    // ----- Nodes ---------------------------------------------------------------------------------

    [[nodiscard]] uint32 createNode(SyntaxKind kind) noexcept
    {
        if (mIsFatal == true)
        {
            return NO_NODE;
        }
        if (mTree.Nodes.GetSize() >= MAX_SYNTAX_NODES)
        {
            fail(DiagnosticCode::SyntaxNodeLimit,
                 ComposeMessage("the source needs more than ", foundation::uint64{MAX_SYNTAX_NODES}, " syntax nodes"));
            return NO_NODE;
        }
        SyntaxNode node;
        node.Kind = kind;
        node.FirstToken = isAtEnd() == true ? getLastConsumed() : static_cast<uint32>(mPosition);
        node.LastToken = node.FirstToken;
        if (mTree.Nodes.TryAdd(node) == false || mLastChildren.TryAdd(NO_NODE) == false)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
            return NO_NODE;
        }
        return static_cast<uint32>(mTree.Nodes.GetSize() - 1);
    }

    /// Create a node whose principal token is the current one, and consume it.
    [[nodiscard]] uint32 createTokenNode(SyntaxKind kind) noexcept
    {
        const uint32 node = createNode(kind);
        if (node != NO_NODE)
        {
            mTree.Nodes[node].Token = static_cast<uint32>(mPosition);
        }
        ++mPosition;
        finish(node);
        return node;
    }

    void addChild(uint32 parent, uint32 child) noexcept
    {
        if (parent == NO_NODE || child == NO_NODE)
        {
            return;
        }
        if (mTree.Nodes[parent].FirstChild == NO_NODE)
        {
            mTree.Nodes[parent].FirstChild = child;
        }
        else
        {
            mTree.Nodes[mLastChildren[parent]].NextSibling = child;
        }
        mLastChildren[parent] = child;
        // A child parsed before its parent was created (a binary operand) extends the parent.
        if (mTree.Nodes[child].FirstToken < mTree.Nodes[parent].FirstToken)
        {
            mTree.Nodes[parent].FirstToken = mTree.Nodes[child].FirstToken;
        }
    }

    void finish(uint32 node) noexcept
    {
        if (node != NO_NODE)
        {
            const uint32 last = getLastConsumed();
            mTree.Nodes[node].LastToken =
                last == NO_TOKEN || last < mTree.Nodes[node].FirstToken ? mTree.Nodes[node].FirstToken : last;
        }
    }

    void setToken(uint32 node, usize token) noexcept
    {
        if (node != NO_NODE)
        {
            mTree.Nodes[node].Token = static_cast<uint32>(token);
        }
    }

    [[nodiscard]] uint32 createErrorNode() noexcept
    {
        const uint32 node = createNode(SyntaxKind::Error);
        finish(node);
        return node;
    }

    [[nodiscard]] bool enter() noexcept
    {
        ++mDepth;
        if (mDepth > MAX_SYNTAX_DEPTH)
        {
            fail(DiagnosticCode::SyntaxDepthLimit,
                 ComposeMessage("statements and expressions nest deeper than ", foundation::uint64{MAX_SYNTAX_DEPTH}));
            return false;
        }
        return true;
    }

    void leave() noexcept
    {
        --mDepth;
    }

    // ----- Recovery ------------------------------------------------------------------------------

    /// Skip to just after the next `;`, or to the `}` closing the current block, at nesting zero.
    void synchronizeStatement() noexcept
    {
        uint32 nesting = 0;
        while (isAtEnd() == false)
        {
            const Token& token = mInput.Tokens[mPosition];
            if (token.Kind == TokenKind::Punctuator)
            {
                if (token.Symbol == Punctuator::Semicolon && nesting == 0)
                {
                    ++mPosition;
                    break;
                }
                if (token.Symbol == Punctuator::LeftBrace || token.Symbol == Punctuator::LeftParenthesis ||
                    token.Symbol == Punctuator::LeftBracket)
                {
                    ++nesting;
                }
                else if (token.Symbol == Punctuator::RightBrace || token.Symbol == Punctuator::RightParenthesis ||
                         token.Symbol == Punctuator::RightBracket)
                {
                    // Only a `}` closes the enclosing block; an unmatched `)` or `]` belongs to the
                    // statement being skipped.
                    if (nesting == 0 && token.Symbol == Punctuator::RightBrace)
                    {
                        break;
                    }
                    nesting = nesting > 0 ? nesting - 1 : 0;
                }
            }
            ++mPosition;
        }
        mIsRecovering = false;
    }

    /// Skip to just after the next `;` or block at nesting zero, at top level.
    void synchronizeDeclaration() noexcept
    {
        uint32 nesting = 0;
        while (isAtEnd() == false)
        {
            const Token& token = mInput.Tokens[mPosition];
            ++mPosition;
            if (token.Kind != TokenKind::Punctuator)
            {
                continue;
            }
            if (token.Symbol == Punctuator::Semicolon && nesting == 0)
            {
                break;
            }
            if (token.Symbol == Punctuator::LeftBrace || token.Symbol == Punctuator::LeftParenthesis ||
                token.Symbol == Punctuator::LeftBracket)
            {
                ++nesting;
            }
            else if (token.Symbol == Punctuator::RightBrace || token.Symbol == Punctuator::RightParenthesis ||
                     token.Symbol == Punctuator::RightBracket)
            {
                if (nesting > 0)
                {
                    --nesting;
                }
                if (nesting == 0 && token.Symbol == Punctuator::RightBrace)
                {
                    (void)consumePunctuator(Punctuator::Semicolon);
                    break;
                }
            }
        }
        mIsRecovering = false;
    }

    // ----- Declarations --------------------------------------------------------------------------

    /// Parse `[a, b(1)][c]` attribute groups into `outAttributes`.
    void parseAttributes(Array<uint32>& outAttributes) noexcept
    {
        while (mIsFatal == false && isPunctuator(Punctuator::LeftBracket) == true &&
               isPunctuator(Punctuator::LeftBracket, 1) == false)
        {
            ++mPosition;
            while (mIsFatal == false)
            {
                if (isName() == false)
                {
                    reportExpected("an attribute name");
                    return;
                }
                const uint32 attribute = createNode(SyntaxKind::Attribute);
                setToken(attribute, mPosition);
                ++mPosition;
                if (consumePunctuator(Punctuator::LeftParenthesis) == true &&
                    consumePunctuator(Punctuator::RightParenthesis) == false)
                {
                    while (mIsFatal == false)
                    {
                        addChild(attribute, parseExpression());
                        if (consumePunctuator(Punctuator::Comma) == true)
                        {
                            continue;
                        }
                        if (expect(Punctuator::RightParenthesis, "',' or ')' after an attribute argument") == false)
                        {
                            return;
                        }
                        break;
                    }
                }
                finish(attribute);
                if (outAttributes.TryAdd(attribute) == false)
                {
                    mSink.ReportOutOfMemory();
                    mIsFatal = true;
                    return;
                }
                if (consumePunctuator(Punctuator::Comma) == true)
                {
                    continue;
                }
                if (expect(Punctuator::RightBracket, "',' or ']' in the attribute list") == false)
                {
                    return;
                }
                break;
            }
        }
    }

    void addChildren(uint32 parent, const Array<uint32>& children) noexcept
    {
        for (const uint32 child : children)
        {
            addChild(parent, child);
        }
    }

    /// Report a construct that a later phase adds, then skip it.
    [[nodiscard]] bool rejectLaterPhase() noexcept
    {
        for (const std::string_view keyword : internal::LATER_PHASE_KEYWORDS)
        {
            if (isWord(keyword) == true)
            {
                report(DiagnosticCode::LaterPhaseConstruct,
                       ComposeMessage("'", keyword, "' arrives with interfaces, concepts and templates in LUSL L2"));
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] uint32 parseDeclaration(DeclarationScope scope) noexcept
    {
        Array<uint32> attributes;
        parseAttributes(attributes);
        if (mIsRecovering == true || mIsFatal == true)
        {
            return createErrorNode();
        }
        if (rejectLaterPhase() == true)
        {
            return createErrorNode();
        }
        if (isWord("struct") == true)
        {
            return parseStruct(attributes);
        }
        if (isWord("enum") == true)
        {
            return parseEnum(attributes);
        }
        if (isWord("extern") == true && scope == DeclarationScope::TopLevel)
        {
            return parseExternBlock(attributes);
        }
        if (isWord("static_assert") == true)
        {
            return parseStaticAssert();
        }
        if (isDeclarationStart() == false)
        {
            reportExpected("a declaration");
            return createErrorNode();
        }
        return parseVariableOrFunction(attributes, scope);
    }

    [[nodiscard]] uint32 parseStruct(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::StructDeclaration);
        ++mPosition;
        if (parseDeclaredName(node) == false)
        {
            return node;
        }
        addChildren(node, attributes);
        if (isPunctuator(Punctuator::Colon) == true)
        {
            report(DiagnosticCode::LaterPhaseConstruct,
                   ComposeMessage("interface conformance arrives with interfaces in LUSL L2"));
            return node;
        }
        if (consumePunctuator(Punctuator::Semicolon) == true)
        {
            // An opaque declaration; the checker accepts it only in an `extern slang` block.
            finish(node);
            return node;
        }
        if (expect(Punctuator::LeftBrace, "'{' after the struct name") == false)
        {
            return node;
        }
        while (mIsFatal == false && isAtEnd() == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            const usize before = mPosition;
            addChild(node, parseDeclaration(DeclarationScope::Struct));
            if (mIsRecovering == true)
            {
                synchronizeStatement();
            }
            if (mPosition == before)
            {
                ++mPosition;
            }
        }
        if (expect(Punctuator::RightBrace, "'}' to close the struct") == true)
        {
            (void)expect(Punctuator::Semicolon, "';' after the struct");
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseEnum(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::EnumDeclaration);
        ++mPosition;
        if (isWord("class") == true)
        {
            ++mPosition;
        }
        else
        {
            report(DiagnosticCode::UnscopedEnum, ComposeMessage("LUSL enums are scoped; write 'enum class'"));
            return node;
        }
        if (parseDeclaredName(node) == false)
        {
            return node;
        }
        if (attributes.IsEmpty() == false)
        {
            report(DiagnosticCode::UnexpectedToken, ComposeMessage("enums take no attributes"));
            return node;
        }
        if (expect(Punctuator::Colon, "':' and the underlying type of the enum") == false)
        {
            return node;
        }
        addChild(node, parseType());
        if (expect(Punctuator::LeftBrace, "'{' after the underlying type") == false)
        {
            return node;
        }
        while (mIsFatal == false && mIsRecovering == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            const uint32 enumerator = createNode(SyntaxKind::Enumerator);
            if (parseDeclaredName(enumerator) == false)
            {
                break;
            }
            if (consumePunctuator(Punctuator::Equal) == true)
            {
                addChild(enumerator, parseExpression());
            }
            finish(enumerator);
            addChild(node, enumerator);
            if (consumePunctuator(Punctuator::Comma) == false)
            {
                break;
            }
        }
        if (mIsRecovering == false && expect(Punctuator::RightBrace, "',' or '}' after an enumerator") == true)
        {
            (void)expect(Punctuator::Semicolon, "';' after the enum");
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseExternBlock(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::ExternSlangBlock);
        ++mPosition;
        if (isWord("slang") == false)
        {
            reportExpected("'slang' after 'extern'");
            return node;
        }
        ++mPosition;
        const Token* pathOrNull = getTokenOrNull(0);
        if (pathOrNull == nullptr || pathOrNull->Kind != TokenKind::String)
        {
            reportExpected("the quoted path of the Slang file");
            return node;
        }
        setToken(node, mPosition);
        ++mPosition;
        if (attributes.IsEmpty() == false)
        {
            report(DiagnosticCode::UnexpectedToken, ComposeMessage("'extern slang' blocks take no attributes"));
            return node;
        }
        if (expect(Punctuator::LeftBrace, "'{' after the Slang path") == false)
        {
            return node;
        }
        while (mIsFatal == false && isAtEnd() == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            const usize before = mPosition;
            addChild(node, parseDeclaration(DeclarationScope::ExternBlock));
            if (mIsRecovering == true)
            {
                synchronizeStatement();
            }
            if (mPosition == before)
            {
                ++mPosition;
            }
        }
        (void)expect(Punctuator::RightBrace, "'}' to close the 'extern slang' block");
        (void)consumePunctuator(Punctuator::Semicolon);
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseStaticAssert() noexcept
    {
        const uint32 node = createNode(SyntaxKind::StaticAssert);
        setToken(node, mPosition);
        ++mPosition;
        if (expect(Punctuator::LeftParenthesis, "'(' after 'static_assert'") == false)
        {
            return node;
        }
        addChild(node, parseExpression());
        if (expect(Punctuator::Comma, "',' and a message string") == false)
        {
            return node;
        }
        const Token* messageOrNull = getTokenOrNull(0);
        if (messageOrNull == nullptr || messageOrNull->Kind != TokenKind::String)
        {
            reportExpected("a message string");
            return node;
        }
        addChild(node, createTokenNode(SyntaxKind::StringLiteral));
        if (expect(Punctuator::RightParenthesis, "')' after the message") == true)
        {
            (void)expect(Punctuator::Semicolon, "';' after 'static_assert'");
        }
        finish(node);
        return node;
    }

    /// Consume a declared name into `node`'s token; reports keywords and missing names.
    [[nodiscard]] bool parseDeclaredName(uint32 node) noexcept
    {
        if (isName() == true)
        {
            setToken(node, mPosition);
            ++mPosition;
            return true;
        }
        if (isAtEnd() == false && mInput.Tokens[mPosition].Kind == TokenKind::Identifier)
        {
            report(DiagnosticCode::KeywordAsName,
                   ComposeMessage("'", spellCurrent(), "' is a LUSL keyword and cannot be a name"));
            return false;
        }
        reportExpected("a name");
        return false;
    }

    /// Whether the tokens at the current position begin a variable or function declaration.
    [[nodiscard]] bool isDeclarationStart() const noexcept
    {
        if (isWord("const") == true || isWord("static") == true || isWord("groupshared") == true ||
            isWord("void") == true)
        {
            return true;
        }
        usize offset = 0;
        if (isName(offset) == false)
        {
            return false;
        }
        ++offset;
        while (isPunctuator(Punctuator::ColonColon, offset) == true && isName(offset + 1) == true)
        {
            offset += 2;
        }
        if (isPunctuator(Punctuator::Less, offset) == true)
        {
            // A generic type: match the angle brackets through names, numbers, commas and `::`.
            usize nesting = 0;
            while (true)
            {
                const Token* tokenOrNull = getTokenOrNull(offset);
                if (tokenOrNull == nullptr)
                {
                    return false;
                }
                if (tokenOrNull->Kind == TokenKind::Punctuator)
                {
                    if (tokenOrNull->Symbol == Punctuator::Less)
                    {
                        ++nesting;
                    }
                    else if (tokenOrNull->Symbol == Punctuator::Greater ||
                             tokenOrNull->Symbol == Punctuator::GreaterGreater)
                    {
                        const usize closed = tokenOrNull->Symbol == Punctuator::Greater ? 1 : 2;
                        if (closed > nesting)
                        {
                            return false;
                        }
                        nesting -= closed;
                    }
                    else if (tokenOrNull->Symbol != Punctuator::Comma && tokenOrNull->Symbol != Punctuator::ColonColon)
                    {
                        return false;
                    }
                }
                else if (tokenOrNull->Kind != TokenKind::Identifier && tokenOrNull->Kind != TokenKind::Number)
                {
                    return false;
                }
                ++offset;
                if (nesting == 0)
                {
                    break;
                }
            }
        }
        // Any identifier counts, so a keyword in name position reports KeywordAsName.
        const Token* nameOrNull = getTokenOrNull(offset);
        return nameOrNull != nullptr && nameOrNull->Kind == TokenKind::Identifier;
    }

    [[nodiscard]] uint32 parseVariableOrFunction(const Array<uint32>& attributes, DeclarationScope scope) noexcept
    {
        const usize start = mPosition;
        bool isStatic = false;
        bool isConst = false;
        bool isGroupshared = false;
        while (true)
        {
            bool* flagOrNull = nullptr;
            if (isWord("static") == true)
            {
                flagOrNull = &isStatic;
            }
            else if (isWord("const") == true)
            {
                flagOrNull = &isConst;
            }
            else if (isWord("groupshared") == true)
            {
                flagOrNull = &isGroupshared;
            }
            if (flagOrNull == nullptr)
            {
                break;
            }
            if (*flagOrNull == true)
            {
                report(DiagnosticCode::UnexpectedToken, ComposeMessage("'", spellCurrent(), "' appears twice"));
                return createErrorNode();
            }
            *flagOrNull = true;
            ++mPosition;
        }
        const uint32 type = parseType();
        const bool isFunction = isName() == true && isPunctuator(Punctuator::LeftParenthesis, 1) == true;
        const bool isModified = isStatic == true || isConst == true || isGroupshared == true;
        if (isFunction == true && scope != DeclarationScope::Statement && isModified == false)
        {
            return parseFunction(start, attributes, type);
        }
        const uint32 node = createNode(SyntaxKind::VariableDeclaration);
        if (node == NO_NODE)
        {
            return NO_NODE;
        }
        mTree.Nodes[node].FirstToken = static_cast<uint32>(start);
        mTree.Nodes[node].IsStatic = isStatic;
        mTree.Nodes[node].IsConst = isConst;
        mTree.Nodes[node].IsGroupshared = isGroupshared;
        addChildren(node, attributes);
        addChild(node, type);
        if (parseDeclaredName(node) == false)
        {
            return node;
        }
        parseArraySizes(node);
        if (consumePunctuator(Punctuator::Equal) == true)
        {
            if (isWord("void") == true)
            {
                addChild(node, createTokenNode(SyntaxKind::VoidInitializer));
            }
            else
            {
                addChild(node, parseExpression());
            }
        }
        if (isPunctuator(Punctuator::Comma) == true)
        {
            report(DiagnosticCode::MultipleDeclarators, ComposeMessage("declare one variable per statement"));
            return node;
        }
        if (scope != DeclarationScope::Statement)
        {
            (void)expect(Punctuator::Semicolon, "';' after the declaration");
        }
        finish(node);
        return node;
    }

    void parseArraySizes(uint32 node) noexcept
    {
        while (mIsFatal == false && mIsRecovering == false && isPunctuator(Punctuator::LeftBracket) == true)
        {
            const uint32 size = createNode(SyntaxKind::ArraySize);
            ++mPosition;
            addChild(size, parseExpression());
            (void)expect(Punctuator::RightBracket, "']' after the array size");
            finish(size);
            addChild(node, size);
        }
    }

    [[nodiscard]] uint32 parseFunction(usize start, const Array<uint32>& attributes, uint32 returnType) noexcept
    {
        const uint32 node = createNode(SyntaxKind::FunctionDeclaration);
        if (node == NO_NODE)
        {
            return NO_NODE;
        }
        mTree.Nodes[node].FirstToken = static_cast<uint32>(start);
        addChildren(node, attributes);
        addChild(node, returnType);
        setToken(node, mPosition);
        // The caller checked that the name is followed by '('.
        mPosition += 2;
        if (consumePunctuator(Punctuator::RightParenthesis) == false)
        {
            while (mIsFatal == false && mIsRecovering == false)
            {
                addChild(node, parseParameter());
                if (consumePunctuator(Punctuator::Comma) == true)
                {
                    continue;
                }
                (void)expect(Punctuator::RightParenthesis, "',' or ')' after a parameter");
                break;
            }
        }
        if (mIsRecovering == true)
        {
            return node;
        }
        if (consumePunctuator(Punctuator::Semicolon) == false)
        {
            if (isPunctuator(Punctuator::LeftBrace) == false)
            {
                reportExpected("'{' or ';' after the parameter list");
                return node;
            }
            addChild(node, parseBlock());
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseParameter() noexcept
    {
        const uint32 node = createNode(SyntaxKind::Parameter);
        if (node == NO_NODE)
        {
            return NO_NODE;
        }
        if (isWord("in") == true || isWord("out") == true || isWord("inout") == true)
        {
            mTree.Nodes[node].Direction = isWord("in") == true    ? ParameterDirection::In
                                          : isWord("out") == true ? ParameterDirection::Out
                                                                  : ParameterDirection::Inout;
            ++mPosition;
        }
        addChild(node, parseType());
        if (parseDeclaredName(node) == false)
        {
            return node;
        }
        parseArraySizes(node);
        if (consumePunctuator(Punctuator::Colon) == true)
        {
            const uint32 semantic = createNode(SyntaxKind::Semantic);
            if (parseDeclaredName(semantic) == false)
            {
                return node;
            }
            finish(semantic);
            addChild(node, semantic);
        }
        if (consumePunctuator(Punctuator::Equal) == true)
        {
            addChild(node, parseExpression());
        }
        finish(node);
        return node;
    }

    // ----- Types ---------------------------------------------------------------------------------

    [[nodiscard]] uint32 parseType() noexcept
    {
        const uint32 node = createNode(SyntaxKind::Type);
        if (isWord("void") == true)
        {
            setToken(node, mPosition);
            ++mPosition;
            finish(node);
            return node;
        }
        if (isName() == false)
        {
            reportExpected("a type");
            return node;
        }
        setToken(node, mPosition);
        ++mPosition;
        while (isPunctuator(Punctuator::ColonColon) == true && isName(1) == true)
        {
            setToken(node, mPosition + 1);
            mPosition += 2;
        }
        if (consumePunctuator(Punctuator::Less) == true)
        {
            while (mIsFatal == false && mIsRecovering == false)
            {
                if (isName() == true &&
                    (isPunctuator(Punctuator::Comma, 1) == true || isPunctuator(Punctuator::Greater, 1) == true ||
                     isPunctuator(Punctuator::GreaterGreater, 1) == true || isPunctuator(Punctuator::Less, 1) == true ||
                     isPunctuator(Punctuator::ColonColon, 1) == true))
                {
                    addChild(node, parseType());
                }
                else
                {
                    // A constant argument; stop below comparisons so '>' closes the list.
                    addChild(node, parseBinary(PRECEDENCE_SHIFT + 1));
                }
                if (consumePunctuator(Punctuator::Comma) == true)
                {
                    continue;
                }
                if (mHasSplitGreater == true)
                {
                    // The second half of a '>>' closes this list.
                    mHasSplitGreater = false;
                    ++mPosition;
                    break;
                }
                if (isPunctuator(Punctuator::GreaterGreater) == true)
                {
                    mHasSplitGreater = true;
                    break;
                }
                (void)expect(Punctuator::Greater, "',' or '>' in the type arguments");
                break;
            }
        }
        finish(node);
        return node;
    }

    // ----- Statements ----------------------------------------------------------------------------

    [[nodiscard]] uint32 parseBlock() noexcept
    {
        const uint32 node = createNode(SyntaxKind::Block);
        if (expect(Punctuator::LeftBrace, "'{'") == false || enter() == false)
        {
            return node;
        }
        while (mIsFatal == false && isAtEnd() == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            const usize before = mPosition;
            addChild(node, parseStatement());
            if (mIsRecovering == true)
            {
                synchronizeStatement();
            }
            if (mPosition == before && isPunctuator(Punctuator::RightBrace) == false)
            {
                ++mPosition;
            }
        }
        leave();
        (void)expect(Punctuator::RightBrace, "'}' to close the block");
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseStatement() noexcept
    {
        if (enter() == false)
        {
            return NO_NODE;
        }
        const uint32 node = parseStatementBody();
        leave();
        return node;
    }

    [[nodiscard]] uint32 parseStatementBody() noexcept
    {
        if (isPunctuator(Punctuator::LeftBracket) == true && isPunctuator(Punctuator::LeftBracket, 1) == true)
        {
            return parseFallthrough();
        }
        Array<uint32> attributes;
        parseAttributes(attributes);
        if (mIsRecovering == true || mIsFatal == true)
        {
            return createErrorNode();
        }
        if (rejectLaterPhase() == true)
        {
            return createErrorNode();
        }
        const bool hasAttributes = attributes.IsEmpty() == false;
        if (isWord("if") == true)
        {
            return parseIf(attributes);
        }
        if (isWord("for") == true)
        {
            return parseFor(attributes);
        }
        if (isWord("while") == true)
        {
            return parseWhile(attributes);
        }
        if (isWord("do") == true)
        {
            return parseDo(attributes);
        }
        if (isWord("switch") == true)
        {
            return parseSwitch(attributes);
        }
        if (isPunctuator(Punctuator::LeftBrace) == false && isDeclarationStart() == true)
        {
            const uint32 declaration = parseVariableOrFunction(attributes, DeclarationScope::Statement);
            if (mIsRecovering == false)
            {
                (void)expect(Punctuator::Semicolon, "';' after the declaration");
            }
            finish(declaration);
            return declaration;
        }
        if (hasAttributes == true)
        {
            report(DiagnosticCode::UnexpectedToken,
                   ComposeMessage("attributes apply to declarations and to if, for, while, do and switch statements"));
            return createErrorNode();
        }
        if (isPunctuator(Punctuator::LeftBrace) == true)
        {
            return parseBlock();
        }
        if (isWord("break") == true || isWord("continue") == true || isWord("discard") == true)
        {
            const SyntaxKind kind = isWord("break") == true      ? SyntaxKind::BreakStatement
                                    : isWord("continue") == true ? SyntaxKind::ContinueStatement
                                                                 : SyntaxKind::DiscardStatement;
            const uint32 node = createTokenNode(kind);
            (void)expect(Punctuator::Semicolon, "';'");
            finish(node);
            return node;
        }
        if (isWord("return") == true)
        {
            const uint32 node = createTokenNode(SyntaxKind::ReturnStatement);
            if (isPunctuator(Punctuator::Semicolon) == false)
            {
                addChild(node, parseExpression());
            }
            (void)expect(Punctuator::Semicolon, "';' after the return value");
            finish(node);
            return node;
        }
        if (isWord("static_assert") == true)
        {
            return parseStaticAssert();
        }
        if (isWord("case") == true || isWord("default") == true)
        {
            report(DiagnosticCode::UnexpectedToken,
                   ComposeMessage("'", spellCurrent(), "' is only valid directly inside a switch"));
            return createErrorNode();
        }
        if (isPunctuator(Punctuator::Semicolon) == true)
        {
            report(DiagnosticCode::ExpressionWithoutEffect,
                   ComposeMessage("an empty statement does nothing; remove the ';'"));
            ++mPosition;
            mIsRecovering = false;
            return createErrorNode();
        }
        const uint32 node = parseSimpleStatement();
        (void)expect(Punctuator::Semicolon, "';' after the statement");
        finish(node);
        return node;
    }

    /// Assignment, increment or call statement without its `;`.
    [[nodiscard]] uint32 parseSimpleStatement() noexcept
    {
        if (isPunctuator(Punctuator::PlusPlus) == true || isPunctuator(Punctuator::MinusMinus) == true)
        {
            const uint32 node = createNode(SyntaxKind::IncrementStatement);
            setToken(node, mPosition);
            ++mPosition;
            addChild(node, parseUnary());
            finish(node);
            return node;
        }
        const uint32 target = parseExpression();
        if (mIsRecovering == true)
        {
            return target;
        }
        const Token* nextOrNull = getTokenOrNull(0);
        if (nextOrNull != nullptr && nextOrNull->Kind == TokenKind::Punctuator &&
            IsAssignmentOperator(nextOrNull->Symbol) == true)
        {
            const uint32 node = createNode(SyntaxKind::AssignmentStatement);
            setToken(node, mPosition);
            ++mPosition;
            addChild(node, target);
            addChild(node, parseExpression());
            finish(node);
            return node;
        }
        if (isPunctuator(Punctuator::PlusPlus) == true || isPunctuator(Punctuator::MinusMinus) == true)
        {
            const uint32 node = createNode(SyntaxKind::IncrementStatement);
            setToken(node, mPosition);
            ++mPosition;
            addChild(node, target);
            finish(node);
            return node;
        }
        if (target == NO_NODE || mTree.Nodes[target].Kind != SyntaxKind::CallExpression)
        {
            report(DiagnosticCode::ExpressionWithoutEffect,
                   ComposeMessage("only calls, assignments and '++'/'--' can be statements"));
            return target;
        }
        const uint32 node = createNode(SyntaxKind::ExpressionStatement);
        addChild(node, target);
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseFallthrough() noexcept
    {
        const uint32 node = createNode(SyntaxKind::FallthroughStatement);
        mPosition += 2;
        if (isWord("fallthrough") == false)
        {
            reportExpected("'fallthrough' in '[[fallthrough]]'");
            return node;
        }
        setToken(node, mPosition);
        ++mPosition;
        if (expect(Punctuator::RightBracket, "']]'") == true && expect(Punctuator::RightBracket, "']]'") == true)
        {
            (void)expect(Punctuator::Semicolon, "';' after '[[fallthrough]]'");
        }
        finish(node);
        return node;
    }

    /// Parse `( expression )` as a statement condition.
    [[nodiscard]] uint32 parseCondition(std::string_view keyword) noexcept
    {
        const DiagnosticMessage expected = ComposeMessage("'(' after '", keyword, "'");
        if (expect(Punctuator::LeftParenthesis, expected.GetView()) == false)
        {
            return createErrorNode();
        }
        const uint32 condition = parseExpression();
        (void)expect(Punctuator::RightParenthesis, "')' after the condition");
        return condition;
    }

    [[nodiscard]] uint32 parseIf(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::IfStatement);
        ++mPosition;
        addChildren(node, attributes);
        addChild(node, parseCondition("if"));
        if (mIsRecovering == true)
        {
            return node;
        }
        addChild(node, parseStatement());
        if (mIsRecovering == false && isWord("else") == true)
        {
            ++mPosition;
            addChild(node, parseStatement());
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseFor(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::ForStatement);
        ++mPosition;
        addChildren(node, attributes);
        if (expect(Punctuator::LeftParenthesis, "'(' after 'for'") == false)
        {
            return node;
        }
        if (isPunctuator(Punctuator::Semicolon) == true)
        {
            addChild(node, createEmpty());
        }
        else if (isDeclarationStart() == true)
        {
            Array<uint32> none;
            addChild(node, parseVariableOrFunction(none, DeclarationScope::Statement));
        }
        else
        {
            addChild(node, parseSimpleStatement());
        }
        if (expect(Punctuator::Semicolon, "';' after the loop initializer") == false)
        {
            return node;
        }
        addChild(node, isPunctuator(Punctuator::Semicolon) == true ? createEmpty() : parseExpression());
        if (expect(Punctuator::Semicolon, "';' after the loop condition") == false)
        {
            return node;
        }
        addChild(node, isPunctuator(Punctuator::RightParenthesis) == true ? createEmpty() : parseSimpleStatement());
        if (expect(Punctuator::RightParenthesis, "')' after the loop increment") == false)
        {
            return node;
        }
        addChild(node, parseStatement());
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 createEmpty() noexcept
    {
        const uint32 node = createNode(SyntaxKind::Empty);
        if (node != NO_NODE)
        {
            mTree.Nodes[node].LastToken = mTree.Nodes[node].FirstToken;
        }
        return node;
    }

    [[nodiscard]] uint32 parseWhile(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::WhileStatement);
        ++mPosition;
        addChildren(node, attributes);
        addChild(node, parseCondition("while"));
        if (mIsRecovering == false)
        {
            addChild(node, parseStatement());
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseDo(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::DoStatement);
        ++mPosition;
        addChildren(node, attributes);
        addChild(node, parseStatement());
        if (mIsRecovering == true)
        {
            return node;
        }
        if (isWord("while") == false)
        {
            reportExpected("'while' after the 'do' body");
            return node;
        }
        ++mPosition;
        addChild(node, parseCondition("while"));
        (void)expect(Punctuator::Semicolon, "';' after 'do ... while (...)'");
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parseSwitch(const Array<uint32>& attributes) noexcept
    {
        const uint32 node = createNode(SyntaxKind::SwitchStatement);
        ++mPosition;
        addChildren(node, attributes);
        addChild(node, parseCondition("switch"));
        if (mIsRecovering == true || expect(Punctuator::LeftBrace, "'{' after the switch selector") == false)
        {
            return node;
        }
        while (mIsFatal == false && isAtEnd() == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            if (isWord("case") == false && isWord("default") == false)
            {
                reportExpected("'case' or 'default'");
                synchronizeStatement();
                continue;
            }
            const uint32 clause = createTokenNode(SyntaxKind::CaseClause);
            if (spell(mTree.Nodes[clause].Token) == "case")
            {
                addChild(clause, parseExpression());
            }
            if (expect(Punctuator::Colon, "':' after the case label") == false)
            {
                synchronizeStatement();
                continue;
            }
            while (mIsFatal == false && isAtEnd() == false && isPunctuator(Punctuator::RightBrace) == false &&
                   isWord("case") == false && isWord("default") == false)
            {
                const usize before = mPosition;
                addChild(clause, parseStatement());
                if (mIsRecovering == true)
                {
                    synchronizeStatement();
                }
                if (mPosition == before)
                {
                    ++mPosition;
                }
            }
            finish(clause);
            addChild(node, clause);
        }
        (void)expect(Punctuator::RightBrace, "'}' to close the switch");
        finish(node);
        return node;
    }

    // ----- Expressions ---------------------------------------------------------------------------

    [[nodiscard]] uint32 parseExpression() noexcept
    {
        if (enter() == false)
        {
            return NO_NODE;
        }
        const uint32 condition = parseBinary(PRECEDENCE_LOGICAL_OR);
        uint32 result = condition;
        if (mIsRecovering == false && isPunctuator(Punctuator::Question) == true)
        {
            result = createNode(SyntaxKind::ConditionalExpression);
            ++mPosition;
            addChild(result, condition);
            addChild(result, parseExpression());
            if (expect(Punctuator::Colon, "':' in the conditional expression") == true)
            {
                addChild(result, parseExpression());
            }
            finish(result);
        }
        leave();
        return result;
    }

    [[nodiscard]] uint32 parseBinary(uint32 minimum) noexcept
    {
        uint32 left = parseUnary();
        while (mIsFatal == false && mIsRecovering == false && isAtEnd() == false &&
               mInput.Tokens[mPosition].Kind == TokenKind::Punctuator)
        {
            const uint32 precedence = GetPrecedence(mInput.Tokens[mPosition].Symbol);
            if (precedence == 0 || precedence < minimum)
            {
                break;
            }
            const uint32 node = createNode(SyntaxKind::BinaryExpression);
            setToken(node, mPosition);
            ++mPosition;
            addChild(node, left);
            addChild(node, parseBinary(precedence + 1));
            finish(node);
            left = node;
        }
        return left;
    }

    [[nodiscard]] uint32 parseUnary() noexcept
    {
        if (enter() == false)
        {
            return NO_NODE;
        }
        uint32 result = NO_NODE;
        if (isPunctuator(Punctuator::Exclamation) == true)
        {
            reportWithFix(DiagnosticCode::LogicalNot,
                          ComposeMessage("LUSL has no '!'; compare with '== false'"),
                          makeLogicalNotFix(mPosition));
            result = createErrorNode();
        }
        else if (isPunctuator(Punctuator::PlusPlus) == true || isPunctuator(Punctuator::MinusMinus) == true)
        {
            report(DiagnosticCode::AssignmentInExpression,
                   ComposeMessage("'", spellCurrent(), "' is a statement in LUSL, not part of an expression"));
            result = createErrorNode();
        }
        else if (isPunctuator(Punctuator::Plus) == true || isPunctuator(Punctuator::Minus) == true ||
                 isPunctuator(Punctuator::Tilde) == true)
        {
            result = createNode(SyntaxKind::UnaryExpression);
            setToken(result, mPosition);
            ++mPosition;
            addChild(result, parseUnary());
            finish(result);
        }
        else
        {
            result = parsePostfix(parsePrimary());
        }
        leave();
        return result;
    }

    [[nodiscard]] bool isPrimaryStart() const noexcept
    {
        const Token* tokenOrNull = getTokenOrNull(0);
        if (tokenOrNull == nullptr)
        {
            return false;
        }
        return isName() == true || tokenOrNull->Kind == TokenKind::Number ||
               isPunctuator(Punctuator::LeftParenthesis) == true;
    }

    [[nodiscard]] uint32 parsePrimary() noexcept
    {
        const Token* tokenOrNull = getTokenOrNull(0);
        if (tokenOrNull == nullptr)
        {
            reportExpected("an expression");
            return createErrorNode();
        }
        if (isName() == true)
        {
            return createTokenNode(SyntaxKind::Name);
        }
        if (tokenOrNull->Kind == TokenKind::Number)
        {
            return createTokenNode(SyntaxKind::NumberLiteral);
        }
        if (tokenOrNull->Kind == TokenKind::String)
        {
            return createTokenNode(SyntaxKind::StringLiteral);
        }
        if (isWord("true") == true || isWord("false") == true)
        {
            return createTokenNode(SyntaxKind::BooleanLiteral);
        }
        if (isPunctuator(Punctuator::LeftParenthesis) == true)
        {
            const uint32 node = createNode(SyntaxKind::ParenthesizedExpression);
            ++mPosition;
            addChild(node, parseExpression());
            (void)expect(Punctuator::RightParenthesis, "')'");
            finish(node);
            if (mIsRecovering == false && isPrimaryStart() == true)
            {
                report(DiagnosticCode::CStyleCast,
                       ComposeMessage("LUSL has no C casts; write 'Type(value)' instead of '(Type)value'"));
            }
            return node;
        }
        if (isPunctuator(Punctuator::LeftBrace) == true)
        {
            return parseInitializerList();
        }
        if (tokenOrNull->Kind == TokenKind::Identifier)
        {
            report(DiagnosticCode::KeywordAsName,
                   ComposeMessage("'", spellCurrent(), "' is a LUSL keyword and cannot be used here"));
            return createErrorNode();
        }
        reportExpected("an expression");
        return createErrorNode();
    }

    [[nodiscard]] uint32 parseInitializerList() noexcept
    {
        const uint32 node = createNode(SyntaxKind::InitializerList);
        setToken(node, mPosition);
        ++mPosition;
        while (mIsFatal == false && mIsRecovering == false && isPunctuator(Punctuator::RightBrace) == false)
        {
            if (isPunctuator(Punctuator::Period) == false)
            {
                reportExpected("a designated initializer such as '.Name = value'");
                return node;
            }
            const uint32 field = createNode(SyntaxKind::FieldInitializer);
            ++mPosition;
            if (parseDeclaredName(field) == false || expect(Punctuator::Equal, "'=' after the field name") == false)
            {
                return node;
            }
            addChild(field, parseExpression());
            finish(field);
            addChild(node, field);
            if (consumePunctuator(Punctuator::Comma) == false)
            {
                break;
            }
        }
        if (mIsRecovering == false)
        {
            (void)expect(Punctuator::RightBrace, "',' or '}' in the initializer");
        }
        finish(node);
        return node;
    }

    [[nodiscard]] uint32 parsePostfix(uint32 base) noexcept
    {
        uint32 result = base;
        while (mIsFatal == false && mIsRecovering == false)
        {
            if (isPunctuator(Punctuator::Period) == true)
            {
                const uint32 node = createNode(SyntaxKind::MemberExpression);
                ++mPosition;
                addChild(node, result);
                if (parseDeclaredName(node) == false)
                {
                    return node;
                }
                finish(node);
                result = node;
            }
            else if (isPunctuator(Punctuator::ColonColon) == true)
            {
                const uint32 node = createNode(SyntaxKind::ScopeExpression);
                setToken(node, mPosition);
                ++mPosition;
                addChild(node, result);
                if (isName() == false)
                {
                    reportExpected("a name after '::'");
                    return node;
                }
                addChild(node, createTokenNode(SyntaxKind::Name));
                finish(node);
                result = node;
            }
            else if (isPunctuator(Punctuator::LeftBracket) == true)
            {
                const uint32 node = createNode(SyntaxKind::IndexExpression);
                ++mPosition;
                addChild(node, result);
                addChild(node, parseExpression());
                (void)expect(Punctuator::RightBracket, "']' after the index");
                finish(node);
                result = node;
            }
            else if (isPunctuator(Punctuator::LeftParenthesis) == true)
            {
                result = parseCall(result);
            }
            else
            {
                break;
            }
        }
        return result;
    }

    [[nodiscard]] uint32 parseCall(uint32 callee) noexcept
    {
        const uint32 node = createNode(SyntaxKind::CallExpression);
        ++mPosition;
        addChild(node, callee);
        if (consumePunctuator(Punctuator::RightParenthesis) == true)
        {
            finish(node);
            return node;
        }
        bool hasNamed = false;
        while (mIsFatal == false && mIsRecovering == false)
        {
            const uint32 argument = createNode(SyntaxKind::Argument);
            const bool isNamed = isName() == true && isPunctuator(Punctuator::Equal, 1) == true;
            if (isNamed == true)
            {
                setToken(argument, mPosition);
                mPosition += 2;
                hasNamed = true;
            }
            else if (hasNamed == true)
            {
                report(DiagnosticCode::PositionalAfterNamed,
                       ComposeMessage("positional arguments must come before named arguments"));
                return node;
            }
            if (argument != NO_NODE && (isWord("out") == true || isWord("inout") == true))
            {
                mTree.Nodes[argument].Direction =
                    isWord("out") == true ? ParameterDirection::Out : ParameterDirection::Inout;
                ++mPosition;
            }
            addChild(argument, parseExpression());
            finish(argument);
            addChild(node, argument);
            if (consumePunctuator(Punctuator::Comma) == true)
            {
                continue;
            }
            (void)expect(Punctuator::RightParenthesis, "',' or ')' after an argument");
            break;
        }
        finish(node);
        return node;
    }

    const PreprocessRequest& mRequest;
    const PreprocessResult& mInput;
    SyntaxTree& mTree;
    internal::DiagnosticSink mSink;
    Array<uint32> mLastChildren;
    usize mPosition = 0;
    usize mDepth = 0;
    bool mIsRecovering = false;
    bool mIsFatal = false;
    bool mHasSplitGreater = false;
};
} // namespace

Status Parse(const PreprocessRequest& request, const PreprocessResult& preprocessed, SyntaxTree& outTree) noexcept
{
    outTree = SyntaxTree{};
    Parser parser(request, preprocessed, outTree);
    return parser.Run();
}

std::string_view GetSyntaxKindName(SyntaxKind kind) noexcept
{
    switch (kind)
    {
        case SyntaxKind::TranslationUnit:
            return "translation-unit";
        case SyntaxKind::VariableDeclaration:
            return "variable-declaration";
        case SyntaxKind::FunctionDeclaration:
            return "function-declaration";
        case SyntaxKind::Parameter:
            return "parameter";
        case SyntaxKind::Semantic:
            return "semantic";
        case SyntaxKind::StructDeclaration:
            return "struct-declaration";
        case SyntaxKind::EnumDeclaration:
            return "enum-declaration";
        case SyntaxKind::Enumerator:
            return "enumerator";
        case SyntaxKind::ExternSlangBlock:
            return "extern-slang-block";
        case SyntaxKind::StaticAssert:
            return "static-assert";
        case SyntaxKind::Attribute:
            return "attribute";
        case SyntaxKind::Type:
            return "type";
        case SyntaxKind::ArraySize:
            return "array-size";
        case SyntaxKind::Block:
            return "block";
        case SyntaxKind::ExpressionStatement:
            return "expression-statement";
        case SyntaxKind::AssignmentStatement:
            return "assignment-statement";
        case SyntaxKind::IncrementStatement:
            return "increment-statement";
        case SyntaxKind::IfStatement:
            return "if-statement";
        case SyntaxKind::ForStatement:
            return "for-statement";
        case SyntaxKind::WhileStatement:
            return "while-statement";
        case SyntaxKind::DoStatement:
            return "do-statement";
        case SyntaxKind::SwitchStatement:
            return "switch-statement";
        case SyntaxKind::CaseClause:
            return "case-clause";
        case SyntaxKind::BreakStatement:
            return "break-statement";
        case SyntaxKind::ContinueStatement:
            return "continue-statement";
        case SyntaxKind::ReturnStatement:
            return "return-statement";
        case SyntaxKind::DiscardStatement:
            return "discard-statement";
        case SyntaxKind::FallthroughStatement:
            return "fallthrough-statement";
        case SyntaxKind::Name:
            return "name";
        case SyntaxKind::ScopeExpression:
            return "scope-expression";
        case SyntaxKind::NumberLiteral:
            return "number-literal";
        case SyntaxKind::BooleanLiteral:
            return "boolean-literal";
        case SyntaxKind::StringLiteral:
            return "string-literal";
        case SyntaxKind::ParenthesizedExpression:
            return "parenthesized-expression";
        case SyntaxKind::UnaryExpression:
            return "unary-expression";
        case SyntaxKind::BinaryExpression:
            return "binary-expression";
        case SyntaxKind::ConditionalExpression:
            return "conditional-expression";
        case SyntaxKind::CallExpression:
            return "call-expression";
        case SyntaxKind::Argument:
            return "argument";
        case SyntaxKind::MemberExpression:
            return "member-expression";
        case SyntaxKind::IndexExpression:
            return "index-expression";
        case SyntaxKind::InitializerList:
            return "initializer-list";
        case SyntaxKind::FieldInitializer:
            return "field-initializer";
        case SyntaxKind::VoidInitializer:
            return "void-initializer";
        case SyntaxKind::Empty:
            return "empty";
        case SyntaxKind::Error:
            return "error";
    }
    return {};
}
} // namespace ludus::tooling::shader_language
