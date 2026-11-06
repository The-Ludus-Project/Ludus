// Coordinate-space rules over the checked LUSL syntax tree
// (docs/architecture/shading-language.md#matrices-and-coordinate-spaces).
//
// A vector type may name the space its values are in (`float32x3<World>`), and a matrix type the
// spaces it maps between (`float32x4x4<World, Clip>`, source first, as in the `WorldToClip`
// naming rule). Spaces are erased by lowering; this pass only reports values of two different
// spaces meeting. A value without a space adopts the space of its context, so untagged code
// checks as before.
//
// Spaces propagate through names, fields, swizzles, array elements, unary and arithmetic
// operators, `?:`, constructions and conversions from one tagged vector, intrinsics whose result
// is a vector or matrix (`transpose` swaps the spaces), and function returns. `M * v` requires v
// in M's source space and yields M's destination space; `A * B` requires B's destination to be
// A's source and maps from B's source to A's destination.

#include "internal/analysis.hpp"

#include <ludus/foundation/base/core.h>

#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::uint8;

/// The spaces of a value: a vector's space is To; a matrix maps From to To. NO_SYMBOL when absent.
struct Spaces final
{
    uint32 From = NO_SYMBOL;
    uint32 To = NO_SYMBOL;

    [[nodiscard]] bool IsTagged() const noexcept
    {
        return To != NO_SYMBOL;
    }

    [[nodiscard]] bool IsMatrix() const noexcept
    {
        return From != NO_SYMBOL;
    }

    [[nodiscard]] bool IsSame(const Spaces& other) const noexcept
    {
        return From == other.From && To == other.To;
    }
};

// The analyzer passes syntax-node, token and symbol indices, separate uint32 index spaces whose
// parameter names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
class SpaceAnalyzer final
{
public:
    SpaceAnalyzer(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept : mInputs(inputs), mSink(inoutSink)
    {
    }

    [[nodiscard]] bool Run() noexcept
    {
        const SemanticModel& model = mInputs.Model;
        if (hasSpaces() == false)
        {
            return true;
        }
        if (mIsReported.TryResize(mInputs.Preprocessed.Tokens.GetSize(), 0) == false ||
            CollectFunctionBodies(mInputs.Tree, model, mBodies) == false ||
            MakeSpecializedModels(model, mSpecializedModels) == false)
        {
            return false;
        }
        for (const FunctionBody& body : mBodies)
        {
            mModelOrNull = body.Specialization == NO_SPECIALIZATION ? &model : &mSpecializedModels[body.Specialization];
            mFunction = body.Function;
            for (uint32 child = firstChild(body.Node); child != NO_NODE; child = nextSibling(child))
            {
                if (getKind(child) == SyntaxKind::Block)
                {
                    visitStatement(child);
                }
            }
        }
        return true;
    }

private:
    // ----- Tree and model access -----------------------------------------------------------------

    /// Modules that declare no space have nothing to check.
    [[nodiscard]] bool hasSpaces() const noexcept
    {
        for (const Symbol& symbol : mInputs.Model.Symbols)
        {
            if (symbol.Kind == SymbolKind::Space)
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

    [[nodiscard]] uint32 firstChild(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_NODE : mInputs.Tree.Nodes[node].FirstChild;
    }

    [[nodiscard]] uint32 nextSibling(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_NODE : mInputs.Tree.Nodes[node].NextSibling;
    }

    [[nodiscard]] SyntaxKind getKind(uint32 node) const noexcept
    {
        return node == NO_NODE ? SyntaxKind::Error : mInputs.Tree.Nodes[node].Kind;
    }

    [[nodiscard]] uint32 skipAttributes(uint32 child) const noexcept
    {
        while (child != NO_NODE && getKind(child) == SyntaxKind::Attribute)
        {
            child = nextSibling(child);
        }
        return child;
    }

    [[nodiscard]] std::string_view spell(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mInputs.Request, mInputs.Preprocessed, mInputs.Preprocessed.Tokens[token]);
    }

    [[nodiscard]] const SemanticModel& getModel() const noexcept
    {
        return *mModelOrNull;
    }

    [[nodiscard]] uint32 getSymbol(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_SYMBOL : getModel().NodeSymbols[node];
    }

    [[nodiscard]] TypeKind getTypeKind(uint32 node) const noexcept
    {
        const uint32 type = node == NO_NODE ? NO_TYPE : getModel().NodeTypes[node];
        return type < getModel().Types.GetSize() ? getModel().Types[type].Kind : TypeKind::Error;
    }

    /// True when expression `node` is a vector or matrix, whose value can carry spaces.
    [[nodiscard]] bool canCarry(uint32 node) const noexcept
    {
        const TypeKind kind = getTypeKind(node);
        return kind == TypeKind::Vector || kind == TypeKind::Matrix;
    }

    // ----- Declared spaces -----------------------------------------------------------------------

    /// The spaces Type node `type` names.
    [[nodiscard]] Spaces getTypeSpaces(uint32 type) const noexcept
    {
        if (getKind(type) != SyntaxKind::Type)
        {
            return Spaces{};
        }
        const uint32 first = firstChild(type);
        const uint32 second = nextSibling(first);
        const uint32 firstSpace = getSpaceSymbol(first);
        if (second == NO_NODE)
        {
            return Spaces{NO_SYMBOL, firstSpace};
        }
        const uint32 secondSpace = getSpaceSymbol(second);
        if (firstSpace == NO_SYMBOL || secondSpace == NO_SYMBOL)
        {
            return Spaces{};
        }
        return Spaces{firstSpace, secondSpace};
    }

    [[nodiscard]] uint32 getSpaceSymbol(uint32 argument) const noexcept
    {
        const uint32 symbol = argument == NO_NODE ? NO_SYMBOL : mInputs.Model.NodeSymbols[argument];
        return symbol != NO_SYMBOL && mInputs.Model.Symbols[symbol].Kind == SymbolKind::Space ? symbol : NO_SYMBOL;
    }

    /// The spaces a variable, parameter or field is declared with, or a function returns.
    [[nodiscard]] Spaces getDeclaredSpaces(uint32 symbol) const noexcept
    {
        if (symbol == NO_SYMBOL || symbol >= getModel().Symbols.GetSize())
        {
            return Spaces{};
        }
        const Symbol& declared = getModel().Symbols[symbol];
        if (declared.Declaration == NO_NODE)
        {
            return Spaces{};
        }
        switch (declared.Kind)
        {
            case SymbolKind::Variable:
            case SymbolKind::Parameter:
            case SymbolKind::Field:
            case SymbolKind::Function:
                return getTypeSpaces(skipAttributes(firstChild(declared.Declaration)));
            default:
                return Spaces{};
        }
    }

    // ----- Diagnostics ---------------------------------------------------------------------------

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
                     DiagnosticCode::SpaceMismatch,
                     spelled.Location,
                     expansion,
                     message,
                     DiagnosticFix{});
    }

