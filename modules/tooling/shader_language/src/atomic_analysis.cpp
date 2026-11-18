// Atomic storage rules over the checked LUSL syntax tree
// (docs/modules/tooling/shader-language.md#resources-and-bindings).
//
// The checker marks each LUSL `groupshared` variable and RWStructuredBuffer binding that an
// `Interlocked` call updates (Symbol::IsAtomic), and lowering declares its storage `Atomic<T>`.
// Plain reads and `=` stores of its elements lower to loads and stores; this pass rejects the
// uses that have no `Atomic<T>` spelling: compound assignments, `++`/`--`, `out`/`inout`
// arguments, and whole arrays or buffers used as values.

#include "internal/analysis.hpp"

#include <ludus/foundation/base/core.h>

#include "internal/type_table.hpp"

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::uint8;
using foundation::usize;

class AtomicAnalyzer final
{
public:
    AtomicAnalyzer(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept : mInputs(inputs), mSink(inoutSink)
    {
    }

    [[nodiscard]] bool Run() noexcept
    {
        if (hasAtomicStorage() == false)
        {
            return true;
        }
        const usize nodeCount = mInputs.Tree.Nodes.GetSize();
        if (mIsReported.TryResize(mInputs.Preprocessed.Tokens.GetSize(), 0) == false ||
            mIsObject.TryResize(nodeCount, 0) == false ||
            MakeSpecializedModels(mInputs.Model, mSpecializedModels) == false)
        {
            return false;
        }
        markObjects();
        analyze(mInputs.Model);
        for (const SemanticModel& specialized : mSpecializedModels)
        {
            analyze(specialized);
        }
        return true;
    }

private:
    [[nodiscard]] bool hasAtomicStorage() const noexcept
    {
        for (const Symbol& symbol : mInputs.Model.Symbols)
        {
            if (symbol.IsAtomic == true)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const SyntaxNode& getNode(uint32 node) const noexcept
    {
        return mInputs.Tree.Nodes[node];
    }

    [[nodiscard]] SyntaxKind getKind(uint32 node) const noexcept
    {
        return node == NO_NODE ? SyntaxKind::Error : mInputs.Tree.Nodes[node].Kind;
    }

    [[nodiscard]] uint32 firstChild(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_NODE : mInputs.Tree.Nodes[node].FirstChild;
    }

    [[nodiscard]] std::string_view spell(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mInputs.Request, mInputs.Preprocessed, mInputs.Preprocessed.Tokens[token]);
    }

    /// Mark the objects of index and member expressions, through parentheses: an indexed atomic
    /// array or buffer is an element access, not a whole value.
    void markObjects() noexcept
    {
        for (usize node = 0; node < mInputs.Tree.Nodes.GetSize(); ++node)
        {
            const SyntaxKind kind = mInputs.Tree.Nodes[node].Kind;
            if (kind != SyntaxKind::IndexExpression && kind != SyntaxKind::MemberExpression)
            {
                continue;
            }
            uint32 object = firstChild(static_cast<uint32>(node));
            while (object != NO_NODE)
            {
                mIsObject[object] = 1;
                object = getKind(object) == SyntaxKind::ParenthesizedExpression ? firstChild(object) : NO_NODE;
            }
        }
    }

    /// The atomic storage symbol an access path starts from, or NO_SYMBOL.
    [[nodiscard]] uint32 getAtomicRoot(const SemanticModel& model, uint32 node) const noexcept
    {
        uint32 current = node;
        while (getKind(current) == SyntaxKind::IndexExpression || getKind(current) == SyntaxKind::MemberExpression ||
               getKind(current) == SyntaxKind::ParenthesizedExpression)
        {
            current = firstChild(current);
        }
        if (getKind(current) != SyntaxKind::Name)
        {
            return NO_SYMBOL;
        }
        const uint32 symbol = model.NodeSymbols[current];
        return symbol != NO_SYMBOL && model.Symbols[symbol].IsAtomic == true ? symbol : NO_SYMBOL;
    }

    void analyze(const SemanticModel& model) noexcept
    {
        for (usize index = 0; index < mInputs.Tree.Nodes.GetSize(); ++index)
        {
            const uint32 node = static_cast<uint32>(index);
            const SyntaxNode& syntax = getNode(node);
            switch (syntax.Kind)
            {
                case SyntaxKind::AssignmentStatement: {
                    const uint32 root = getAtomicRoot(model, firstChild(node));
                    if (root != NO_SYMBOL && spell(syntax.Token) != "=")
                    {
                        reportUpdate(model, root, syntax.Token, spell(syntax.Token));
                    }
                    break;
                }
                case SyntaxKind::IncrementStatement: {
                    const uint32 root = getAtomicRoot(model, firstChild(node));
                    if (root != NO_SYMBOL)
                    {
                        reportUpdate(model, root, syntax.Token, spell(syntax.Token));
                    }
                    break;
                }
                case SyntaxKind::Argument: {
                    const bool isWritable =
                        syntax.Direction == ParameterDirection::Out || syntax.Direction == ParameterDirection::Inout;
                    const uint32 root = isWritable == true ? getAtomicRoot(model, firstChild(node)) : NO_SYMBOL;
                    if (root != NO_SYMBOL)
                    {
                        report(syntax.FirstToken,
                               ComposeMessage("'",
                                              spell(model.Symbols[root].NameToken),
                                              "' is atomic storage, which cannot be an 'out' or 'inout' argument; "
                                              "pass a local variable, then store it"));
                    }
                    break;
                }
                case SyntaxKind::Name:
                case SyntaxKind::IndexExpression:
                    checkWholeValue(model, node);
                    break;
                default:
                    break;
            }
        }
    }

    /// An atomic array or buffer that is not indexed would be copied or passed whole.
    void checkWholeValue(const SemanticModel& model, uint32 node) noexcept
    {
        if (mIsObject[node] != 0)
        {
            return;
        }
        const uint32 type = model.NodeTypes[node];
        if (type == NO_TYPE || type >= model.Types.GetSize())
        {
            return;
        }
        const TypeKind kind = model.Types[type].Kind;
        if (kind != TypeKind::Array && kind != TypeKind::Resource)
        {
            return;
        }
        const uint32 root = getAtomicRoot(model, node);
        if (root == NO_SYMBOL)
        {
            return;
        }
        report(getNode(node).FirstToken,
               ComposeMessage("'",
                              spell(model.Symbols[root].NameToken),
                              "' is atomic storage; use its elements, since a whole atomic array or buffer cannot "
                              "be copied or passed"));
    }

    void reportUpdate(const SemanticModel& model, uint32 root, uint32 token, std::string_view operation) noexcept
    {
        report(token,
               ComposeMessage("'",
                              spell(model.Symbols[root].NameToken),
                              "' is atomic storage, which '",
                              operation,
                              "' would update without an atomic operation; call InterlockedAdd or another "
                              "Interlocked operation"));
    }

    void report(uint32 token, const DiagnosticMessage& message) noexcept
    {
        if (token == NO_TOKEN || token >= mIsReported.GetSize() || mIsReported[token] != 0)
        {
            return;
        }
        // A specialized template reports once, not once per specialization.
        mIsReported[token] = 1;
        const Token& spelled = mInputs.Preprocessed.Tokens[token];
        SourceLocation expansion;
        if (spelled.ExpansionIndex != NO_EXPANSION &&
            spelled.ExpansionIndex < mInputs.Preprocessed.Expansions.GetSize())
        {
            expansion = mInputs.Preprocessed.Expansions[spelled.ExpansionIndex];
        }
        mSink.Report(Severity::Error,
                     DiagnosticCode::AtomicStorageAccess,
                     spelled.Location,
                     expansion,
                     message,
                     DiagnosticFix{});
    }

    const AnalysisInputs& mInputs;
    DiagnosticSink& mSink;
    Array<SemanticModel> mSpecializedModels;
    Array<uint8> mIsReported;
    Array<uint8> mIsObject;
};
} // namespace

bool AnalyzeAtomics(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept
{
    AtomicAnalyzer analyzer(inputs, inoutSink);
    return analyzer.Run();
}
} // namespace ludus::tooling::shader_language::internal