    [[nodiscard]] std::string_view getSpaceName(uint32 space) const noexcept
    {
        return space == NO_SYMBOL ? std::string_view() : spell(mInputs.Model.Symbols[space].NameToken);
    }

    /// Append "World" for a vector's space or "World to Clip" for a matrix's.
    void appendSpaces(DiagnosticMessage& inoutMessage, const Spaces& spaces) const noexcept
    {
        if (spaces.IsMatrix() == true)
        {
            AppendPart(inoutMessage, getSpaceName(spaces.From));
            AppendPart(inoutMessage, " to ");
        }
        AppendPart(inoutMessage, getSpaceName(spaces.To));
    }

    /// Append the repair for a value in `found` where `expected` is required.
    void appendRepair(DiagnosticMessage& inoutMessage, const Spaces& expected, const Spaces& found) const noexcept
    {
        if (expected.IsMatrix() == true || found.IsMatrix() == true)
        {
            AppendPart(inoutMessage, "; use a matrix from ");
            appendSpaces(inoutMessage, expected);
            return;
        }
        AppendPart(inoutMessage, "; transform it with a matrix from ");
        AppendPart(inoutMessage, getSpaceName(found.To));
        AppendPart(inoutMessage, " to ");
        AppendPart(inoutMessage, getSpaceName(expected.To));
    }

    /// Report a value in `found` that meets `expected`, which `what` names (a target, a parameter).
    void requireSpaces(const Spaces& expected,
                       const Spaces& found,
                       uint32 token,
                       std::string_view what,
                       std::string_view name) noexcept
    {
        if (expected.IsTagged() == false || found.IsTagged() == false || expected.IsSame(found) == true)
        {
            return;
        }
        DiagnosticMessage message =
            ComposeMessage(what, " '", name, expected.IsMatrix() == true ? "' maps " : "' is in ");
        appendSpaces(message, expected);
        AppendPart(message, found.IsMatrix() == true ? ", but this value maps " : ", but this value is in ");
        appendSpaces(message, found);
        appendRepair(message, expected, found);
        report(token, message);
    }

    /// The common spaces of two operands of `operation`; reports when both carry different ones.
    [[nodiscard]] Spaces join(const Spaces& left, const Spaces& right, uint32 operation) noexcept
    {
        if (left.IsTagged() == false)
        {
            return right;
        }
        if (right.IsTagged() == true && left.IsSame(right) == false)
        {
            DiagnosticMessage message = ComposeMessage("'", spell(operation), "' combines a value in ");
            appendSpaces(message, left);
            AppendPart(message, " with a value in ");
            appendSpaces(message, right);
            AppendPart(message, "; transform one into the other's space first");
            report(operation, message);
        }
        return left;
    }

    // ----- Statements ----------------------------------------------------------------------------

    void visitStatement(uint32 node) noexcept
    {
        if (node == NO_NODE)
        {
            return;
        }
        switch (getKind(node))
        {
            case SyntaxKind::Block:
            case SyntaxKind::CaseClause:
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    visitStatement(child);
                }
                return;
            case SyntaxKind::VariableDeclaration:
                visitDeclaration(node);
                return;
            case SyntaxKind::ExpressionStatement:
                (void)evaluate(firstChild(node));
                return;
            case SyntaxKind::AssignmentStatement: {
                const uint32 target = firstChild(node);
                const uint32 value = nextSibling(target);
                const Spaces targetSpaces = evaluate(target);
                const Spaces valueSpaces = evaluate(value);
                // `v *= M` applies a matrix; only a matrix product changes spaces.
                if (getTypeKind(value) != TypeKind::Matrix || getTypeKind(target) == TypeKind::Matrix)
                {
                    requireSpaces(targetSpaces,
                                  valueSpaces,
                                  getNode(value).FirstToken,
                                  "target",
                                  spell(getNode(target).Token));
                }
                return;
            }
            case SyntaxKind::IncrementStatement:
                (void)evaluate(firstChild(node));
                return;
            case SyntaxKind::SwitchStatement: {
                const uint32 selector = skipAttributes(firstChild(node));
                (void)evaluate(selector);
                for (uint32 clause = nextSibling(selector); clause != NO_NODE; clause = nextSibling(clause))
                {
                    visitStatement(clause);
                }
                return;
            }
            case SyntaxKind::IfStatement:
                visitIf(node);
                return;
            case SyntaxKind::ForStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement:
                for (uint32 child = skipAttributes(firstChild(node)); child != NO_NODE; child = nextSibling(child))
                {
                    if (isStatement(child) == true)
                    {
                        visitStatement(child);
                    }
                    else
                    {
                        (void)evaluate(child);
                    }
                }
                return;
            case SyntaxKind::ReturnStatement:
                visitReturn(node);
                return;
            default:
                return;
        }
    }

    [[nodiscard]] bool isStatement(uint32 node) const noexcept
    {
        switch (getKind(node))
        {
            case SyntaxKind::Block:
            case SyntaxKind::VariableDeclaration:
            case SyntaxKind::ExpressionStatement:
            case SyntaxKind::AssignmentStatement:
            case SyntaxKind::IncrementStatement:
            case SyntaxKind::IfStatement:
            case SyntaxKind::ForStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement:
            case SyntaxKind::SwitchStatement:
            case SyntaxKind::BreakStatement:
            case SyntaxKind::ContinueStatement:
            case SyntaxKind::ReturnStatement:
            case SyntaxKind::DiscardStatement:
                return true;
            default:
                return false;
        }
    }

    void visitDeclaration(uint32 node) noexcept
    {
        uint32 child = skipAttributes(firstChild(node));
        const Spaces declared = getTypeSpaces(child);
        child = nextSibling(child);
        while (child != NO_NODE && getKind(child) == SyntaxKind::ArraySize)
        {
            child = nextSibling(child);
        }
        if (child == NO_NODE || getKind(child) == SyntaxKind::VoidInitializer)
        {
            return;
        }
        const Spaces value = evaluate(child);
        requireSpaces(declared, value, getNode(child).FirstToken, "variable", spell(getNode(node).Token));
    }

    void visitIf(uint32 node) noexcept
    {
        const uint32 condition = skipAttributes(firstChild(node));
        const uint32 whenTrue = nextSibling(condition);
        const uint32 whenFalse = nextSibling(whenTrue);
        if (getNode(node).IsConstexpr == true)
        {
            // Only the selected branch was checked.
            const bool isTrue = getModel().NodeIntegerConstants[node].Value != 0;
            visitStatement(isTrue == true ? whenTrue : whenFalse);
            return;
        }
        (void)evaluate(condition);
        visitStatement(whenTrue);
        visitStatement(whenFalse);
    }

    void visitReturn(uint32 node) noexcept
    {
        const uint32 value = firstChild(node);
        if (value == NO_NODE)
        {
            return;
        }
        const Spaces found = evaluate(value);
        requireSpaces(getDeclaredSpaces(mFunction),
                      found,
                      getNode(value).FirstToken,
                      "the result of",
                      spell(getModel().Symbols[mFunction].NameToken));
    }

    // ----- Expressions ---------------------------------------------------------------------------

    [[nodiscard]] Spaces evaluateChildren(uint32 node) noexcept
    {
        for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
        {
            (void)evaluate(child);
        }
        return Spaces{};
    }

    /// Keep `spaces` only when `node` is a vector or matrix; a vector keeps no source space.
    [[nodiscard]] Spaces carry(uint32 node, const Spaces& spaces) const noexcept
    {
        const TypeKind kind = getTypeKind(node);
        if (kind == TypeKind::Matrix)
        {
            return spaces;
        }
        if (kind == TypeKind::Vector && spaces.IsMatrix() == false)
        {
            return spaces;
        }
        return Spaces{};
    }

    [[nodiscard]] Spaces evaluate(uint32 node) noexcept
    {
        if (node == NO_NODE)
        {
            return Spaces{};
        }
        switch (getKind(node))
        {
            case SyntaxKind::Name:
                return getDeclaredSpaces(getSymbol(node));
            case SyntaxKind::MemberExpression: {
                const Spaces object = evaluate(firstChild(node));
                const uint32 symbol = getSymbol(node);
                if (symbol != NO_SYMBOL && getModel().Symbols[symbol].Kind == SymbolKind::Field)
                {
                    return getDeclaredSpaces(symbol);
                }
                // A swizzle keeps the vector's space.
                return carry(node, object);
            }
            case SyntaxKind::IndexExpression: {
                const uint32 object = firstChild(node);
                const Spaces spaces = evaluate(object);
                (void)evaluate(nextSibling(object));
                return getTypeKind(object) == TypeKind::Array ? spaces : Spaces{};
            }
            case SyntaxKind::ParenthesizedExpression:
            case SyntaxKind::UnaryExpression:
                return carry(node, evaluate(firstChild(node)));
            case SyntaxKind::BinaryExpression:
                return evaluateBinary(node);
            case SyntaxKind::ConditionalExpression: {
                const uint32 condition = firstChild(node);
                const uint32 whenTrue = nextSibling(condition);
                const uint32 whenFalse = nextSibling(whenTrue);
                (void)evaluate(condition);
                const Spaces trueSpaces = evaluate(whenTrue);
                return join(trueSpaces, evaluate(whenFalse), getNode(whenFalse).FirstToken);
            }
            case SyntaxKind::CallExpression:
                return evaluateCall(node);
            case SyntaxKind::InitializerList:
                for (uint32 field = firstChild(node); field != NO_NODE; field = nextSibling(field))
                {
                    const uint32 value = firstChild(field);
                    const Spaces found = evaluate(value);
                    requireSpaces(getDeclaredSpaces(getSymbol(field)),
                                  found,
                                  getNode(value).FirstToken,
                                  "field",
                                  spell(getNode(field).Token));
                }
                return Spaces{};
            default:
                return evaluateChildren(node);
        }
    }

    [[nodiscard]] bool isComparison(std::string_view operation) const noexcept
    {
        return operation == "==" || operation == "!=" || operation == "<" || operation == ">" || operation == "<=" ||
               operation == ">=";
    }

    [[nodiscard]] Spaces evaluateBinary(uint32 node) noexcept
    {
        const uint32 left = firstChild(node);
        const uint32 right = nextSibling(left);
        const Spaces leftSpaces = evaluate(left);
        const Spaces rightSpaces = evaluate(right);
        const uint32 operation = getNode(node).Token;
        const std::string_view spelling = spell(operation);
        const TypeKind leftKind = getTypeKind(left);
        const TypeKind rightKind = getTypeKind(right);
        if (spelling == "*" && leftKind == TypeKind::Matrix)
        {
            return applyMatrix(leftSpaces, rightSpaces, rightKind, operation);
        }
        if (spelling == "*" && rightKind == TypeKind::Matrix && leftKind == TypeKind::Vector)
        {
            // `v * M` is the row-vector product; its spaces are not modeled.
            return Spaces{};
        }
        if (spelling == "&&" || spelling == "||")
        {
            return Spaces{};
        }
        if (isComparison(spelling) == true)
        {
            (void)join(leftSpaces, rightSpaces, operation);
            return Spaces{};
        }
        if (leftKind == TypeKind::Scalar || leftKind == TypeKind::UntypedFloat || leftKind == TypeKind::UntypedInteger)
        {
            return carry(node, rightSpaces);
        }
        if (rightKind == TypeKind::Scalar || rightKind == TypeKind::UntypedFloat ||
            rightKind == TypeKind::UntypedInteger)
        {
            return carry(node, leftSpaces);
        }
        return carry(node, join(leftSpaces, rightSpaces, operation));
    }

    /// The spaces of `M * v` or `A * B` for a matrix `M` or `A` with spaces `matrix`.
    [[nodiscard]] Spaces
    applyMatrix(const Spaces& matrix, const Spaces& operand, TypeKind operandKind, uint32 operation) noexcept
    {
        if (operandKind != TypeKind::Vector && operandKind != TypeKind::Matrix)
        {
            // A scaled matrix keeps its spaces.
            return matrix;
        }
        if (matrix.IsMatrix() == false)
        {
            return Spaces{};
        }
        if (operand.IsTagged() == true && operand.To != matrix.From)
        {
            DiagnosticMessage message = ComposeMessage("'*' applies a matrix from ");
            appendSpaces(message, matrix);
            AppendPart(message, operandKind == TypeKind::Vector ? " to a vector in " : " after a matrix to ");
            AppendPart(message, getSpaceName(operand.To));
            AppendPart(message, "; use a matrix from ");
            AppendPart(message, getSpaceName(operand.To));
            AppendPart(message, ", or transform the operand to ");
            AppendPart(message, getSpaceName(matrix.From));
            AppendPart(message, " first");
            report(operation, message);
            // The product has no meaningful spaces; reporting them again would repeat this error.
            return Spaces{};
        }
        if (operandKind == TypeKind::Vector)
        {
            return Spaces{NO_SYMBOL, matrix.To};
        }
        return operand.IsMatrix() == true ? Spaces{operand.From, matrix.To} : Spaces{};
    }

    [[nodiscard]] Spaces evaluateCall(uint32 node) noexcept
    {
        const uint32 callee = firstChild(node);
        const CallKind kind = getModel().NodeCallKinds[node];
        if (getKind(callee) == SyntaxKind::MemberExpression)
        {
            (void)evaluate(firstChild(callee));
        }
        uint32 function = NO_SYMBOL;
        if (kind == CallKind::Function || kind == CallKind::Method)
        {
            const uint32 symbol = getSymbol(node);
            function = symbol == NO_SYMBOL
                           ? NO_SYMBOL
                           : ResolveSpecialization(mInputs.Model, symbol, getModel().NodeTypeArguments[node]);
        }
        Spaces joined;
        for (uint32 argument = nextSibling(callee); argument != NO_NODE; argument = nextSibling(argument))
        {
            const uint32 value = firstChild(argument);
            const Spaces found = evaluate(value);
            if (function != NO_SYMBOL)
            {
                checkArgument(function, argument, found);
            }
            else if (kind != CallKind::ResourceMethod)
            {
                joined = join(joined, found, getNode(value).FirstToken);
            }
        }
        switch (kind)
        {
            case CallKind::Function:
            case CallKind::Method:
                return function == NO_SYMBOL ? Spaces{} : carry(node, getDeclaredSpaces(function));
            case CallKind::Intrinsic:
                if (spell(getNode(callee).Token) == "transpose")
                {
                    return joined.IsMatrix() == true ? Spaces{joined.To, joined.From} : Spaces{};
                }
                return carry(node, joined);
            case CallKind::Construction:
            case CallKind::Conversion:
                return carry(node, joined);
            default:
                return Spaces{};
        }
    }

    void checkArgument(uint32 function, uint32 argument, const Spaces& found) noexcept
    {
        const uint32 position = getModel().NodeParameters[argument];
        const Symbol& declared = getModel().Symbols[function];
        if (position == NO_SYMBOL || position >= declared.MemberCount)
        {
            return;
        }
        const uint32 parameter = getModel().Members[declared.FirstMember + position];
        requireSpaces(getDeclaredSpaces(parameter),
                      found,
                      getNode(firstChild(argument)).FirstToken,
                      "parameter",
                      spell(getModel().Symbols[parameter].NameToken));
    }

    const AnalysisInputs& mInputs;
    DiagnosticSink& mSink;
    Array<FunctionBody> mBodies;
    Array<SemanticModel> mSpecializedModels;
    Array<uint8> mIsReported;
    const SemanticModel* mModelOrNull = nullptr;
    uint32 mFunction = NO_SYMBOL;
};
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

bool AnalyzeSpaces(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept
{
    SpaceAnalyzer analyzer(inputs, inoutSink);
    return analyzer.Run();
}
} // namespace ludus::tooling::shader_language::internal
