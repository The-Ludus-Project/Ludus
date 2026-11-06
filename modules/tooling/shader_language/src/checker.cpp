// Semantic checker for L1 LUSL (docs/architecture/shading-language.md). Names resolve in C order.
// Untyped constants follow the Go Programming Language Specification, "Constants"
// (https://go.dev/ref/spec#Constants): a literal has no type until it meets a target, and converts
// only when the target represents its value. Named arguments bind like the Python Language
// Reference, "Calls" (https://docs.python.org/3/reference/expressions.html#calls): positional
// arguments first, then each parameter at most once by name.

#include <ludus/tooling/shader_language/semantic.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/access_path.hpp"
#include "internal/analysis.hpp"
#include "internal/call_binding.hpp"
#include "internal/diagnostic_sink.hpp"
#include "internal/fix_builder.hpp"
#include "internal/intrinsics.hpp"
#include "internal/scope_table.hpp"
#include "internal/slang_names.hpp"
#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::Array;
using foundation::int64;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
using internal::ComposeMessage;
using internal::ScopeTable;
using internal::TypeTable;

/// Largest int64, the range of untyped integer constants.
constexpr int64 INT64_MAXIMUM = 0x7fffffffffffffff;
/// Smallest int64.
constexpr int64 INT64_MINIMUM = -INT64_MAXIMUM - 1;
/// 2^24, the float32 significand: odd integers above it are not representable.
constexpr int64 FLOAT32_EXACT_INTEGER = int64{1} << 24;
/// 2^11, the float16 significand.
constexpr int64 FLOAT16_EXACT_INTEGER = int64{1} << 11;
/// 2^53, the float64 significand.
constexpr int64 FLOAT64_EXACT_INTEGER = int64{1} << 53;
/// Largest finite float16.
constexpr int64 FLOAT16_MAXIMUM = 65504;
/// Largest int16 and uint16.
constexpr int64 INT16_MAXIMUM = 0x7fff;
constexpr int64 UINT16_MAXIMUM = 0xffff;
/// Largest int32 and uint32.
constexpr int64 INT32_MAXIMUM = 0x7fffffff;
constexpr int64 UINT32_MAXIMUM = 0xffffffff;
/// Base of hexadecimal literals and the value of the digit `a`.
constexpr uint64 HEXADECIMAL_BASE = 16;
constexpr uint64 HEXADECIMAL_LETTER_VALUE = 10;
/// Components a swizzle may name.
constexpr usize MAX_SWIZZLE_COMPONENTS = 4;
/// Arguments of `[threads(x, y, z)]`.
constexpr usize THREAD_GROUP_DIMENSIONS = 3;

/// Inclusive range of an integer scalar kind; false for kinds whose range exceeds int64.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a minimum and maximum pair, in that order.
[[nodiscard]] bool GetIntegerRange(ScalarKind kind, int64& outMinimum, int64& outMaximum) noexcept
{
    switch (kind)
    {
        case ScalarKind::Int16:
            outMinimum = -INT16_MAXIMUM - 1;
            outMaximum = INT16_MAXIMUM;
            return true;
        case ScalarKind::Uint16:
            outMinimum = 0;
            outMaximum = UINT16_MAXIMUM;
            return true;
        case ScalarKind::Int32:
            outMinimum = -INT32_MAXIMUM - 1;
            outMaximum = INT32_MAXIMUM;
            return true;
        case ScalarKind::Uint32:
            outMinimum = 0;
            outMaximum = UINT32_MAXIMUM;
            return true;
        case ScalarKind::Int64:
            outMinimum = INT64_MINIMUM;
            outMaximum = INT64_MAXIMUM;
            return true;
        case ScalarKind::Uint64:
            outMinimum = 0;
            outMaximum = INT64_MAXIMUM;
            return true;
        default:
            return false;
    }
}

/// Bit mask of the components a swizzle names, or zero when it is not a swizzle of `size`.
[[nodiscard]] uint32 GetSwizzleMask(std::string_view name, uint32 size, bool& outHasRepeat) noexcept
{
    outHasRepeat = false;
    if (name.empty() == true || name.size() > MAX_SWIZZLE_COMPONENTS)
    {
        return 0;
    }
    constexpr std::string_view POSITION = "xyzw";
    constexpr std::string_view COLOR = "rgba";
    const bool isPosition = POSITION.find(name[0]) != std::string_view::npos;
    const std::string_view set = isPosition == true ? POSITION : COLOR;
    uint32 mask = 0;
    for (const char component : name)
    {
        const usize index = set.find(component);
        if (index == std::string_view::npos || index >= size)
        {
            return 0;
        }
        const uint32 bit = uint32{1} << index;
        outHasRepeat = outHasRepeat == true || (mask & bit) != 0;
        mask |= bit;
    }
    return mask;
}

// The checker passes syntax-node, type and symbol indices, three separate uint32 index spaces
// whose parameter names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)

/// Per-function checking state.
struct FunctionContext final
{
    uint32 Function = NO_SYMBOL;
    uint32 ReturnType = NO_TYPE;
    uint32 Struct = NO_SYMBOL;
    bool IsMutating = false;
    uint32 LoopDepth = 0;
    uint32 SwitchDepth = 0;
};

/// A writable argument, kept to check aliasing within one call.
struct WritableArgument final
{
    uint32 Node = NO_NODE;
    internal::AccessPath Path;
};

class Checker final
{
public:
    Checker(const PreprocessRequest& request,
            const PreprocessResult& preprocessed,
            const SyntaxTree& tree,
            const CheckOptions& options,
            SemanticModel& outModel) noexcept
        : mRequest(request), mInput(preprocessed), mTree(tree), mOptions(options), mModel(outModel),
          mSink(outModel.Diagnostics, outModel.DroppedDiagnosticCount), mTypes(outModel.Types)
    {
    }

    [[nodiscard]] Status Run() noexcept
    {
        if (mInput.Outcome != Status::Ready || mTree.Outcome != Status::Ready || mTree.Nodes.IsEmpty() == true)
        {
            mSink.ReportError(DiagnosticCode::InvalidRequest,
                              SourceLocation{},
                              SourceLocation{},
                              ComposeMessage("Check needs Ready preprocessing and parsing results"));
            mModel.Outcome = Status::InvalidRequest;
            return mModel.Outcome;
        }
        const usize nodeCount = mTree.Nodes.GetSize();
        if (mTypes.Initialize() == false || mScopes.Initialize() != internal::ScopeOutcome::Ok ||
            mModel.NodeTypes.TryResize(nodeCount, NO_TYPE) == false ||
            mModel.NodeSymbols.TryResize(nodeCount, NO_SYMBOL) == false ||
            mModel.NodeParameters.TryResize(nodeCount, NO_SYMBOL) == false ||
            mModel.NodeCallKinds.TryResize(nodeCount, CallKind::None) == false ||
            mModel.NodeIntegerConstants.TryResize(nodeCount) == false || declareIntrinsics() == false)
        {
            mSink.ReportOutOfMemory();
            return finish();
        }
        for (uint32 declaration = firstChild(0); declaration != NO_NODE && mIsFatal == false;
             declaration = nextSibling(declaration))
        {
            checkDeclaration(declaration, false);
        }
        reportUndefinedFunctions();
        // Flow and style rules read a complete, consistent model, so they run only after it checks.
        if (mSink.HasErrors() == false && mSink.HasOutOfMemory() == false && mIsFatal == false)
        {
            const internal::AnalysisInputs inputs{mRequest, mInput, mTree, mModel};
            if (internal::AnalyzeFlow(inputs, mSink) == false ||
                internal::CheckStyle(inputs, mOptions.Style, mSink) == false)
            {
                mSink.ReportOutOfMemory();
            }
        }
        return finish();
    }

private:
    // ----- Tree and token access -----------------------------------------------------------------

    [[nodiscard]] const SyntaxNode& getNode(uint32 node) const noexcept
    {
        return mTree.Nodes[node];
    }

    [[nodiscard]] uint32 firstChild(uint32 node) const noexcept
    {
        return mTree.Nodes[node].FirstChild;
    }

    [[nodiscard]] uint32 nextSibling(uint32 node) const noexcept
    {
        return mTree.Nodes[node].NextSibling;
    }

    [[nodiscard]] SyntaxKind getKind(uint32 node) const noexcept
    {
        return node == NO_NODE ? SyntaxKind::Error : mTree.Nodes[node].Kind;
    }

    [[nodiscard]] std::string_view spell(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInput.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mRequest, mInput, mInput.Tokens[token]);
    }

    [[nodiscard]] std::string_view spellNode(uint32 node) const noexcept
    {
        return spell(getNode(node).Token);
    }

    /// First child after the leading Attribute children.
    [[nodiscard]] uint32 skipAttributes(uint32 child) const noexcept
    {
        while (child != NO_NODE && getKind(child) == SyntaxKind::Attribute)
        {
            child = nextSibling(child);
        }
        return child;
    }

    // ----- Diagnostics ---------------------------------------------------------------------------

    void reportAtToken(DiagnosticCode code,
                       uint32 token,
                       const DiagnosticMessage& message,
                       const DiagnosticFix& fix = DiagnosticFix{}) noexcept
    {
        if (token == NO_TOKEN || token >= mInput.Tokens.GetSize())
        {
            mSink.Report(Severity::Error, code, SourceLocation{}, SourceLocation{}, message, fix);
            return;
        }
        const Token& spelled = mInput.Tokens[token];
        SourceLocation expansion;
        if (spelled.ExpansionIndex != NO_EXPANSION && spelled.ExpansionIndex < mInput.Expansions.GetSize())
        {
            expansion = mInput.Expansions[spelled.ExpansionIndex];
        }
        mSink.Report(Severity::Error, code, spelled.Location, expansion, message, fix);
    }

    void report(DiagnosticCode code,
                uint32 node,
                const DiagnosticMessage& message,
                const DiagnosticFix& fix = DiagnosticFix{}) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        reportAtToken(code, syntax.Token != NO_TOKEN ? syntax.Token : syntax.FirstToken, message, fix);
    }

    /// The fix that wraps expression `node` as `typeName(...)`.
    [[nodiscard]] DiagnosticFix makeWrapFix(uint32 node, std::string_view typeName) const noexcept
    {
        FixText opening;
        if (opening.TryAssign(typeName) != foundation::StringStatus::Ok ||
            opening.TryAppend("(") != foundation::StringStatus::Ok)
        {
            return DiagnosticFix{};
        }
        internal::FixBuilder fix(mRequest, mInput);
        fix.InsertBefore(getNode(node).FirstToken, opening.GetView());
        fix.InsertAfter(getNode(node).LastToken, ")");
        return fix.GetFix();
    }

    /// True when an explicit conversion from `found` to `expected` exists: both numeric with one shape.
    [[nodiscard]] bool hasExplicitConversion(uint32 expected, uint32 found) const noexcept
    {
        const SemanticType& to = mTypes.Get(expected);
        const SemanticType& from = mTypes.Get(found);
        const bool isNumericShape =
            (to.Kind == TypeKind::Scalar || to.Kind == TypeKind::Vector || to.Kind == TypeKind::Matrix) &&
            to.Scalar != ScalarKind::Bool;
        return isNumericShape == true && from.Kind == to.Kind && from.Rows == to.Rows && from.Columns == to.Columns &&
               from.Scalar != ScalarKind::Bool;
    }

    [[nodiscard]] TypeNameText nameOf(uint32 type) const noexcept
    {
        return FormatTypeName(mModel, type, mRequest, mInput);
    }

    void reportMismatch(uint32 node, uint32 expected, uint32 found) noexcept
    {
        const TypeNameText expectedName = nameOf(expected);
        const TypeNameText foundName = nameOf(found);
        const DiagnosticFix fix = hasExplicitConversion(expected, found) == true
                                      ? makeWrapFix(node, expectedName.GetView())
                                      : DiagnosticFix{};
        report(DiagnosticCode::TypeMismatch,
               node,
               ComposeMessage("expected ",
                              expectedName.GetView(),
                              ", found ",
                              foundName.GetView(),
                              "; LUSL has no implicit conversions, so write ",
                              expectedName.GetView(),
                              "(value)"),
               fix);
    }

    // ----- Symbols -------------------------------------------------------------------------------

    [[nodiscard]] uint32 createSymbol(SymbolKind kind, uint32 nameToken, uint32 declaration) noexcept
    {
        if (mModel.Symbols.GetSize() >= MAX_SYMBOLS)
        {
            reportAtToken(DiagnosticCode::CheckerLimit,
                          nameToken,
                          ComposeMessage("more than ", uint64{MAX_SYMBOLS}, " symbols"));
            mIsFatal = true;
            return NO_SYMBOL;
        }
        Symbol symbol;
        symbol.Kind = kind;
        symbol.NameToken = nameToken;
        symbol.Declaration = declaration;
        symbol.IsExtern = mIsInExtern;
        if (mModel.Symbols.TryAdd(symbol) == false || mNextOverloads.TryAdd(NO_SYMBOL) == false)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
            return NO_SYMBOL;
        }
        const uint32 index = static_cast<uint32>(mModel.Symbols.GetSize() - 1);
        if (declaration != NO_NODE)
        {
            mModel.NodeSymbols[declaration] = index;
        }
        // Extern declarations mirror Slang names, which are valid Slang by definition.
        if (mIsInExtern == false && nameToken != NO_TOKEN && internal::IsReservedSlangName(spell(nameToken)) == true)
        {
            reportAtToken(
                DiagnosticCode::ReservedName,
                nameToken,
                ComposeMessage("'", spell(nameToken), "' is reserved in the generated Slang; choose another name"));
        }
        return index;
    }

    [[nodiscard]] Symbol& getSymbol(uint32 symbol) noexcept
    {
        return mModel.Symbols[symbol];
    }

    [[nodiscard]] std::string_view getSymbolName(uint32 symbol) const noexcept
    {
        if (symbol < mIntrinsicCount)
        {
            return internal::INTRINSICS[symbol].Name;
        }
        return spell(mModel.Symbols[symbol].NameToken);
    }

    /// Bind `symbol` under its name; reports a redeclaration and returns false on conflict.
    [[nodiscard]] bool bindSymbol(uint32 symbol) noexcept
    {
        if (symbol == NO_SYMBOL)
        {
            return false;
        }
        uint32 previous = ScopeTable::UNBOUND;
        const internal::ScopeOutcome outcome = mScopes.TryBind(getSymbolName(symbol), symbol, previous);
        if (outcome != internal::ScopeOutcome::Ok)
        {
            failScope(outcome, mModel.Symbols[symbol].NameToken);
            return false;
        }
        if (previous != ScopeTable::UNBOUND)
        {
            reportAtToken(DiagnosticCode::Redeclaration,
                          mModel.Symbols[symbol].NameToken,
                          ComposeMessage("'", getSymbolName(symbol), "' is already declared in this scope"));
            return false;
        }
        return true;
    }

    void failScope(internal::ScopeOutcome outcome, uint32 token) noexcept
    {
        if (outcome == internal::ScopeOutcome::OutOfMemory)
        {
            mSink.ReportOutOfMemory();
        }
        else
        {
            reportAtToken(DiagnosticCode::CheckerLimit, token, ComposeMessage("too many distinct names"));
        }
        mIsFatal = true;
    }

    [[nodiscard]] bool enterScope() noexcept
    {
        if (mScopes.Enter() != internal::ScopeOutcome::Ok)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool addMember(uint32 symbol) noexcept
    {
        if (mModel.Members.TryAdd(symbol) == false)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool declareIntrinsics() noexcept
    {
        for (const internal::IntrinsicSpelling& intrinsic : internal::INTRINSICS)
        {
            (void)intrinsic;
            const uint32 symbol = createSymbol(SymbolKind::Intrinsic, NO_TOKEN, NO_NODE);
            if (symbol == NO_SYMBOL)
            {
                return false;
            }
            ++mIntrinsicCount;
            uint32 previous = ScopeTable::UNBOUND;
            if (mScopes.TryBind(getSymbolName(symbol), symbol, previous) != internal::ScopeOutcome::Ok)
            {
                return false;
            }
        }
        // User declarations live in a scope inside the built-ins, so they may reuse built-in names.
        return mScopes.Enter() == internal::ScopeOutcome::Ok;
    }

    [[nodiscard]] Status finish() noexcept
    {
        Status outcome = Status::Ready;
        if (mSink.HasOutOfMemory() == true)
        {
            outcome = Status::OutOfMemory;
        }
        else if (mSink.HasErrors() == true)
        {
            outcome = Status::Failed;
        }
        mModel.Outcome = outcome;
        return outcome;
    }

    // ----- Types ---------------------------------------------------------------------------------

    [[nodiscard]] const SemanticType& getType(uint32 type) const noexcept
    {
        return mTypes.Get(type);
    }

    [[nodiscard]] bool isError(uint32 type) const noexcept
    {
        return type == NO_TYPE || getType(type).Kind == TypeKind::Error;
    }

    [[nodiscard]] bool isBoolScalar(uint32 type) const noexcept
    {
        return getType(type).Kind == TypeKind::Scalar && getType(type).Scalar == ScalarKind::Bool;
    }

    [[nodiscard]] bool isUntyped(uint32 type) const noexcept
    {
        const TypeKind kind = getType(type).Kind;
        return kind == TypeKind::UntypedInteger || kind == TypeKind::UntypedFloat;
    }

    [[nodiscard]] bool isOpaque(uint32 type) const noexcept
    {
        const SemanticType& shape = getType(type);
        return shape.Kind == TypeKind::Struct && mModel.Symbols[shape.Symbol].IsOpaque == true;
    }

    [[nodiscard]] uint32 failType(internal::TypeOutcome outcome, uint32 node) noexcept
    {
        if (outcome == internal::TypeOutcome::OutOfMemory)
        {
            mSink.ReportOutOfMemory();
        }
        else
        {
            report(DiagnosticCode::CheckerLimit,
                   node,
                   ComposeMessage("more than ", uint64{MAX_SEMANTIC_TYPES}, " types"));
        }
        mIsFatal = true;
        return TypeTable::ERROR_TYPE;
    }

    /// Resolve a Type node to a type index; reports unknown names.
    [[nodiscard]] uint32 resolveType(uint32 node) noexcept
    {
        if (node == NO_NODE || getKind(node) != SyntaxKind::Type)
        {
            return TypeTable::ERROR_TYPE;
        }
        const std::string_view name = spellNode(node);
        uint32 type = NO_TYPE;
        if (name == "void")
        {
            type = TypeTable::VOID_TYPE;
        }
        else
        {
            type = mTypes.FindBuiltin(name);
        }
        if (type == NO_TYPE)
        {
            const uint32 symbol = mScopes.Find(name);
            if (symbol != ScopeTable::UNBOUND &&
                (mModel.Symbols[symbol].Kind == SymbolKind::Struct || mModel.Symbols[symbol].Kind == SymbolKind::Enum))
            {
                type = mModel.Symbols[symbol].Type;
                mModel.NodeSymbols[node] = symbol;
            }
        }
        if (type == NO_TYPE)
        {
            report(DiagnosticCode::UnknownType, node, ComposeMessage("'", name, "' is not a type"));
            type = TypeTable::ERROR_TYPE;
        }
        if (firstChild(node) != NO_NODE && isError(type) == false)
        {
            report(DiagnosticCode::UnsupportedConstruct,
                   node,
                   ComposeMessage("type arguments arrive with templates (L2) and the binding model (L3)"));
            type = TypeTable::ERROR_TYPE;
        }
        mModel.NodeTypes[node] = type;
        return type;
    }

    /// Apply the ArraySize children that follow `child` to `element`, outermost first.
    [[nodiscard]] uint32 applyArraySizes(uint32 element, uint32 child) noexcept
    {
        foundation::StaticArray<uint32, MAX_SYNTAX_DEPTH> counts{};
        usize dimensionCount = 0;
        for (; child != NO_NODE && getKind(child) == SyntaxKind::ArraySize; child = nextSibling(child))
        {
            const uint32 size = firstChild(child);
            uint32 count = 0;
            if (size != NO_NODE && getKind(size) == SyntaxKind::NumberLiteral)
            {
                report(DiagnosticCode::ArraySizeLiteral,
                       size,
                       ComposeMessage("name the array size with a static const instead of the literal '",
                                      spellNode(size),
                                      "'"));
                element = TypeTable::ERROR_TYPE;
            }
            else if (size != NO_NODE)
            {
                (void)checkExpression(size, NO_TYPE);
                const IntegerConstant value = mModel.NodeIntegerConstants[size];
                const SemanticType& sizeType = getType(mModel.NodeTypes[size]);
                const bool isInteger =
                    sizeType.Kind == TypeKind::UntypedInteger ||
                    (sizeType.Kind == TypeKind::Scalar && internal::IsIntegerScalar(sizeType.Scalar));
                if (isError(mModel.NodeTypes[size]) == true)
                {
                    element = TypeTable::ERROR_TYPE;
                }
                else if (isInteger == false || value.IsKnown == false)
                {
                    report(DiagnosticCode::NotConstant,
                           size,
                           ComposeMessage("an array size must be an integer constant"));
                    element = TypeTable::ERROR_TYPE;
                }
                else if (value.Value <= 0 || value.Value > static_cast<int64>(MAX_ARRAY_LENGTH))
                {
                    report(DiagnosticCode::InvalidArraySize,
                           size,
                           ComposeMessage("an array size must be 1 to ", uint64{MAX_ARRAY_LENGTH}));
                    element = TypeTable::ERROR_TYPE;
                }
                else
                {
                    count = static_cast<uint32>(value.Value);
                }
            }
            if (dimensionCount < counts.GetSize())
            {
                counts[dimensionCount] = count;
                ++dimensionCount;
            }
        }
        if (isError(element) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        uint32 type = element;
        for (usize i = dimensionCount; i > 0; --i)
        {
            uint32 array = NO_TYPE;
            const internal::TypeOutcome outcome = mTypes.TryGetArray(type, counts[i - 1], array);
            if (outcome != internal::TypeOutcome::Ok)
            {
                return failType(outcome, child == NO_NODE ? 0 : child);
            }
            type = array;
        }
        return type;
    }

    // ----- Declarations --------------------------------------------------------------------------

    void checkDeclaration(uint32 node, bool isMember) noexcept
    {
        switch (getKind(node))
        {
            case SyntaxKind::VariableDeclaration:
                checkGlobalVariable(node);
                return;
            case SyntaxKind::FunctionDeclaration:
                checkFunction(node, NO_SYMBOL);
                return;
            case SyntaxKind::StructDeclaration:
                checkStruct(node);
                return;
            case SyntaxKind::EnumDeclaration:
                checkEnum(node);
                return;
            case SyntaxKind::ExternSlangBlock:
                checkExternBlock(node);
                return;
            case SyntaxKind::StaticAssert:
                checkStaticAssert(node);
                return;
            default:
                (void)isMember;
                return;
        }
    }

    void checkExternBlock(uint32 node) noexcept
    {
        const bool wasInExtern = mIsInExtern;
        mIsInExtern = true;
        for (uint32 child = firstChild(node); child != NO_NODE && mIsFatal == false; child = nextSibling(child))
        {
            checkDeclaration(child, false);
        }
        mIsInExtern = wasInExtern;
    }

    void rejectAttributes(uint32 node, std::string_view what) noexcept
    {
        for (uint32 child = firstChild(node); child != NO_NODE && getKind(child) == SyntaxKind::Attribute;
             child = nextSibling(child))
        {
            report(DiagnosticCode::InvalidAttribute,
                   child,
                   ComposeMessage("'", spellNode(child), "' does not apply to ", what));
        }
    }

    void checkGlobalVariable(uint32 node) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        const bool isStaticConstant = syntax.IsStatic == true && syntax.IsConst == true;
        if (isStaticConstant == false && syntax.IsGroupshared == false && mIsInExtern == false)
        {
            report(DiagnosticCode::InvalidTopLevelVariable,
                   node,
                   ComposeMessage("top-level variables are 'static const' or 'groupshared'; resources come from "
                                  "render-graph bindings or 'extern slang' declarations"));
        }
        checkVariable(node, true);
    }

    /// Check a variable declaration and bind its name in the current scope.
    void checkVariable(uint32 node, bool isGlobal) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        rejectAttributes(node, "variables");
        const uint32 typeNode = skipAttributes(firstChild(node));
        uint32 type = applyArraySizes(resolveType(typeNode), nextSibling(typeNode));
        uint32 initializer = nextSibling(typeNode);
        while (initializer != NO_NODE && getKind(initializer) == SyntaxKind::ArraySize)
        {
            initializer = nextSibling(initializer);
        }
        if (type == TypeTable::VOID_TYPE)
        {
            report(DiagnosticCode::UnknownType, node, ComposeMessage("a variable cannot be void"));
            type = TypeTable::ERROR_TYPE;
        }
        if (syntax.IsGroupshared == true && isGlobal == false)
        {
            report(DiagnosticCode::InvalidTopLevelVariable,
                   node,
                   ComposeMessage("'groupshared' variables are declared at top level"));
        }
        const bool isConstant = syntax.IsConst == true;
        if (initializer == NO_NODE && isConstant == true && mIsInExtern == false)
        {
            report(DiagnosticCode::NotConstant, node, ComposeMessage("a const variable needs an initializer"));
        }
        if (initializer != NO_NODE && mIsInExtern == true)
        {
            report(DiagnosticCode::InvalidInitializer,
                   initializer,
                   ComposeMessage("'extern slang' declarations take no initializer; the Slang file defines it"));
            initializer = NO_NODE;
        }
        IntegerConstant value;
        if (initializer != NO_NODE && getKind(initializer) != SyntaxKind::VoidInitializer)
        {
            (void)checkExpression(initializer, type);
            (void)coerce(initializer, type, true);
            if (isConstant == true && isConstantExpression(initializer) == false)
            {
                report(DiagnosticCode::NotConstant,
                       initializer,
                       ComposeMessage("a const variable needs a constant initializer"));
            }
            value = mModel.NodeIntegerConstants[initializer];
        }
        const uint32 symbol = createSymbol(SymbolKind::Variable, syntax.Token, node);
        if (symbol == NO_SYMBOL)
        {
            return;
        }
        Symbol& declared = getSymbol(symbol);
        declared.Type = type;
        declared.IsConstant = isConstant;
        declared.IsGroupshared = syntax.IsGroupshared;
        if (isConstant == true && value.IsKnown == true)
        {
            declared.HasIntegerValue = true;
            declared.IntegerValue = value.Value;
        }
        mModel.NodeTypes[node] = type;
        (void)bindSymbol(symbol);
    }

    void checkStaticAssert(uint32 node) noexcept
    {
        const uint32 condition = firstChild(node);
        const uint32 message = condition == NO_NODE ? NO_NODE : nextSibling(condition);
        if (condition == NO_NODE)
        {
            return;
        }
        const uint32 type = checkExpression(condition, NO_TYPE);
        if (isError(type) == true)
        {
            return;
        }
        const IntegerConstant value = mModel.NodeIntegerConstants[condition];
        if (isBoolScalar(type) == false || value.IsKnown == false)
        {
            report(DiagnosticCode::NotConstant,
                   condition,
                   ComposeMessage("static_assert needs a constant bool condition"));
            return;
        }
        if (value.Value == 0)
        {
            const std::string_view text = message == NO_NODE ? std::string_view{} : spellNode(message);
            report(DiagnosticCode::StaticAssertFailed, condition, ComposeMessage("static_assert failed: ", text));
        }
    }

    void checkEnum(uint32 node) noexcept
    {
        rejectAttributes(node, "enums");
        const uint32 underlyingNode = firstChild(node);
        const uint32 underlying = resolveType(underlyingNode);
        ScalarKind kind = ScalarKind::Int32;
        if (isError(underlying) == false)
        {
            if (getType(underlying).Kind != TypeKind::Scalar ||
                internal::IsIntegerScalar(getType(underlying).Scalar) == false)
            {
                report(DiagnosticCode::TypeMismatch,
                       underlyingNode,
                       ComposeMessage("an enum's underlying type is an integer scalar"));
            }
            else
            {
                kind = getType(underlying).Scalar;
            }
        }
        const uint32 symbol = createSymbol(SymbolKind::Enum, getNode(node).Token, node);
        if (symbol == NO_SYMBOL)
        {
            return;
        }
        uint32 type = NO_TYPE;
        const internal::TypeOutcome outcome = mTypes.TryGetNamed(TypeKind::Enum, kind, symbol, type);
        if (outcome != internal::TypeOutcome::Ok)
        {
            (void)failType(outcome, node);
            return;
        }
        getSymbol(symbol).Type = type;
        mModel.NodeTypes[node] = type;
        if (bindSymbol(symbol) == false)
        {
            return;
        }
        getSymbol(symbol).FirstMember = static_cast<uint32>(mModel.Members.GetSize());
        int64 next = 0;
        int64 minimum = 0;
        int64 maximum = 0;
        (void)GetIntegerRange(kind, minimum, maximum);
        for (uint32 child = nextSibling(underlyingNode); child != NO_NODE && mIsFatal == false;
             child = nextSibling(child))
        {
            const uint32 value = firstChild(child);
            if (value != NO_NODE)
            {
                (void)checkExpression(value, NO_TYPE);
                const IntegerConstant constant = mModel.NodeIntegerConstants[value];
                if (constant.IsKnown == false || isUntyped(mModel.NodeTypes[value]) == false)
                {
                    report(DiagnosticCode::NotConstant,
                           value,
                           ComposeMessage("an enumerator value is an integer literal or constant expression"));
                }
                else
                {
                    next = constant.Value;
                }
            }
            if (next < minimum || next > maximum)
            {
                report(DiagnosticCode::ConstantOutOfRange,
                       child,
                       ComposeMessage("the enumerator value does not fit ", internal::GetScalarName(kind)));
            }
            for (uint32 i = 0; i < getSymbol(symbol).MemberCount; ++i)
            {
                const uint32 previous = mModel.Members[getSymbol(symbol).FirstMember + i];
                if (getSymbolName(previous) == spellNode(child))
                {
                    report(DiagnosticCode::Redeclaration,
                           child,
                           ComposeMessage("'", spellNode(child), "' is already an enumerator of this enum"));
                }
            }
            const uint32 enumerator = createSymbol(SymbolKind::Enumerator, getNode(child).Token, child);
            if (enumerator == NO_SYMBOL || addMember(enumerator) == false)
            {
                return;
            }
            Symbol& declared = getSymbol(enumerator);
            declared.Type = type;
            declared.Parent = symbol;
            declared.IsConstant = true;
            declared.HasIntegerValue = true;
            declared.IntegerValue = next;
            ++getSymbol(symbol).MemberCount;
            mModel.NodeTypes[child] = type;
            next = next < INT64_MAXIMUM ? next + 1 : next;
        }
    }

    void checkStruct(uint32 node) noexcept
    {
        rejectAttributes(node, "structs");
        const SyntaxNode& syntax = getNode(node);
        const bool isOpaqueDeclaration = spell(syntax.Token + 1) == ";";
        if (isOpaqueDeclaration == true && mIsInExtern == false)
        {
            report(DiagnosticCode::InvalidInitializer,
                   node,
                   ComposeMessage("a struct without a body declares an opaque 'extern slang' type"));
        }
        const uint32 symbol = createSymbol(SymbolKind::Struct, syntax.Token, node);
        if (symbol == NO_SYMBOL)
        {
            return;
        }
        uint32 type = NO_TYPE;
        const internal::TypeOutcome outcome = mTypes.TryGetNamed(TypeKind::Struct, ScalarKind::Bool, symbol, type);
        if (outcome != internal::TypeOutcome::Ok)
        {
            (void)failType(outcome, node);
            return;
        }
        getSymbol(symbol).Type = type;
        getSymbol(symbol).IsOpaque = isOpaqueDeclaration;
        mModel.NodeTypes[node] = type;
        if (bindSymbol(symbol) == false)
        {
            return;
        }
        // Pass 1: fields, so methods can read them; the struct is incomplete until it ends.
        mIncompleteStruct = symbol;
        getSymbol(symbol).FirstMember = static_cast<uint32>(mModel.Members.GetSize());
        Array<uint32> methods;
        for (uint32 member = skipAttributes(firstChild(node)); member != NO_NODE && mIsFatal == false;
             member = nextSibling(member))
        {
            if (getKind(member) == SyntaxKind::FunctionDeclaration)
            {
                if (methods.TryAdd(member) == false)
                {
                    mSink.ReportOutOfMemory();
                    mIsFatal = true;
                }
                continue;
            }
            if (getKind(member) == SyntaxKind::VariableDeclaration)
            {
                checkField(symbol, member);
            }
        }
        mIncompleteStruct = NO_SYMBOL;
        // A struct's members are contiguous in Members, but method parameters are appended while
        // signatures are checked, so reserve one slot per method after the fields now.
        mNextMethodSlot = static_cast<uint32>(mModel.Members.GetSize());
        if (mModel.Members.TryResize(mModel.Members.GetSize() + methods.GetSize(), NO_SYMBOL) == false)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
            return;
        }
        // Pass 2: method signatures, so any method may call any other.
        Array<uint32> methodSymbols;
        for (const uint32 method : methods)
        {
            if (methodSymbols.TryAdd(checkFunctionSignature(method, symbol)) == false)
            {
                mSink.ReportOutOfMemory();
                mIsFatal = true;
                return;
            }
        }
        // Pass 3: method bodies, with the struct's fields and methods in scope.
        for (usize i = 0; i < methods.GetSize(); ++i)
        {
            checkFunctionBody(methods[i], methodSymbols[i]);
        }
    }

    [[nodiscard]] uint32 findMember(uint32 structSymbol, std::string_view name, SymbolKind kind) const noexcept
    {
        const Symbol& declared = mModel.Symbols[structSymbol];
        for (uint32 i = 0; i < declared.MemberCount; ++i)
        {
            const uint32 member = mModel.Members[declared.FirstMember + i];
            if (mModel.Symbols[member].Kind == kind && getSymbolName(member) == name)
            {
                return member;
            }
        }
        return NO_SYMBOL;
    }

    void checkField(uint32 structSymbol, uint32 node) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        rejectAttributes(node, "fields");
        if (syntax.IsStatic == true || syntax.IsConst == true || syntax.IsGroupshared == true)
        {
            report(DiagnosticCode::UnsupportedConstruct,
                   node,
                   ComposeMessage("struct fields take no 'static', 'const' or 'groupshared'"));
        }
        const uint32 typeNode = firstChild(node);
        uint32 type = applyArraySizes(resolveType(typeNode), nextSibling(typeNode));
        if (isError(type) == false && getType(type).Kind == TypeKind::Struct &&
            getType(type).Symbol == mIncompleteStruct)
        {
            report(DiagnosticCode::TypeMismatch, typeNode, ComposeMessage("a struct cannot contain itself"));
            type = TypeTable::ERROR_TYPE;
        }
        uint32 initializer = nextSibling(typeNode);
        while (initializer != NO_NODE && getKind(initializer) == SyntaxKind::ArraySize)
        {
            initializer = nextSibling(initializer);
        }
        if (initializer != NO_NODE)
        {
            if (getKind(initializer) == SyntaxKind::VoidInitializer)
            {
                report(DiagnosticCode::InvalidInitializer, initializer, ComposeMessage("fields cannot be '= void'"));
            }
            else
            {
                (void)checkExpression(initializer, type);
                (void)coerce(initializer, type, true);
                if (isConstantExpression(initializer) == false)
                {
                    report(DiagnosticCode::NotConstant,
                           initializer,
                           ComposeMessage("a field default must be a constant expression"));
                }
            }
        }
        if (findMember(structSymbol, spell(syntax.Token), SymbolKind::Field) != NO_SYMBOL)
        {
            report(DiagnosticCode::Redeclaration,
                   node,
                   ComposeMessage("'", spell(syntax.Token), "' is already a field of this struct"));
        }
        const uint32 field = createSymbol(SymbolKind::Field, syntax.Token, node);
        if (field == NO_SYMBOL || addMember(field) == false)
        {
            return;
        }
        getSymbol(field).Type = type;
        getSymbol(field).Parent = structSymbol;
        getSymbol(field).HasDefault = initializer != NO_NODE;
        ++getSymbol(structSymbol).MemberCount;
        mModel.NodeTypes[node] = type;
    }

    // ----- Functions -----------------------------------------------------------------------------

    struct Signature final
    {
        uint32 ReturnType = NO_TYPE;
        bool IsEntry = false;
        bool IsMutating = false;
    };

    [[nodiscard]] Signature checkFunctionAttributes(uint32 node, uint32 structSymbol) noexcept
    {
        Signature signature;
        bool hasThreads = false;
        for (uint32 attribute = firstChild(node); attribute != NO_NODE && getKind(attribute) == SyntaxKind::Attribute;
             attribute = nextSibling(attribute))
        {
            const std::string_view name = spellNode(attribute);
            uint32 argumentCount = 0;
            for (uint32 argument = firstChild(attribute); argument != NO_NODE; argument = nextSibling(argument))
            {
                ++argumentCount;
            }
            if (name == "stage")
            {
                const uint32 argument = firstChild(attribute);
                if (argumentCount != 1 || getKind(argument) != SyntaxKind::Name || spellNode(argument) != "compute")
                {
                    report(DiagnosticCode::InvalidAttribute,
                           attribute,
                           ComposeMessage("L1 entry points are '[stage(compute)]'; raster stages arrive in L3"));
                }
                signature.IsEntry = true;
            }
            else if (name == "threads")
            {
                hasThreads = true;
                bool isValid = argumentCount == THREAD_GROUP_DIMENSIONS;
                for (uint32 argument = firstChild(attribute); argument != NO_NODE; argument = nextSibling(argument))
                {
                    (void)checkExpression(argument, NO_TYPE);
                    const IntegerConstant value = mModel.NodeIntegerConstants[argument];
                    isValid = isValid == true && value.IsKnown == true && value.Value > 0;
                }
                if (isValid == false)
                {
                    report(DiagnosticCode::InvalidAttribute,
                           attribute,
                           ComposeMessage("'[threads(x, y, z)]' takes three positive integer constants"));
                }
            }
            else if (name == "mutating")
            {
                signature.IsMutating = true;
                if (structSymbol == NO_SYMBOL || argumentCount != 0)
                {
                    report(DiagnosticCode::InvalidAttribute,
                           attribute,
                           ComposeMessage("'[mutating]' takes no arguments and applies to struct methods"));
                }
            }
            else if ((name == "noinline" || name == "forceinline") && argumentCount == 0)
            {
                continue;
            }
            else
            {
                report(DiagnosticCode::InvalidAttribute,
                       attribute,
                       ComposeMessage("'", name, "' is not a function attribute"));
            }
        }
        if (signature.IsEntry != hasThreads)
        {
            report(DiagnosticCode::InvalidAttribute,
                   node,
                   ComposeMessage("compute entry points need both '[stage(compute)]' and '[threads(x, y, z)]'"));
        }
        return signature;
    }

    /// True when two functions take the same parameter types; with `isDirectionCompared`, also
    /// the same directions.
    [[nodiscard]] bool areSameParameters(uint32 left, uint32 right, bool isDirectionCompared) const noexcept
    {
        const Symbol& a = mModel.Symbols[left];
        const Symbol& b = mModel.Symbols[right];
        if (a.MemberCount != b.MemberCount)
        {
            return false;
        }
        for (uint32 i = 0; i < a.MemberCount; ++i)
        {
            const Symbol& p = mModel.Symbols[mModel.Members[a.FirstMember + i]];
            const Symbol& q = mModel.Symbols[mModel.Members[b.FirstMember + i]];
            if (p.Type != q.Type || (isDirectionCompared == true && p.Direction != q.Direction))
            {
                return false;
            }
        }
        return true;
    }

    /// Check a function's signature and register it; returns its symbol, or NO_SYMBOL when it does
    /// not register (a redeclaration).
    [[nodiscard]] uint32 checkFunctionSignature(uint32 node, uint32 structSymbol) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        const Signature signature = checkFunctionAttributes(node, structSymbol);
        const uint32 returnNode = skipAttributes(firstChild(node));
        const uint32 returnType = resolveType(returnNode);
        const uint32 symbol = createSymbol(SymbolKind::Function, syntax.Token, node);
        if (symbol == NO_SYMBOL)
        {
            return NO_SYMBOL;
        }
        getSymbol(symbol).Type = returnType;
        getSymbol(symbol).Parent = structSymbol;
        getSymbol(symbol).IsMutating = signature.IsMutating;
        // Parameters are checked in their own scope to catch repeated names, then unbound until
        // the body is checked, so the function name binds in the enclosing scope.
        if (enterScope() == false)
        {
            return NO_SYMBOL;
        }
        getSymbol(symbol).FirstMember = static_cast<uint32>(mModel.Members.GetSize());
        uint32 body = NO_NODE;
        for (uint32 child = nextSibling(returnNode); child != NO_NODE && mIsFatal == false; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Block)
            {
                body = child;
                break;
            }
            checkParameter(symbol, child, signature.IsEntry);
        }
        mScopes.Leave();
        if (signature.IsEntry == true && returnType != TypeTable::VOID_TYPE)
        {
            report(DiagnosticCode::InvalidReturn, returnNode, ComposeMessage("compute entry points return void"));
        }
        getSymbol(symbol).IsDefined = body != NO_NODE;
        if (body != NO_NODE && mIsInExtern == true)
        {
            report(DiagnosticCode::InvalidInitializer,
                   body,
                   ComposeMessage("'extern slang' functions are declared without a body"));
        }
        if (body == NO_NODE && mIsInExtern == false && structSymbol != NO_SYMBOL)
        {
            report(DiagnosticCode::UndefinedFunction, node, ComposeMessage("methods are defined in their struct"));
        }
        return registerFunction(symbol, structSymbol);
    }

    /// Check the body of a function whose signature registered as `symbol`.
    void checkFunctionBody(uint32 node, uint32 symbol) noexcept
    {
        if (symbol == NO_SYMBOL || mIsInExtern == true)
        {
            return;
        }
        uint32 body = NO_NODE;
        for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Block)
            {
                body = child;
            }
        }
        if (body == NO_NODE)
        {
            return;
        }
        const Symbol& function = mModel.Symbols[symbol];
        FunctionContext context;
        context.Function = symbol;
        context.ReturnType = function.Type;
        context.Struct = function.Parent;
        context.IsMutating = function.IsMutating;
        const FunctionContext outer = mFunction;
        mFunction = context;
        // Fields and methods sit in a scope outside the parameters, which may shadow them.
        if (enterScope() == false)
        {
            return;
        }
        if (function.Parent != NO_SYMBOL)
        {
            bindStructMembers(function.Parent);
        }
        if (enterScope() == true)
        {
            // The parameters of this declaration; a definition after a declaration has its own.
            for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
            {
                if (getKind(child) == SyntaxKind::Parameter && mModel.NodeSymbols[child] != NO_SYMBOL)
                {
                    (void)bindSymbol(mModel.NodeSymbols[child]);
                }
            }
            checkBlockStatements(body);
            mScopes.Leave();
        }
        mScopes.Leave();
        mFunction = outer;
    }

    void checkFunction(uint32 node, uint32 structSymbol) noexcept
    {
        checkFunctionBody(node, checkFunctionSignature(node, structSymbol));
    }

    /// Bind the fields and methods of `structSymbol` in the current scope, for method bodies.
    void bindStructMembers(uint32 structSymbol) noexcept
    {
        const Symbol& declared = mModel.Symbols[structSymbol];
        for (uint32 i = 0; i < declared.MemberCount; ++i)
        {
            const uint32 member = mModel.Members[declared.FirstMember + i];
            uint32 previous = ScopeTable::UNBOUND;
            const internal::ScopeOutcome outcome = mScopes.TryBind(getSymbolName(member), member, previous);
            if (outcome != internal::ScopeOutcome::Ok)
            {
                failScope(outcome, mModel.Symbols[member].NameToken);
                return;
            }
        }
    }

    /// Register a function under its name: a new overload, or the definition of an earlier
    /// declaration with the same parameters. Returns the symbol calls bind to, or NO_SYMBOL for a
    /// redeclaration.
    [[nodiscard]] uint32 registerFunction(uint32 symbol, uint32 structSymbol) noexcept
    {
        const std::string_view name = getSymbolName(symbol);
        uint32 head = NO_SYMBOL;
        if (structSymbol != NO_SYMBOL)
        {
            head = findMember(structSymbol, name, SymbolKind::Function);
        }
        else
        {
            const uint32 bound = mScopes.Find(name);
            head = bound == ScopeTable::UNBOUND ? NO_SYMBOL : bound;
        }
        if (head != NO_SYMBOL && mModel.Symbols[head].Kind == SymbolKind::Function &&
            mModel.Symbols[head].Parent == structSymbol)
        {
            for (uint32 candidate = head; candidate != NO_SYMBOL; candidate = mNextOverloads[candidate])
            {
                if (areSameParameters(candidate, symbol, false) == false)
                {
                    continue;
                }
                // Overload choice ignores call-site marks, so such overloads could never be called.
                if (areSameParameters(candidate, symbol, true) == false)
                {
                    reportAtToken(DiagnosticCode::Redeclaration,
                                  getSymbol(symbol).NameToken,
                                  ComposeMessage("'",
                                                 name,
                                                 "' has an overload with these parameter types; overloads cannot "
                                                 "differ only in parameter directions"));
                    return NO_SYMBOL;
                }
                Symbol& earlier = getSymbol(candidate);
                if (earlier.IsDefined == false && getSymbol(symbol).IsDefined == true &&
                    earlier.Type == getSymbol(symbol).Type)
                {
                    // A definition after its declaration: calls bind to the definition.
                    earlier.IsDefined = true;
                    earlier.Declaration = getSymbol(symbol).Declaration;
                    earlier.FirstMember = getSymbol(symbol).FirstMember;
                    mModel.NodeSymbols[getSymbol(symbol).Declaration] = candidate;
                    return candidate;
                }
                reportAtToken(DiagnosticCode::Redeclaration,
                              getSymbol(symbol).NameToken,
                              ComposeMessage("'", name, "' is already declared with these parameters"));
                return NO_SYMBOL;
            }
            uint32 last = head;
            while (mNextOverloads[last] != NO_SYMBOL)
            {
                last = mNextOverloads[last];
            }
            mNextOverloads[last] = symbol;
            return symbol;
        }
        if (structSymbol != NO_SYMBOL)
        {
            if (findMember(structSymbol, name, SymbolKind::Field) != NO_SYMBOL)
            {
                reportAtToken(DiagnosticCode::Redeclaration,
                              getSymbol(symbol).NameToken,
                              ComposeMessage("'", name, "' is already a field of this struct"));
                return NO_SYMBOL;
            }
            // checkStruct reserved this method's slot after the fields.
            mModel.Members[mNextMethodSlot] = symbol;
            ++mNextMethodSlot;
            ++getSymbol(structSymbol).MemberCount;
            return symbol;
        }
        uint32 previous = ScopeTable::UNBOUND;
        const internal::ScopeOutcome outcome = mScopes.TryBind(name, symbol, previous);
        if (outcome != internal::ScopeOutcome::Ok)
        {
            failScope(outcome, getSymbol(symbol).NameToken);
            return NO_SYMBOL;
        }
        if (previous != ScopeTable::UNBOUND)
        {
            reportAtToken(DiagnosticCode::Redeclaration,
                          getSymbol(symbol).NameToken,
                          ComposeMessage("'", name, "' is already declared in this scope"));
            return NO_SYMBOL;
        }
        return symbol;
    }

    void checkParameter(uint32 function, uint32 node, bool isEntry) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        const uint32 typeNode = firstChild(node);
        uint32 type = applyArraySizes(resolveType(typeNode), nextSibling(typeNode));
        if (type == TypeTable::VOID_TYPE)
        {
            report(DiagnosticCode::UnknownType, typeNode, ComposeMessage("a parameter cannot be void"));
            type = TypeTable::ERROR_TYPE;
        }
        uint32 child = nextSibling(typeNode);
        while (child != NO_NODE && getKind(child) == SyntaxKind::ArraySize)
        {
            child = nextSibling(child);
        }
        uint32 semantic = NO_NODE;
        if (child != NO_NODE && getKind(child) == SyntaxKind::Semantic)
        {
            semantic = child;
            child = nextSibling(child);
        }
        checkSemantic(node, semantic, type, isEntry);
        const uint32 defaultValue = child;
        if (defaultValue != NO_NODE)
        {
            if (syntax.Direction == ParameterDirection::Out || syntax.Direction == ParameterDirection::Inout)
            {
                report(DiagnosticCode::DirectionMismatch,
                       defaultValue,
                       ComposeMessage("out and inout parameters cannot have defaults"));
            }
            (void)checkExpression(defaultValue, type);
            (void)coerce(defaultValue, type, false);
            if (isConstantExpression(defaultValue) == false)
            {
                report(DiagnosticCode::NotConstant,
                       defaultValue,
                       ComposeMessage("a parameter default must be a constant expression"));
            }
        }
        const uint32 symbol = createSymbol(SymbolKind::Parameter, syntax.Token, node);
        if (symbol == NO_SYMBOL || addMember(symbol) == false)
        {
            return;
        }
        Symbol& declared = getSymbol(symbol);
        declared.Type = type;
        declared.Direction = syntax.Direction;
        declared.Parent = function;
        declared.HasDefault = defaultValue != NO_NODE;
        ++getSymbol(function).MemberCount;
        mModel.NodeTypes[node] = type;
        (void)bindSymbol(symbol);
    }

    void checkSemantic(uint32 parameter, uint32 semantic, uint32 type, bool isEntry) noexcept
    {
        if (semantic == NO_NODE)
        {
            if (isEntry == true)
            {
                report(DiagnosticCode::InvalidSemantic,
                       parameter,
                       ComposeMessage("compute entry point parameters need a system-value semantic"));
            }
            return;
        }
        if (isEntry == false)
        {
            report(DiagnosticCode::InvalidSemantic,
                   semantic,
                   ComposeMessage("semantics apply to entry point parameters only"));
            return;
        }
        const std::string_view name = spellNode(semantic);
        uint32 expected = NO_TYPE;
        constexpr uint32 THREAD_ID_COMPONENTS = 3;
        if (name == "DispatchThreadId" || name == "GroupThreadId" || name == "GroupId")
        {
            expected = mTypes.GetVector(ScalarKind::Uint32, THREAD_ID_COMPONENTS);
        }
        else if (name == "GroupIndex")
        {
            expected = mTypes.GetScalar(ScalarKind::Uint32);
        }
        if (expected == NO_TYPE)
        {
            report(DiagnosticCode::InvalidSemantic,
                   semantic,
                   ComposeMessage("'",
                                  name,
                                  "' is not a compute semantic; use DispatchThreadId, GroupThreadId, GroupId or "
                                  "GroupIndex"));
            return;
        }
        if (isError(type) == false && type != expected)
        {
            const TypeNameText expectedName = nameOf(expected);
            report(DiagnosticCode::InvalidSemantic,
                   semantic,
                   ComposeMessage("'", name, "' has type ", expectedName.GetView()));
        }
        if (getNode(parameter).Direction == ParameterDirection::Out ||
            getNode(parameter).Direction == ParameterDirection::Inout)
        {
            report(DiagnosticCode::InvalidSemantic, semantic, ComposeMessage("system values are inputs"));
        }
    }

    void reportUndefinedFunctions() noexcept
    {
        for (uint32 symbol = mIntrinsicCount; symbol < mModel.Symbols.GetSize(); ++symbol)
        {
            const Symbol& declared = mModel.Symbols[symbol];
            if (declared.Kind == SymbolKind::Function && declared.IsDefined == false && declared.IsExtern == false &&
                declared.Parent == NO_SYMBOL && mModel.NodeSymbols[declared.Declaration] == symbol)
            {
                reportAtToken(DiagnosticCode::UndefinedFunction,
                              declared.NameToken,
                              ComposeMessage("'", getSymbolName(symbol), "' is declared but never defined"));
            }
        }
    }

    // ----- Statements ----------------------------------------------------------------------------

    void checkBlockStatements(uint32 block) noexcept
    {
        for (uint32 statement = firstChild(block); statement != NO_NODE && mIsFatal == false;
             statement = nextSibling(statement))
        {
            checkStatement(statement);
        }
    }

    void checkScopedStatement(uint32 statement) noexcept
    {
        if (enterScope() == false)
        {
            return;
        }
        checkStatement(statement);
        mScopes.Leave();
    }

    void checkStatementAttributes(uint32 node) noexcept
    {
        for (uint32 attribute = firstChild(node); attribute != NO_NODE && getKind(attribute) == SyntaxKind::Attribute;
             attribute = nextSibling(attribute))
        {
            const std::string_view name = spellNode(attribute);
            const bool isLoop = getKind(node) == SyntaxKind::ForStatement ||
                                getKind(node) == SyntaxKind::WhileStatement || getKind(node) == SyntaxKind::DoStatement;
            const bool isBranch =
                getKind(node) == SyntaxKind::IfStatement || getKind(node) == SyntaxKind::SwitchStatement;
            const bool isKnown = (isLoop == true && (name == "unroll" || name == "loop")) ||
                                 (isBranch == true && (name == "branch" || name == "flatten"));
            if (isKnown == false)
            {
                report(DiagnosticCode::InvalidAttribute,
                       attribute,
                       ComposeMessage("'",
                                      name,
                                      "' does not apply here; loops take unroll or loop, branches take "
                                      "branch or flatten"));
                continue;
            }
            for (uint32 argument = firstChild(attribute); argument != NO_NODE; argument = nextSibling(argument))
            {
                (void)checkExpression(argument, NO_TYPE);
                const IntegerConstant value = mModel.NodeIntegerConstants[argument];
                if (name != "unroll" || nextSibling(argument) != NO_NODE || value.IsKnown == false || value.Value <= 0)
                {
                    report(DiagnosticCode::InvalidAttribute,
                           attribute,
                           ComposeMessage("only '[unroll(count)]' takes an argument, a positive integer constant"));
                    break;
                }
            }
        }
    }

    void requireCondition(uint32 condition) noexcept
    {
        if (condition == NO_NODE || getKind(condition) == SyntaxKind::Empty)
        {
            return;
        }
        const uint32 type = checkExpression(condition, NO_TYPE);
        if (isError(type) == false && isBoolScalar(type) == false)
        {
            const TypeNameText found = nameOf(type);
            report(DiagnosticCode::NonBoolCondition,
                   condition,
                   ComposeMessage("conditions are bool, not ",
                                  found.GetView(),
                                  "; compare explicitly, for example 'value != 0'"),
                   makeNotZeroFix(condition, type));
        }
    }

    /// The fix that appends ` != 0` to a numeric scalar condition whose operators bind tighter than `!=`.
    [[nodiscard]] DiagnosticFix makeNotZeroFix(uint32 condition, uint32 type) const noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Kind != TypeKind::Scalar && isUntyped(type) == false)
        {
            return DiagnosticFix{};
        }
        const SyntaxKind kind = getKind(condition);
        bool isTight = kind == SyntaxKind::Name || kind == SyntaxKind::MemberExpression ||
                       kind == SyntaxKind::CallExpression || kind == SyntaxKind::IndexExpression ||
                       kind == SyntaxKind::ParenthesizedExpression || kind == SyntaxKind::UnaryExpression ||
                       kind == SyntaxKind::NumberLiteral;
        if (kind == SyntaxKind::BinaryExpression)
        {
            const std::string_view operation = spellNode(condition);
            isTight = operation == "+" || operation == "-" || operation == "*" || operation == "/" ||
                      operation == "%" || operation == "<<" || operation == ">>";
        }
        if (isTight == false)
        {
            return DiagnosticFix{};
        }
        internal::FixBuilder fix(mRequest, mInput);
        fix.InsertAfter(getNode(condition).LastToken, " != 0");
        return fix.GetFix();
    }

    void checkStatement(uint32 node) noexcept
    {
        switch (getKind(node))
        {
            case SyntaxKind::Block:
                if (enterScope() == true)
                {
                    checkBlockStatements(node);
                    mScopes.Leave();
                }
                return;
            case SyntaxKind::VariableDeclaration:
                checkVariable(node, false);
                return;
            case SyntaxKind::ExpressionStatement:
                (void)checkExpression(firstChild(node), NO_TYPE);
                return;
            case SyntaxKind::AssignmentStatement:
                checkAssignment(node);
                return;
            case SyntaxKind::IncrementStatement:
                checkIncrement(node);
                return;
            case SyntaxKind::IfStatement: {
                checkStatementAttributes(node);
                const uint32 condition = skipAttributes(firstChild(node));
                requireCondition(condition);
                const uint32 whenTrue = condition == NO_NODE ? NO_NODE : nextSibling(condition);
                if (whenTrue != NO_NODE)
                {
                    checkScopedStatement(whenTrue);
                    if (nextSibling(whenTrue) != NO_NODE)
                    {
                        checkScopedStatement(nextSibling(whenTrue));
                    }
                }
                return;
            }
            case SyntaxKind::ForStatement:
                checkFor(node);
                return;
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement: {
                checkStatementAttributes(node);
                uint32 first = skipAttributes(firstChild(node));
                const uint32 second = first == NO_NODE ? NO_NODE : nextSibling(first);
                const bool isWhile = getKind(node) == SyntaxKind::WhileStatement;
                const uint32 condition = isWhile == true ? first : second;
                const uint32 body = isWhile == true ? second : first;
                requireCondition(condition);
                ++mFunction.LoopDepth;
                if (body != NO_NODE)
                {
                    checkScopedStatement(body);
                }
                --mFunction.LoopDepth;
                return;
            }
            case SyntaxKind::SwitchStatement:
                checkSwitch(node);
                return;
            case SyntaxKind::BreakStatement:
                if (mFunction.LoopDepth == 0 && mFunction.SwitchDepth == 0)
                {
                    report(DiagnosticCode::InvalidReturn,
                           node,
                           ComposeMessage("'break' is only valid in a loop or switch"));
                }
                return;
            case SyntaxKind::ContinueStatement:
                if (mFunction.LoopDepth == 0)
                {
                    report(DiagnosticCode::InvalidReturn, node, ComposeMessage("'continue' is only valid in a loop"));
                }
                return;
            case SyntaxKind::ReturnStatement:
                checkReturn(node);
                return;
            case SyntaxKind::StaticAssert:
                checkStaticAssert(node);
                return;
            case SyntaxKind::FallthroughStatement:
                if (mFunction.SwitchDepth == 0)
                {
                    report(DiagnosticCode::InvalidSwitch, node, ComposeMessage("'[[fallthrough]]' ends a case clause"));
                }
                return;
            default:
                return;
        }
    }

    void checkFor(uint32 node) noexcept
    {
        checkStatementAttributes(node);
        if (enterScope() == false)
        {
            return;
        }
        const uint32 initializer = skipAttributes(firstChild(node));
        const uint32 condition = initializer == NO_NODE ? NO_NODE : nextSibling(initializer);
        const uint32 increment = condition == NO_NODE ? NO_NODE : nextSibling(condition);
        const uint32 body = increment == NO_NODE ? NO_NODE : nextSibling(increment);
        if (initializer != NO_NODE && getKind(initializer) != SyntaxKind::Empty)
        {
            checkStatement(initializer);
        }
        requireCondition(condition);
        if (increment != NO_NODE && getKind(increment) != SyntaxKind::Empty)
        {
            checkStatement(increment);
        }
        ++mFunction.LoopDepth;
        if (body != NO_NODE)
        {
            checkScopedStatement(body);
        }
        --mFunction.LoopDepth;
        mScopes.Leave();
    }

    void checkSwitch(uint32 node) noexcept
    {
        checkStatementAttributes(node);
        const uint32 selector = skipAttributes(firstChild(node));
        if (selector == NO_NODE)
        {
            return;
        }
        const uint32 type = checkExpression(selector, NO_TYPE);
        const SemanticType& shape = getType(type);
        const bool isInteger = shape.Kind == TypeKind::Enum ||
                               (shape.Kind == TypeKind::Scalar && internal::IsIntegerScalar(shape.Scalar) == true);
        if (isError(type) == false && isInteger == false)
        {
            report(DiagnosticCode::InvalidSwitch,
                   selector,
                   ComposeMessage("a switch selector is an integer or an enum"));
        }
        Array<int64> labels;
        ++mFunction.SwitchDepth;
        for (uint32 clause = nextSibling(selector); clause != NO_NODE && mIsFatal == false;
             clause = nextSibling(clause))
        {
            uint32 statement = firstChild(clause);
            if (spellNode(clause) == "case" && statement != NO_NODE)
            {
                const uint32 label = statement;
                statement = nextSibling(statement);
                (void)checkExpression(label, type);
                (void)coerce(label, type, false);
                const IntegerConstant value = mModel.NodeIntegerConstants[label];
                if (value.IsKnown == false)
                {
                    report(DiagnosticCode::InvalidSwitch, label, ComposeMessage("case labels are constants"));
                }
                else
                {
                    for (const int64 previous : labels)
                    {
                        if (previous == value.Value)
                        {
                            report(DiagnosticCode::InvalidSwitch, label, ComposeMessage("this case label repeats"));
                        }
                    }
                    if (labels.TryAdd(value.Value) == false)
                    {
                        mSink.ReportOutOfMemory();
                        mIsFatal = true;
                    }
                }
            }
            if (enterScope() == false)
            {
                break;
            }
            for (; statement != NO_NODE; statement = nextSibling(statement))
            {
                if (getKind(statement) == SyntaxKind::FallthroughStatement && nextSibling(statement) != NO_NODE)
                {
                    report(DiagnosticCode::InvalidSwitch,
                           statement,
                           ComposeMessage("'[[fallthrough]]' must be the last statement of its case"));
                }
                checkStatement(statement);
            }
            mScopes.Leave();
        }
        --mFunction.SwitchDepth;
    }

    void checkReturn(uint32 node) noexcept
    {
        const uint32 value = firstChild(node);
        if (mFunction.Function == NO_SYMBOL)
        {
            return;
        }
        if (value == NO_NODE)
        {
            if (mFunction.ReturnType != TypeTable::VOID_TYPE && isError(mFunction.ReturnType) == false)
            {
                const TypeNameText expected = nameOf(mFunction.ReturnType);
                report(DiagnosticCode::InvalidReturn,
                       node,
                       ComposeMessage("this function returns ", expected.GetView(), "; return a value"));
            }
            return;
        }
        if (mFunction.ReturnType == TypeTable::VOID_TYPE)
        {
            report(DiagnosticCode::InvalidReturn, value, ComposeMessage("a void function returns no value"));
            (void)checkExpression(value, NO_TYPE);
            return;
        }
        (void)checkExpression(value, mFunction.ReturnType);
        (void)coerce(value, mFunction.ReturnType, false);
    }

    void checkAssignment(uint32 node) noexcept
    {
        const uint32 target = firstChild(node);
        const uint32 value = target == NO_NODE ? NO_NODE : nextSibling(target);
        if (target == NO_NODE || value == NO_NODE)
        {
            return;
        }
        const uint32 targetType = checkExpression(target, NO_TYPE);
        (void)requireAssignable(target);
        const std::string_view operation = spellNode(node);
        (void)checkExpression(value, targetType);
        if (operation == "=")
        {
            (void)coerce(value, targetType, true);
            return;
        }
        // `a op= b` checks as `a = a op b`.
        const std::string_view binary = operation.substr(0, operation.size() - 1);
        const uint32 result = typeBinary(node, binary, target, value);
        if (isError(result) == false && isError(targetType) == false && result != targetType)
        {
            reportMismatch(value, targetType, result);
        }
    }

    void checkIncrement(uint32 node) noexcept
    {
        const uint32 target = firstChild(node);
        if (target == NO_NODE)
        {
            return;
        }
        const uint32 type = checkExpression(target, NO_TYPE);
        (void)requireAssignable(target);
        const SemanticType& shape = getType(type);
        if (isError(type) == false && (shape.Kind != TypeKind::Scalar || shape.Scalar == ScalarKind::Bool))
        {
            const TypeNameText found = nameOf(type);
            report(DiagnosticCode::InvalidOperands,
                   node,
                   ComposeMessage("'", spellNode(node), "' needs a numeric scalar, not ", found.GetView()));
        }
    }

    // ----- Expressions ---------------------------------------------------------------------------

    void setConstant(uint32 node, int64 value) noexcept
    {
        mModel.NodeIntegerConstants[node] = IntegerConstant{true, value};
    }

    [[nodiscard]] IntegerConstant getConstant(uint32 node) const noexcept
    {
        return node == NO_NODE ? IntegerConstant{} : mModel.NodeIntegerConstants[node];
    }

    [[nodiscard]] uint32 setType(uint32 node, uint32 type) noexcept
    {
        mModel.NodeTypes[node] = type;
        return type;
    }

    /// Check `node` and record its type. `expected` types designated initializers only.
    [[nodiscard]] uint32 checkExpression(uint32 node, uint32 expected) noexcept
    {
        if (node == NO_NODE || mIsFatal == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        switch (getKind(node))
        {
            case SyntaxKind::Name:
                return setType(node, checkName(node));
            case SyntaxKind::ScopeExpression:
                return setType(node, checkScope(node));
            case SyntaxKind::NumberLiteral:
                return setType(node, checkNumber(node));
            case SyntaxKind::BooleanLiteral:
                setConstant(node, spellNode(node) == "true" ? 1 : 0);
                return setType(node, mTypes.GetScalar(ScalarKind::Bool));
            case SyntaxKind::StringLiteral:
                report(DiagnosticCode::TypeMismatch,
                       node,
                       ComposeMessage("strings appear only in static_assert messages and attributes"));
                return setType(node, TypeTable::ERROR_TYPE);
            case SyntaxKind::ParenthesizedExpression: {
                const uint32 inner = firstChild(node);
                const uint32 type = checkExpression(inner, expected);
                mModel.NodeIntegerConstants[node] = getConstant(inner);
                return setType(node, type);
            }
            case SyntaxKind::UnaryExpression:
                return setType(node, checkUnary(node));
            case SyntaxKind::BinaryExpression: {
                const uint32 left = firstChild(node);
                const uint32 right = left == NO_NODE ? NO_NODE : nextSibling(left);
                (void)checkExpression(left, NO_TYPE);
                (void)checkExpression(right, NO_TYPE);
                return setType(node, typeBinary(node, spellNode(node), left, right));
            }
            case SyntaxKind::ConditionalExpression:
                return setType(node, checkConditional(node, expected));
            case SyntaxKind::CallExpression:
                return setType(node, checkCall(node, expected));
            case SyntaxKind::MemberExpression:
                return setType(node, checkMember(node));
            case SyntaxKind::IndexExpression:
                return setType(node, checkIndex(node));
            case SyntaxKind::InitializerList:
                return setType(node, checkInitializerList(node, expected));
            default:
                return setType(node, TypeTable::ERROR_TYPE);
        }
    }

    [[nodiscard]] bool isZeroLiteral(uint32 node) const noexcept
    {
        return getKind(node) == SyntaxKind::NumberLiteral && spellNode(node) == "0";
    }

    /// Check that `node`'s value may initialize `target`, adapting untyped constants.
    [[nodiscard]] bool coerce(uint32 node, uint32 target, bool isZeroAllowed) noexcept
    {
        if (node == NO_NODE || target == NO_TYPE)
        {
            return true;
        }
        const uint32 source = mModel.NodeTypes[node];
        if (isError(source) == true || isError(target) == true || source == target)
        {
            return true;
        }
        if (source == TypeTable::VOID_TYPE)
        {
            report(DiagnosticCode::VoidValue, node, ComposeMessage("this call returns no value"));
            return false;
        }
        if (isZeroAllowed == true && isZeroLiteral(node) == true && target != TypeTable::VOID_TYPE)
        {
            // The zero rule: the literal 0 zero-fills any type that has a zero value.
            if (isOpaque(target) == true)
            {
                report(DiagnosticCode::InvalidZeroInitializer,
                       node,
                       ComposeMessage("an opaque 'extern slang' type has no zero value"));
                return false;
            }
            (void)setType(node, target);
            return true;
        }
        const SemanticType& goal = getType(target);
        const TypeKind kind = getType(source).Kind;
        if (kind == TypeKind::UntypedInteger || kind == TypeKind::UntypedFloat)
        {
            if (goal.Kind != TypeKind::Scalar)
            {
                const TypeNameText goalName = nameOf(target);
                if (goal.Kind == TypeKind::Vector || goal.Kind == TypeKind::Matrix)
                {
                    report(DiagnosticCode::TypeMismatch,
                           node,
                           ComposeMessage("a constant does not spread into ",
                                          goalName.GetView(),
                                          " implicitly; write ",
                                          goalName.GetView(),
                                          "(value)"),
                           makeWrapFix(node, goalName.GetView()));
                    return false;
                }
                reportMismatch(node, target, source);
                return false;
            }
            if (goal.Scalar == ScalarKind::Bool)
            {
                reportMismatch(node, target, source);
                return false;
            }
            if (kind == TypeKind::UntypedFloat && internal::IsFloatScalar(goal.Scalar) == false)
            {
                const TypeNameText goalName = nameOf(target);
                report(DiagnosticCode::TypeMismatch,
                       node,
                       ComposeMessage("a floating constant does not become ",
                                      goalName.GetView(),
                                      "; write an integer constant"));
                return false;
            }
            const IntegerConstant value = getConstant(node);
            if (kind == TypeKind::UntypedInteger && value.IsKnown == true &&
                fitsScalar(value.Value, goal.Scalar) == false)
            {
                const TypeNameText goalName = nameOf(target);
                report(DiagnosticCode::ConstantOutOfRange,
                       node,
                       ComposeMessage("the constant does not fit ", goalName.GetView(), " exactly"));
                return false;
            }
            (void)setType(node, target);
            return true;
        }
        reportMismatch(node, target, source);
        return false;
    }

    /// True when `scalar` represents the integer `value` exactly.
    [[nodiscard]] static bool fitsScalar(int64 value, ScalarKind scalar) noexcept
    {
        int64 minimum = 0;
        int64 maximum = 0;
        if (GetIntegerRange(scalar, minimum, maximum) == true)
        {
            return value >= minimum && value <= maximum;
        }
        // Exactly representable: the odd part of |value| fits the significand.
        int64 significandLimit = FLOAT32_EXACT_INTEGER;
        if (scalar == ScalarKind::Float16)
        {
            significandLimit = FLOAT16_EXACT_INTEGER;
            if (value > FLOAT16_MAXIMUM || value < -FLOAT16_MAXIMUM)
            {
                return false;
            }
        }
        else if (scalar == ScalarKind::Float64)
        {
            significandLimit = FLOAT64_EXACT_INTEGER;
        }
        if (value == INT64_MINIMUM)
        {
            return true;
        }
        uint64 magnitude = static_cast<uint64>(value < 0 ? -value : value);
        while (magnitude != 0 && (magnitude & 1) == 0)
        {
            magnitude >>= 1;
        }
        return magnitude <= static_cast<uint64>(significandLimit);
    }

    [[nodiscard]] uint32 checkNumber(uint32 node) noexcept
    {
        const std::string_view spelling = spellNode(node);
        const bool isHexadecimal =
            spelling.size() > 2 && spelling[0] == '0' && (spelling[1] == 'x' || spelling[1] == 'X');
        const bool isFloat = isHexadecimal == false && (spelling.find('.') != std::string_view::npos ||
                                                        spelling.find('e') != std::string_view::npos ||
                                                        spelling.find('E') != std::string_view::npos);
        if (isFloat == true)
        {
            return checkFloatLiteral(node, spelling);
        }
        uint64 value = 0;
        if (isHexadecimal == true)
        {
            for (usize i = 2; i < spelling.size(); ++i)
            {
                const char digit = spelling[i];
                uint64 digitValue = HEXADECIMAL_BASE;
                if (digit >= '0' && digit <= '9')
                {
                    digitValue = static_cast<uint64>(digit - '0');
                }
                else if (digit >= 'a' && digit <= 'f')
                {
                    digitValue = static_cast<uint64>(digit - 'a') + HEXADECIMAL_LETTER_VALUE;
                }
                else if (digit >= 'A' && digit <= 'F')
                {
                    digitValue = static_cast<uint64>(digit - 'A') + HEXADECIMAL_LETTER_VALUE;
                }
                if (digitValue == HEXADECIMAL_BASE)
                {
                    return rejectLiteral(node, "is not a hexadecimal literal; LUSL literals take no suffix");
                }
                if (foundation::core::TryMultiply(value, HEXADECIMAL_BASE, value) == false ||
                    foundation::core::TryAdd(value, digitValue, value) == false)
                {
                    value = ~uint64{0};
                    break;
                }
            }
        }
        else
        {
            if (spelling.size() > 1 && spelling[0] == '0')
            {
                return rejectLiteral(node, "looks octal; LUSL has no octal literals");
            }
            const foundation::NumberParseStatus status =
                foundation::ParseUint64(spelling.data(), spelling.size(), value);
            if (status == foundation::NumberParseStatus::OutOfRange)
            {
                value = ~uint64{0};
            }
            else if (status != foundation::NumberParseStatus::Success)
            {
                return rejectLiteral(node, "is not an integer literal; LUSL literals take no suffix");
            }
        }
        if (value > static_cast<uint64>(INT64_MAXIMUM))
        {
            report(DiagnosticCode::ConstantOutOfRange,
                   node,
                   ComposeMessage("'", spelling, "' exceeds the int64 range of integer constants"));
            return TypeTable::ERROR_TYPE;
        }
        setConstant(node, static_cast<int64>(value));
        return TypeTable::UNTYPED_INTEGER_TYPE;
    }

    [[nodiscard]] uint32 rejectLiteral(uint32 node, std::string_view reason) noexcept
    {
        report(DiagnosticCode::InvalidLiteral, node, ComposeMessage("'", spellNode(node), "' ", reason));
        return TypeTable::ERROR_TYPE;
    }

    [[nodiscard]] uint32 checkFloatLiteral(uint32 node, std::string_view spelling) noexcept
    {
        // Validate with the Ludus-owned decimal parser, whose JSON grammar needs a digit on both
        // sides of '.'; LUSL also accepts `.5` and `1.`.
        constexpr usize MAX_FLOAT_LITERAL_BYTES = 1024;
        foundation::StaticString<MAX_FLOAT_LITERAL_BYTES> normalized;
        bool isOk = spelling.size() + 2 <= MAX_FLOAT_LITERAL_BYTES;
        if (isOk == true && spelling.front() == '.')
        {
            isOk = normalized.TryAppend("0") == foundation::StringStatus::Ok;
        }
        for (usize i = 0; i < spelling.size() && isOk == true; ++i)
        {
            isOk = normalized.TryAppend(spelling.substr(i, 1)) == foundation::StringStatus::Ok;
            const bool isDotWithoutDigit =
                spelling[i] == '.' && (i + 1 == spelling.size() || spelling[i + 1] < '0' || spelling[i + 1] > '9');
            if (isOk == true && isDotWithoutDigit == true)
            {
                isOk = normalized.TryAppend("0") == foundation::StringStatus::Ok;
            }
        }
        foundation::float32 value = 0;
        const foundation::NumberParseStatus status =
            isOk == true ? foundation::ParseFloat32(normalized.GetData(), normalized.GetSize(), value)
                         : foundation::NumberParseStatus::TooLong;
        if (status != foundation::NumberParseStatus::Success && status != foundation::NumberParseStatus::OutOfRange)
        {
            return rejectLiteral(node, "is not a floating literal; LUSL literals take no suffix");
        }
        return TypeTable::UNTYPED_FLOAT_TYPE;
    }

    [[nodiscard]] uint32 checkName(uint32 node) noexcept
    {
        const std::string_view name = spellNode(node);
        const uint32 symbol = mScopes.Find(name);
        if (symbol == ScopeTable::UNBOUND)
        {
            if (mTypes.FindBuiltin(name) != NO_TYPE)
            {
                report(DiagnosticCode::TypeMismatch,
                       node,
                       ComposeMessage("'", name, "' is a type; construct a value with ", name, "(...)"));
            }
            else
            {
                report(DiagnosticCode::UnknownName, node, ComposeMessage("'", name, "' is not declared"));
            }
            return TypeTable::ERROR_TYPE;
        }
        const Symbol& declared = mModel.Symbols[symbol];
        mModel.NodeSymbols[node] = symbol;
        switch (declared.Kind)
        {
            case SymbolKind::Variable:
            case SymbolKind::Parameter:
            case SymbolKind::Field:
                if (declared.HasIntegerValue == true)
                {
                    setConstant(node, declared.IntegerValue);
                }
                return declared.Type;
            case SymbolKind::Function:
            case SymbolKind::Intrinsic:
                report(DiagnosticCode::TypeMismatch, node, ComposeMessage("'", name, "' is a function; call it"));
                return TypeTable::ERROR_TYPE;
            default:
                report(DiagnosticCode::TypeMismatch, node, ComposeMessage("'", name, "' is a type, not a value"));
                return TypeTable::ERROR_TYPE;
        }
    }

    [[nodiscard]] uint32 checkScope(uint32 node) noexcept
    {
        const uint32 left = firstChild(node);
        const uint32 right = left == NO_NODE ? NO_NODE : nextSibling(left);
        if (left == NO_NODE || right == NO_NODE || getKind(left) != SyntaxKind::Name)
        {
            report(DiagnosticCode::UnsupportedConstruct,
                   node,
                   ComposeMessage("'::' names enumerators of an enum class"));
            return TypeTable::ERROR_TYPE;
        }
        const uint32 owner = mScopes.Find(spellNode(left));
        if (owner == ScopeTable::UNBOUND || mModel.Symbols[owner].Kind != SymbolKind::Enum)
        {
            report(DiagnosticCode::UnknownName,
                   left,
                   ComposeMessage("'", spellNode(left), "' is not an enum; '::' names enumerators of an enum class"));
            return TypeTable::ERROR_TYPE;
        }
        mModel.NodeSymbols[left] = owner;
        const Symbol& enumeration = mModel.Symbols[owner];
        for (uint32 i = 0; i < enumeration.MemberCount; ++i)
        {
            const uint32 enumerator = mModel.Members[enumeration.FirstMember + i];
            if (getSymbolName(enumerator) == spellNode(right))
            {
                mModel.NodeSymbols[node] = enumerator;
                mModel.NodeSymbols[right] = enumerator;
                setConstant(node, mModel.Symbols[enumerator].IntegerValue);
                (void)setType(right, enumeration.Type);
                return enumeration.Type;
            }
        }
        report(DiagnosticCode::UnknownMember,
               right,
               ComposeMessage("'", spellNode(left), "' has no enumerator '", spellNode(right), "'"));
        return TypeTable::ERROR_TYPE;
    }

    [[nodiscard]] uint32 checkUnary(uint32 node) noexcept
    {
        const uint32 operand = firstChild(node);
        const uint32 type = checkExpression(operand, NO_TYPE);
        if (isError(type) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        const std::string_view operation = spellNode(node);
        const SemanticType& shape = getType(type);
        const bool isInteger = shape.Kind == TypeKind::UntypedInteger ||
                               ((shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector) &&
                                internal::IsIntegerScalar(shape.Scalar) == true);
        const bool isNumeric =
            isUntyped(type) == true ||
            ((shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector || shape.Kind == TypeKind::Matrix) &&
             shape.Scalar != ScalarKind::Bool);
        if ((operation == "~" && isInteger == false) || isNumeric == false)
        {
            const TypeNameText found = nameOf(type);
            report(DiagnosticCode::InvalidOperands,
                   node,
                   ComposeMessage("'", operation, "' is not defined for ", found.GetView()));
            return TypeTable::ERROR_TYPE;
        }
        const IntegerConstant value = getConstant(operand);
        if (value.IsKnown == true && isInteger == true)
        {
            if (operation == "-")
            {
                if (value.Value == INT64_MINIMUM)
                {
                    report(DiagnosticCode::ConstantOverflow, node, ComposeMessage("'-' overflows int64"));
                    return TypeTable::ERROR_TYPE;
                }
                foldTyped(node, -value.Value, type);
            }
            else
            {
                foldTyped(node, operation == "~" ? ~value.Value : value.Value, type);
            }
        }
        return type;
    }

    /// Record a folded integer constant, checking it fits a typed result.
    void foldTyped(uint32 node, int64 value, uint32 type) noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Kind == TypeKind::Scalar && shape.Scalar != ScalarKind::Bool &&
            fitsScalar(value, shape.Scalar) == false)
        {
            const TypeNameText name = nameOf(type);
            report(DiagnosticCode::ConstantOverflow,
                   node,
                   ComposeMessage("the constant result does not fit ", name.GetView()));
            return;
        }
        setConstant(node, value);
    }

    /// Adapt untyped operands of a binary operation and return the common type, or NO_TYPE when
    /// the operands differ. `isSpreadAllowed` lets a scalar combine with a vector or matrix of its type.
    [[nodiscard]] uint32 unify(uint32 leftNode, uint32 rightNode, bool isSpreadAllowed) noexcept
    {
        const uint32 left = mModel.NodeTypes[leftNode];
        const uint32 right = mModel.NodeTypes[rightNode];
        if (left == right)
        {
            return left;
        }
        const bool isLeftUntyped = isUntyped(left);
        const bool isRightUntyped = isUntyped(right);
        if (isLeftUntyped == true && isRightUntyped == true)
        {
            return getType(left).Kind == TypeKind::UntypedFloat || getType(right).Kind == TypeKind::UntypedFloat
                       ? TypeTable::UNTYPED_FLOAT_TYPE
                       : TypeTable::UNTYPED_INTEGER_TYPE;
        }
        if (isLeftUntyped == true || isRightUntyped == true)
        {
            const uint32 constantNode = isLeftUntyped == true ? leftNode : rightNode;
            const uint32 other = isLeftUntyped == true ? right : left;
            const SemanticType& shape = getType(other);
            if (shape.Kind != TypeKind::Scalar && (isSpreadAllowed == false || shape.Kind == TypeKind::Array ||
                                                   shape.Kind == TypeKind::Struct || shape.Kind == TypeKind::Enum))
            {
                return NO_TYPE;
            }
            if (shape.Kind == TypeKind::Enum || shape.Kind == TypeKind::Struct || shape.Kind == TypeKind::Array)
            {
                return NO_TYPE;
            }
            return coerce(constantNode, mTypes.GetScalar(shape.Scalar), false) == true ? other : TypeTable::ERROR_TYPE;
        }
        const SemanticType& a = getType(left);
        const SemanticType& b = getType(right);
        if (isSpreadAllowed == true && a.Scalar == b.Scalar)
        {
            if (a.Kind == TypeKind::Scalar && (b.Kind == TypeKind::Vector || b.Kind == TypeKind::Matrix))
            {
                return right;
            }
            if (b.Kind == TypeKind::Scalar && (a.Kind == TypeKind::Vector || a.Kind == TypeKind::Matrix))
            {
                return left;
            }
        }
        return NO_TYPE;
    }

    void reportOperands(uint32 node, std::string_view operation, uint32 left, uint32 right) noexcept
    {
        const TypeNameText leftName = nameOf(left);
        const TypeNameText rightName = nameOf(right);
        report(
            DiagnosticCode::InvalidOperands,
            node,
            ComposeMessage("'", operation, "' is not defined for ", leftName.GetView(), " and ", rightName.GetView()));
    }

    /// Type of `left operation right`, folding integer constants.
    [[nodiscard]] uint32 typeBinary(uint32 node, std::string_view operation, uint32 leftNode, uint32 rightNode) noexcept
    {
        if (leftNode == NO_NODE || rightNode == NO_NODE)
        {
            return TypeTable::ERROR_TYPE;
        }
        const uint32 left = mModel.NodeTypes[leftNode];
        const uint32 right = mModel.NodeTypes[rightNode];
        if (isError(left) == true || isError(right) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        const uint32 boolType = mTypes.GetScalar(ScalarKind::Bool);
        if (operation == "&&" || operation == "||")
        {
            if (isBoolScalar(left) == false || isBoolScalar(right) == false)
            {
                reportOperands(node, operation, left, right);
                return TypeTable::ERROR_TYPE;
            }
            const IntegerConstant a = getConstant(leftNode);
            const IntegerConstant b = getConstant(rightNode);
            if (a.IsKnown == true && b.IsKnown == true)
            {
                const bool isTrue = operation == "&&" ? (a.Value != 0 && b.Value != 0) : (a.Value != 0 || b.Value != 0);
                setConstant(node, isTrue == true ? 1 : 0);
            }
            return boolType;
        }
        if (operation == "*" && (getType(left).Kind == TypeKind::Matrix || getType(right).Kind == TypeKind::Matrix))
        {
            return typeMatrixProduct(node, leftNode, rightNode);
        }
        const bool isComparison = operation == "==" || operation == "!=" || operation == "<" || operation == "<=" ||
                                  operation == ">" || operation == ">=";
        const bool isEquality = operation == "==" || operation == "!=";
        const bool isBitwise =
            operation == "&" || operation == "|" || operation == "^" || operation == "<<" || operation == ">>";
        const uint32 common = unify(leftNode, rightNode, isComparison == false);
        if (common == TypeTable::ERROR_TYPE)
        {
            return TypeTable::ERROR_TYPE;
        }
        if (common == NO_TYPE)
        {
            reportOperands(node, operation, left, right);
            return TypeTable::ERROR_TYPE;
        }
        const SemanticType& shape = getType(common);
        const bool isUntypedInteger = shape.Kind == TypeKind::UntypedInteger;
        const bool isElementwise = shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector ||
                                   shape.Kind == TypeKind::Matrix || isUntyped(common) == true;
        const bool isBool = isElementwise == true && isUntyped(common) == false && shape.Scalar == ScalarKind::Bool;
        const bool isInteger = isUntypedInteger == true || (isElementwise == true && isUntyped(common) == false &&
                                                            internal::IsIntegerScalar(shape.Scalar) == true);
        bool isDefined = false;
        if (isEquality == true)
        {
            isDefined = isElementwise == true || shape.Kind == TypeKind::Enum;
        }
        else if (isComparison == true)
        {
            isDefined = isElementwise == true && isBool == false && shape.Kind != TypeKind::Matrix;
        }
        else if (isBitwise == true)
        {
            isDefined = isInteger == true;
        }
        else
        {
            isDefined = isElementwise == true && isBool == false;
        }
        if (isDefined == false)
        {
            reportOperands(node, operation, left, right);
            return TypeTable::ERROR_TYPE;
        }
        uint32 result = common;
        if (isComparison == true)
        {
            result = shape.Kind == TypeKind::Vector ? mTypes.GetVector(ScalarKind::Bool, shape.Rows) : boolType;
        }
        foldBinary(node, operation, leftNode, rightNode, result, isInteger);
        return result;
    }

    void foldBinary(uint32 node,
                    std::string_view operation,
                    uint32 leftNode,
                    uint32 rightNode,
                    uint32 result,
                    bool isInteger) noexcept
    {
        const IntegerConstant a = getConstant(leftNode);
        const IntegerConstant b = getConstant(rightNode);
        const bool isBoolResult =
            getType(result).Kind == TypeKind::Scalar && getType(result).Scalar == ScalarKind::Bool;
        if (a.IsKnown == false || b.IsKnown == false || (isInteger == false && isBoolResult == false))
        {
            return;
        }
        if (isBoolResult == true)
        {
            bool isTrue = false;
            if (operation == "==")
            {
                isTrue = a.Value == b.Value;
            }
            else if (operation == "!=")
            {
                isTrue = a.Value != b.Value;
            }
            else if (operation == "<")
            {
                isTrue = a.Value < b.Value;
            }
            else if (operation == "<=")
            {
                isTrue = a.Value <= b.Value;
            }
            else if (operation == ">")
            {
                isTrue = a.Value > b.Value;
            }
            else
            {
                isTrue = a.Value >= b.Value;
            }
            setConstant(node, isTrue == true ? 1 : 0);
            return;
        }
        int64 value = 0;
        bool isRepresentable = true;
        constexpr int64 MAX_SHIFT = 62;
        if (operation == "+")
        {
            isRepresentable = foundation::core::TryAdd(a.Value, b.Value, value);
        }
        else if (operation == "-")
        {
            isRepresentable = foundation::core::TrySubtract(a.Value, b.Value, value);
        }
        else if (operation == "*")
        {
            isRepresentable = foundation::core::TryMultiply(a.Value, b.Value, value);
        }
        else if (operation == "/" || operation == "%")
        {
            if (b.Value == 0)
            {
                report(DiagnosticCode::ConstantOverflow, node, ComposeMessage("the constant divides by zero"));
                return;
            }
            isRepresentable = (a.Value == INT64_MINIMUM && b.Value == -1) == false;
            value = isRepresentable == true ? (operation == "/" ? a.Value / b.Value : a.Value % b.Value) : 0;
        }
        else if (operation == "&")
        {
            value = a.Value & b.Value;
        }
        else if (operation == "|")
        {
            value = a.Value | b.Value;
        }
        else if (operation == "^")
        {
            value = a.Value ^ b.Value;
        }
        else if (operation == "<<" || operation == ">>")
        {
            isRepresentable = b.Value >= 0 && b.Value <= MAX_SHIFT && (operation == ">>" || a.Value >= 0);
            if (isRepresentable == true && operation == "<<")
            {
                isRepresentable = a.Value <= (INT64_MAXIMUM >> b.Value);
            }
            value = isRepresentable == true ? (operation == "<<" ? a.Value << b.Value : a.Value >> b.Value) : 0;
        }
        if (isRepresentable == false)
        {
            report(DiagnosticCode::ConstantOverflow,
                   node,
                   ComposeMessage("the constant '", operation, "' overflows int64"));
            return;
        }
        foldTyped(node, value, result);
    }

    [[nodiscard]] uint32 typeMatrixProduct(uint32 node, uint32 leftNode, uint32 rightNode) noexcept
    {
        const uint32 left = mModel.NodeTypes[leftNode];
        const uint32 right = mModel.NodeTypes[rightNode];
        const SemanticType& a = getType(left);
        const SemanticType& b = getType(right);
        if (a.Kind == TypeKind::Matrix && b.Kind == TypeKind::Matrix && a.Scalar == b.Scalar && a.Columns == b.Rows)
        {
            return mTypes.GetMatrix(a.Scalar, a.Rows, b.Columns);
        }
        if (a.Kind == TypeKind::Matrix && b.Kind == TypeKind::Vector && a.Scalar == b.Scalar && a.Columns == b.Rows)
        {
            return mTypes.GetVector(a.Scalar, a.Rows);
        }
        if (a.Kind == TypeKind::Vector && b.Kind == TypeKind::Matrix)
        {
            report(DiagnosticCode::InvalidOperands,
                   node,
                   ComposeMessage("LUSL uses column vectors; write matrix * vector, or transpose the matrix"));
            return TypeTable::ERROR_TYPE;
        }
        const uint32 common = unify(leftNode, rightNode, true);
        if (common == NO_TYPE || isError(common) == true)
        {
            if (common == NO_TYPE)
            {
                reportOperands(node, "*", left, right);
            }
            return TypeTable::ERROR_TYPE;
        }
        return common;
    }

    [[nodiscard]] uint32 checkConditional(uint32 node, uint32 expected) noexcept
    {
        const uint32 condition = firstChild(node);
        const uint32 whenTrue = condition == NO_NODE ? NO_NODE : nextSibling(condition);
        const uint32 whenFalse = whenTrue == NO_NODE ? NO_NODE : nextSibling(whenTrue);
        requireCondition(condition);
        const uint32 a = checkExpression(whenTrue, expected);
        const uint32 b = checkExpression(whenFalse, expected);
        if (isError(a) == true || isError(b) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        const uint32 common = unify(whenTrue, whenFalse, false);
        if (common == NO_TYPE)
        {
            reportOperands(node, "?:", a, b);
            return TypeTable::ERROR_TYPE;
        }
        const IntegerConstant selector = getConstant(condition);
        const IntegerConstant chosen = selector.Value != 0 ? getConstant(whenTrue) : getConstant(whenFalse);
        if (selector.IsKnown == true && chosen.IsKnown == true)
        {
            setConstant(node, chosen.Value);
        }
        return common;
    }

    [[nodiscard]] uint32 checkMember(uint32 node) noexcept
    {
        const uint32 object = firstChild(node);
        const uint32 type = checkExpression(object, NO_TYPE);
        if (isError(type) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        const std::string_view name = spellNode(node);
        const SemanticType& shape = getType(type);
        if (shape.Kind == TypeKind::Struct)
        {
            if (isOpaque(type) == true)
            {
                report(DiagnosticCode::UnknownMember,
                       node,
                       ComposeMessage("an opaque 'extern slang' type has no fields LUSL can see"));
                return TypeTable::ERROR_TYPE;
            }
            const uint32 field = findMember(shape.Symbol, name, SymbolKind::Field);
            if (field != NO_SYMBOL)
            {
                mModel.NodeSymbols[node] = field;
                return mModel.Symbols[field].Type;
            }
            if (findMember(shape.Symbol, name, SymbolKind::Function) != NO_SYMBOL)
            {
                report(DiagnosticCode::TypeMismatch, node, ComposeMessage("'", name, "' is a method; call it"));
                return TypeTable::ERROR_TYPE;
            }
        }
        if (shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector)
        {
            bool hasRepeat = false;
            const uint32 size = shape.Kind == TypeKind::Scalar ? 1 : shape.Rows;
            if (GetSwizzleMask(name, size, hasRepeat) != 0)
            {
                return mTypes.GetVector(shape.Scalar, static_cast<uint32>(name.size()));
            }
        }
        const TypeNameText typeName = nameOf(type);
        report(DiagnosticCode::UnknownMember,
               node,
               ComposeMessage(typeName.GetView(), " has no member or swizzle '", name, "'"));
        return TypeTable::ERROR_TYPE;
    }

    [[nodiscard]] uint32 checkIndex(uint32 node) noexcept
    {
        const uint32 object = firstChild(node);
        const uint32 index = object == NO_NODE ? NO_NODE : nextSibling(object);
        const uint32 type = checkExpression(object, NO_TYPE);
        const uint32 indexType = checkExpression(index, NO_TYPE);
        if (isError(type) == true || isError(indexType) == true)
        {
            return TypeTable::ERROR_TYPE;
        }
        const SemanticType& indexShape = getType(indexType);
        const bool isIntegerIndex =
            indexShape.Kind == TypeKind::UntypedInteger ||
            (indexShape.Kind == TypeKind::Scalar && internal::IsIntegerScalar(indexShape.Scalar));
        if (isIntegerIndex == false)
        {
            const TypeNameText found = nameOf(indexType);
            report(DiagnosticCode::TypeMismatch,
                   index,
                   ComposeMessage("an index is an integer scalar, not ", found.GetView()));
            return TypeTable::ERROR_TYPE;
        }
        const SemanticType& shape = getType(type);
        uint32 element = NO_TYPE;
        uint32 count = 0;
        if (shape.Kind == TypeKind::Array)
        {
            element = shape.Element;
            count = shape.Count;
        }
        else if (shape.Kind == TypeKind::Vector)
        {
            element = mTypes.GetScalar(shape.Scalar);
            count = shape.Rows;
        }
        else if (shape.Kind == TypeKind::Matrix)
        {
            element = mTypes.GetVector(shape.Scalar, shape.Columns);
            count = shape.Rows;
        }
        if (element == NO_TYPE)
        {
            const TypeNameText found = nameOf(type);
            report(DiagnosticCode::InvalidOperands, node, ComposeMessage(found.GetView(), " cannot be indexed"));
            return TypeTable::ERROR_TYPE;
        }
        const IntegerConstant value = getConstant(index);
        if (value.IsKnown == true && (value.Value < 0 || value.Value >= static_cast<int64>(count)))
        {
            report(DiagnosticCode::ConstantOutOfRange,
                   index,
                   ComposeMessage("the index is outside 0 to ", uint64{count - 1}));
        }
        if (isUntyped(indexType) == true)
        {
            (void)coerce(index, mTypes.GetScalar(ScalarKind::Uint32), false);
        }
        return element;
    }

    [[nodiscard]] uint32 checkInitializerList(uint32 node, uint32 expected) noexcept
    {
        if (expected == NO_TYPE || getType(expected).Kind != TypeKind::Struct)
        {
            if (expected == NO_TYPE || isError(expected) == false)
            {
                report(DiagnosticCode::InvalidInitializer,
                       node,
                       ComposeMessage("a designated initializer needs a struct target, such as a declared variable"));
            }
            return TypeTable::ERROR_TYPE;
        }
        if (isOpaque(expected) == true)
        {
            report(DiagnosticCode::InvalidInitializer,
                   node,
                   ComposeMessage("an opaque 'extern slang' type cannot be initialized in LUSL"));
            return TypeTable::ERROR_TYPE;
        }
        const uint32 structSymbol = getType(expected).Symbol;
        Array<uint32> seen;
        for (uint32 field = firstChild(node); field != NO_NODE && mIsFatal == false; field = nextSibling(field))
        {
            const std::string_view name = spellNode(field);
            const uint32 member = findMember(structSymbol, name, SymbolKind::Field);
            const uint32 value = firstChild(field);
            if (member == NO_SYMBOL)
            {
                const TypeNameText typeName = nameOf(expected);
                report(DiagnosticCode::InvalidInitializer,
                       field,
                       ComposeMessage(typeName.GetView(), " has no field '", name, "'"));
                (void)checkExpression(value, NO_TYPE);
                continue;
            }
            for (const uint32 previous : seen)
            {
                if (previous == member)
                {
                    report(DiagnosticCode::InvalidInitializer,
                           field,
                           ComposeMessage("'", name, "' is initialized twice"));
                }
            }
            if (seen.TryAdd(member) == false)
            {
                mSink.ReportOutOfMemory();
                mIsFatal = true;
                return TypeTable::ERROR_TYPE;
            }
            mModel.NodeSymbols[field] = member;
            const uint32 fieldType = mModel.Symbols[member].Type;
            (void)setType(field, fieldType);
            (void)checkExpression(value, fieldType);
            (void)coerce(value, fieldType, true);
        }
        return expected;
    }

    // ----- Calls ---------------------------------------------------------------------------------

    [[nodiscard]] uint32 checkCall(uint32 node, uint32 expected) noexcept
    {
        (void)expected;
        const uint32 callee = firstChild(node);
        Array<uint32> arguments;
        Array<internal::ArgumentFacts> facts;
        for (uint32 argument = callee == NO_NODE ? NO_NODE : nextSibling(callee); argument != NO_NODE;
             argument = nextSibling(argument))
        {
            const uint32 value = firstChild(argument);
            internal::ArgumentFacts fact;
            if (getNode(argument).Token != NO_TOKEN)
            {
                fact.Name = spell(getNode(argument).Token);
            }
            if (getKind(value) != SyntaxKind::InitializerList)
            {
                fact.Type = checkExpression(value, NO_TYPE);
                (void)setType(argument, fact.Type);
            }
            if (arguments.TryAdd(argument) == false || facts.TryAdd(fact) == false)
            {
                mSink.ReportOutOfMemory();
                mIsFatal = true;
                return TypeTable::ERROR_TYPE;
            }
        }
        if (arguments.GetSize() > MAX_FUNCTION_PARAMETERS)
        {
            report(DiagnosticCode::CheckerLimit,
                   node,
                   ComposeMessage("a call takes at most ", uint64{MAX_FUNCTION_PARAMETERS}, " arguments"));
            return TypeTable::ERROR_TYPE;
        }
        if (getKind(callee) == SyntaxKind::Name)
        {
            const std::string_view name = spellNode(callee);
            const uint32 builtin = mTypes.FindBuiltin(name);
            if (builtin != NO_TYPE)
            {
                return checkConstruction(node, builtin, arguments, facts);
            }
            const uint32 symbol = mScopes.Find(name);
            if (symbol == ScopeTable::UNBOUND)
            {
                report(DiagnosticCode::UnknownName, callee, ComposeMessage("'", name, "' is not declared"));
                return TypeTable::ERROR_TYPE;
            }
            const Symbol& declared = mModel.Symbols[symbol];
            switch (declared.Kind)
            {
                case SymbolKind::Function:
                    return resolveCall(node, symbol, NO_NODE, arguments, facts);
                case SymbolKind::Intrinsic:
                    return checkIntrinsic(node, symbol, arguments, facts);
                case SymbolKind::Enum:
                    return checkConstruction(node, declared.Type, arguments, facts);
                case SymbolKind::Struct:
                    report(DiagnosticCode::InvalidConstruction,
                           callee,
                           ComposeMessage("build a struct with a designated initializer: { .Field = value }"));
                    return TypeTable::ERROR_TYPE;
                default:
                    report(DiagnosticCode::NotCallable, callee, ComposeMessage("'", name, "' is not a function"));
                    return TypeTable::ERROR_TYPE;
            }
        }
        if (getKind(callee) == SyntaxKind::MemberExpression)
        {
            const uint32 object = firstChild(callee);
            const uint32 objectType = checkExpression(object, NO_TYPE);
            if (isError(objectType) == true)
            {
                return TypeTable::ERROR_TYPE;
            }
            const SemanticType& shape = getType(objectType);
            const uint32 method = shape.Kind == TypeKind::Struct && isOpaque(objectType) == false
                                      ? findMember(shape.Symbol, spellNode(callee), SymbolKind::Function)
                                      : NO_SYMBOL;
            if (method == NO_SYMBOL)
            {
                const TypeNameText typeName = nameOf(objectType);
                report(DiagnosticCode::UnknownMember,
                       callee,
                       ComposeMessage(typeName.GetView(), " has no method '", spellNode(callee), "'"));
                return TypeTable::ERROR_TYPE;
            }
            return resolveCall(node, method, object, arguments, facts);
        }
        report(DiagnosticCode::NotCallable,
               callee == NO_NODE ? node : callee,
               ComposeMessage("this is not a function"));
        return TypeTable::ERROR_TYPE;
    }

    [[nodiscard]] bool rejectMarks(uint32 node, const Array<uint32>& arguments, std::string_view what) noexcept
    {
        for (const uint32 argument : arguments)
        {
            if (getNode(argument).Token != NO_TOKEN || getNode(argument).Direction != ParameterDirection::Unmarked ||
                getKind(firstChild(argument)) == SyntaxKind::InitializerList)
            {
                report(DiagnosticCode::InvalidConstruction,
                       argument,
                       ComposeMessage(what, " take positional values without names, marks or initializer lists"));
                (void)node;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] uint32 checkIntrinsic(uint32 node,
                                        uint32 symbol,
                                        const Array<uint32>& arguments,
                                        const Array<internal::ArgumentFacts>& facts) noexcept
    {
        if (rejectMarks(node, arguments, "built-in functions") == false)
        {
            return TypeTable::ERROR_TYPE;
        }
        foundation::StaticArray<uint32, MAX_FUNCTION_PARAMETERS> types{};
        for (usize i = 0; i < facts.GetSize(); ++i)
        {
            types[i] = facts[i].Type;
        }
        const internal::IntrinsicSpelling& intrinsic = internal::INTRINSICS[symbol];
        uint32 operand = NO_TYPE;
        uint32 result = NO_TYPE;
        if (internal::ResolveIntrinsic(intrinsic.Family,
                                       mTypes,
                                       foundation::BorrowedView<const uint32>(types.GetData(), facts.GetSize()),
                                       operand,
                                       result) == false)
        {
            report(DiagnosticCode::NoMatchingOverload,
                   node,
                   ComposeMessage("'",
                                  intrinsic.Name,
                                  "' does not accept these arguments; it takes ",
                                  uint64{internal::GetIntrinsicArity(intrinsic.Family)},
                                  " of matching type"));
            return TypeTable::ERROR_TYPE;
        }
        const bool isBitcast = intrinsic.Family == internal::IntrinsicFamily::BitcastToUint ||
                               intrinsic.Family == internal::IntrinsicFamily::BitcastToInt ||
                               intrinsic.Family == internal::IntrinsicFamily::BitcastToFloat ||
                               intrinsic.Family == internal::IntrinsicFamily::Transpose;
        // Element-wise families accept scalars beside vectors, as the arithmetic operators do.
        const bool isSpreadAllowed = intrinsic.Family == internal::IntrinsicFamily::NumericBinary ||
                                     intrinsic.Family == internal::IntrinsicFamily::FloatBinary ||
                                     intrinsic.Family == internal::IntrinsicFamily::NumericTernary ||
                                     intrinsic.Family == internal::IntrinsicFamily::FloatTernary;
        const uint32 elementScalar = isError(operand) == true ? operand : mTypes.GetScalar(getType(operand).Scalar);
        for (const uint32 argument : arguments)
        {
            const uint32 value = firstChild(argument);
            const uint32 type = mModel.NodeTypes[value];
            const bool isSpreadScalar = isSpreadAllowed == true && (type == elementScalar || isUntyped(type) == true);
            if (isSpreadScalar == true)
            {
                (void)coerce(value, elementScalar, false);
            }
            else if (isBitcast == false)
            {
                (void)coerce(value, operand, false);
            }
            (void)setType(argument, mModel.NodeTypes[value]);
        }
        mModel.NodeSymbols[node] = symbol;
        mModel.NodeCallKinds[node] = CallKind::Intrinsic;
        return result;
    }

    /// Components a construction argument contributes, or zero when it cannot contribute.
    [[nodiscard]] uint32 getComponentCount(uint32 type, ScalarKind kind) const noexcept
    {
        const SemanticType& shape = getType(type);
        if (isUntyped(type) == true)
        {
            return 1;
        }
        if (shape.Scalar != kind)
        {
            return 0;
        }
        if (shape.Kind == TypeKind::Scalar)
        {
            return 1;
        }
        return shape.Kind == TypeKind::Vector ? shape.Rows : 0;
    }

    [[nodiscard]] uint32 checkConstruction(uint32 node,
                                           uint32 type,
                                           const Array<uint32>& arguments,
                                           const Array<internal::ArgumentFacts>& facts) noexcept
    {
        if (rejectMarks(node, arguments, "constructions and conversions") == false)
        {
            return TypeTable::ERROR_TYPE;
        }
        for (const internal::ArgumentFacts& fact : facts)
        {
            if (isError(fact.Type) == true)
            {
                return type;
            }
        }
        const SemanticType& shape = getType(type);
        const TypeNameText typeName = nameOf(type);
        const usize count = arguments.GetSize();
        // One value of another element type, enum or shape of the same size: an explicit conversion.
        if (count == 1)
        {
            const uint32 value = firstChild(arguments[0]);
            const SemanticType& from = getType(facts[0].Type);
            const bool isFromBool =
                from.Kind == TypeKind::Scalar || from.Kind == TypeKind::Vector || from.Kind == TypeKind::Matrix
                    ? from.Scalar == ScalarKind::Bool
                    : false;
            const bool isToBool = shape.Kind != TypeKind::Enum && shape.Scalar == ScalarKind::Bool;
            const bool isSameShape =
                (from.Kind == shape.Kind && from.Rows == shape.Rows && from.Columns == shape.Columns) ||
                (shape.Kind == TypeKind::Scalar && from.Kind == TypeKind::Enum) ||
                (shape.Kind == TypeKind::Enum && from.Kind == TypeKind::Scalar) ||
                (shape.Kind == TypeKind::Enum && from.Kind == TypeKind::Enum);
            const bool isIntegerSide = shape.Kind != TypeKind::Enum || internal::IsIntegerScalar(from.Scalar) == true ||
                                       from.Kind == TypeKind::Enum;
            if (isUntyped(facts[0].Type) == true && shape.Kind != TypeKind::Vector && shape.Kind != TypeKind::Matrix)
            {
                const uint32 target = shape.Kind == TypeKind::Enum ? mTypes.GetScalar(shape.Scalar) : type;
                if (coerce(value, target, false) == false)
                {
                    return type;
                }
                mModel.NodeCallKinds[node] = CallKind::Conversion;
                mModel.NodeIntegerConstants[node] = getConstant(value);
                return type;
            }
            if (isSameShape == true && isFromBool == isToBool && isIntegerSide == true)
            {
                mModel.NodeCallKinds[node] = CallKind::Conversion;
                const IntegerConstant constant = getConstant(value);
                if (constant.IsKnown == true && shape.Kind == TypeKind::Scalar &&
                    internal::IsIntegerScalar(shape.Scalar) == true)
                {
                    foldTyped(node, constant.Value, type);
                }
                return type;
            }
            if (isFromBool != isToBool && isSameShape == true)
            {
                report(DiagnosticCode::InvalidConstruction,
                       node,
                       ComposeMessage("bool and numbers do not convert; compare with '!= 0' or select with '?:'"));
                return type;
            }
        }
        if (shape.Kind == TypeKind::Vector)
        {
            uint32 components = 0;
            for (usize i = 0; i < count; ++i)
            {
                const uint32 contribution = getComponentCount(facts[i].Type, shape.Scalar);
                if (contribution == 0)
                {
                    components = 0;
                    break;
                }
                components += contribution;
            }
            const bool isSplat = count == 1 && components == 1;
            if (components == shape.Rows || isSplat == true)
            {
                for (const uint32 argument : arguments)
                {
                    if (isUntyped(mModel.NodeTypes[firstChild(argument)]) == true)
                    {
                        (void)coerce(firstChild(argument), mTypes.GetScalar(shape.Scalar), false);
                    }
                }
                mModel.NodeCallKinds[node] = CallKind::Construction;
                return type;
            }
        }
        if (shape.Kind == TypeKind::Matrix)
        {
            bool isRows = count == shape.Rows;
            bool isScalars = count == static_cast<usize>(shape.Rows) * shape.Columns;
            for (usize i = 0; i < count; ++i)
            {
                const uint32 contribution = getComponentCount(facts[i].Type, shape.Scalar);
                const bool isVectorRow = getType(facts[i].Type).Kind == TypeKind::Vector;
                isRows = isRows == true && isVectorRow == true && contribution == shape.Columns;
                isScalars = isScalars == true && contribution == 1;
            }
            if (isRows == true || isScalars == true)
            {
                for (const uint32 argument : arguments)
                {
                    if (isUntyped(mModel.NodeTypes[firstChild(argument)]) == true)
                    {
                        (void)coerce(firstChild(argument), mTypes.GetScalar(shape.Scalar), false);
                    }
                }
                mModel.NodeCallKinds[node] = CallKind::Construction;
                return type;
            }
        }
        report(DiagnosticCode::InvalidConstruction,
               node,
               ComposeMessage(typeName.GetView(),
                              " cannot be built from these arguments; vectors take components of their element "
                              "type, or one value to convert"));
        return type;
    }

    [[nodiscard]] uint32 resolveCall(uint32 node,
                                     uint32 head,
                                     uint32 object,
                                     const Array<uint32>& arguments,
                                     const Array<internal::ArgumentFacts>& facts) noexcept
    {
        const foundation::BorrowedView<const internal::ArgumentFacts> argumentView(facts.GetData(), facts.GetSize());
        uint32 best = NO_SYMBOL;
        uint32 bestConversions = 0;
        uint32 ties = 0;
        uint32 candidateCount = 0;
        internal::BindingResult bestBinding;
        internal::BindingResult lastFailure;
        foundation::StaticArray<internal::ParameterFacts, MAX_FUNCTION_PARAMETERS> parameters{};
        for (uint32 candidate = head; candidate != NO_SYMBOL; candidate = mNextOverloads[candidate])
        {
            ++candidateCount;
            const Symbol& function = mModel.Symbols[candidate];
            for (uint32 p = 0; p < function.MemberCount && p < MAX_FUNCTION_PARAMETERS; ++p)
            {
                const uint32 parameter = mModel.Members[function.FirstMember + p];
                const Symbol& declared = mModel.Symbols[parameter];
                parameters[p].Name = getSymbolName(parameter);
                parameters[p].Type = declared.Type;
                parameters[p].HasDefault = declared.HasDefault;
                parameters[p].CanTakeInitializer =
                    getType(declared.Type).Kind == TypeKind::Struct && isOpaque(declared.Type) == false;
            }
            internal::BindingResult binding;
            const bool isViable = internal::BindArguments(
                argumentView,
                foundation::BorrowedView<const internal::ParameterFacts>(parameters.GetData(), function.MemberCount),
                mTypes,
                binding);
            if (isViable == false)
            {
                lastFailure = binding;
                continue;
            }
            if (best == NO_SYMBOL || binding.ConversionCount < bestConversions)
            {
                best = candidate;
                bestConversions = binding.ConversionCount;
                bestBinding = binding;
                ties = 0;
            }
            else if (binding.ConversionCount == bestConversions)
            {
                ++ties;
            }
        }
        const std::string_view name = getSymbolName(head);
        if (best == NO_SYMBOL)
        {
            if (candidateCount == 1)
            {
                reportBindingFailure(node, head, arguments, lastFailure);
            }
            else
            {
                report(DiagnosticCode::NoMatchingOverload,
                       node,
                       ComposeMessage("none of the ",
                                      uint64{candidateCount},
                                      " overloads of '",
                                      name,
                                      "' accept these arguments"));
            }
            return TypeTable::ERROR_TYPE;
        }
        if (ties > 0)
        {
            report(DiagnosticCode::AmbiguousCall,
                   node,
                   ComposeMessage("several overloads of '",
                                  name,
                                  "' accept these arguments; name the argument types explicitly"));
            return TypeTable::ERROR_TYPE;
        }
        const Symbol& function = mModel.Symbols[best];
        mModel.NodeSymbols[node] = best;
        mModel.NodeCallKinds[node] = function.Parent != NO_SYMBOL ? CallKind::Method : CallKind::Function;
        if (object != NO_NODE)
        {
            mModel.NodeSymbols[firstChild(node)] = best;
        }
        checkMethodContext(node, function, object);
        Array<WritableArgument> writable;
        for (usize i = 0; i < arguments.GetSize(); ++i)
        {
            const uint32 argument = arguments[i];
            const uint32 value = firstChild(argument);
            const uint32 position = bestBinding.ParameterOfArgument[i];
            const Symbol& parameter = mModel.Symbols[mModel.Members[function.FirstMember + position]];
            mModel.NodeParameters[argument] = position;
            if (getKind(value) == SyntaxKind::InitializerList)
            {
                (void)checkExpression(value, parameter.Type);
            }
            else
            {
                (void)coerce(value, parameter.Type, false);
            }
            (void)setType(argument, parameter.Type);
            checkArgumentDirection(argument, parameter, writable);
        }
        checkAliasing(writable);
        return function.Type;
    }

    void reportBindingFailure(uint32 node,
                              uint32 function,
                              const Array<uint32>& arguments,
                              const internal::BindingResult& failure) noexcept
    {
        const std::string_view name = getSymbolName(function);
        const Symbol& declared = mModel.Symbols[function];
        const uint32 argument =
            failure.FailingArgument < arguments.GetSize() ? arguments[failure.FailingArgument] : node;
        switch (failure.Failure)
        {
            case internal::BindingFailure::TooManyArguments:
                report(DiagnosticCode::NoMatchingOverload,
                       argument,
                       ComposeMessage("'", name, "' takes ", uint64{declared.MemberCount}, " arguments"));
                return;
            case internal::BindingFailure::UnknownName:
                report(DiagnosticCode::UnknownNamedArgument,
                       argument,
                       ComposeMessage("'", name, "' has no parameter '", spell(getNode(argument).Token), "'"));
                return;
            case internal::BindingFailure::Duplicate:
                report(DiagnosticCode::DuplicateArgument,
                       argument,
                       ComposeMessage("this argument binds a parameter of '", name, "' that is already bound"));
                return;
            case internal::BindingFailure::Missing: {
                const uint32 parameter = mModel.Members[declared.FirstMember + failure.FailingParameter];
                report(DiagnosticCode::MissingArgument,
                       node,
                       ComposeMessage("'", name, "' needs a value for parameter '", getSymbolName(parameter), "'"));
                return;
            }
            case internal::BindingFailure::TypeMismatch: {
                // Find the parameter the failing argument would bind, and report the conversion.
                uint32 position = 0;
                const std::string_view argumentName = spell(getNode(argument).Token);
                if (argumentName.empty() == true)
                {
                    for (uint32 i = 0; i < failure.FailingArgument && i < arguments.GetSize(); ++i)
                    {
                        position += getNode(arguments[i]).Token == NO_TOKEN ? 1 : 0;
                    }
                }
                else
                {
                    for (uint32 p = 0; p < declared.MemberCount; ++p)
                    {
                        if (getSymbolName(mModel.Members[declared.FirstMember + p]) == argumentName)
                        {
                            position = p;
                        }
                    }
                }
                const Symbol& parameter = mModel.Symbols[mModel.Members[declared.FirstMember + position]];
                const uint32 value = firstChild(argument);
                if (getKind(value) == SyntaxKind::InitializerList)
                {
                    report(DiagnosticCode::InvalidInitializer,
                           value,
                           ComposeMessage("this parameter is not a struct a designated initializer can build"));
                    return;
                }
                (void)coerce(value, parameter.Type, false);
                return;
            }
            case internal::BindingFailure::None:
                return;
        }
    }

    void checkMethodContext(uint32 node, const Symbol& function, uint32 object) noexcept
    {
        if (function.Parent == NO_SYMBOL || function.IsMutating == false)
        {
            return;
        }
        if (object != NO_NODE)
        {
            (void)requireAssignable(object);
        }
        else if (mFunction.IsMutating == false)
        {
            report(DiagnosticCode::NotAssignable,
                   node,
                   ComposeMessage("a method that is not [mutating] cannot call the mutating method '",
                                  spell(function.NameToken),
                                  "'"));
        }
    }

    void
    checkArgumentDirection(uint32 argument, const Symbol& parameter, Array<WritableArgument>& inoutWritable) noexcept
    {
        const ParameterDirection mark = getNode(argument).Direction;
        const bool isWritableParameter =
            parameter.Direction == ParameterDirection::Out || parameter.Direction == ParameterDirection::Inout;
        const std::string_view parameterName = spell(parameter.NameToken);
        const uint32 valueToken = getNode(firstChild(argument)).FirstToken;
        const uint32 markToken = mark == ParameterDirection::Unmarked || valueToken == 0 ? NO_TOKEN : valueToken - 1;
        if (isWritableParameter == true && mark != parameter.Direction)
        {
            const std::string_view keyword = parameter.Direction == ParameterDirection::Out ? "out" : "inout";
            internal::FixBuilder fix(mRequest, mInput);
            if (markToken == NO_TOKEN)
            {
                FixText inserted;
                (void)inserted.TryAssign(keyword);
                (void)inserted.TryAppend(" ");
                fix.InsertBefore(valueToken, inserted.GetView());
            }
            else
            {
                fix.Replace(markToken, keyword);
            }
            report(DiagnosticCode::DirectionMismatch,
                   argument,
                   ComposeMessage("parameter '",
                                  parameterName,
                                  "' is ",
                                  keyword,
                                  "; mark the argument '",
                                  keyword,
                                  " value'"),
                   fix.GetFix());
            return;
        }
        if (isWritableParameter == false && mark != ParameterDirection::Unmarked)
        {
            internal::FixBuilder fix(mRequest, mInput);
            fix.Remove(markToken);
            report(DiagnosticCode::DirectionMismatch,
                   argument,
                   ComposeMessage("parameter '", parameterName, "' is an input; remove the mark"),
                   fix.GetFix());
            return;
        }
        if (isWritableParameter == false)
        {
            return;
        }
        const uint32 value = firstChild(argument);
        if (requireAssignable(value) == false)
        {
            return;
        }
        WritableArgument entry;
        entry.Node = value;
        collectAccessPath(value, entry.Path);
        if (inoutWritable.TryAdd(entry) == false)
        {
            mSink.ReportOutOfMemory();
            mIsFatal = true;
        }
    }

    void checkAliasing(const Array<WritableArgument>& writable) noexcept
    {
        for (usize i = 0; i < writable.GetSize(); ++i)
        {
            for (usize j = 0; j < i; ++j)
            {
                if (internal::MayOverlap(writable[i].Path, writable[j].Path) == true)
                {
                    report(DiagnosticCode::AliasedArguments,
                           writable[i].Node,
                           ComposeMessage("this writable argument may share storage with an earlier one; pass "
                                          "separate variables"));
                    break;
                }
            }
        }
    }

    // ----- Lvalues and constants -----------------------------------------------------------------

    /// Report and return false unless `node` names assignable storage.
    [[nodiscard]] bool requireAssignable(uint32 node) noexcept
    {
        if (node == NO_NODE || isError(mModel.NodeTypes[node]) == true)
        {
            return false;
        }
        switch (getKind(node))
        {
            case SyntaxKind::ParenthesizedExpression:
                return requireAssignable(firstChild(node));
            case SyntaxKind::Name: {
                const uint32 symbol = mModel.NodeSymbols[node];
                if (symbol == NO_SYMBOL)
                {
                    return false;
                }
                const Symbol& declared = mModel.Symbols[symbol];
                if (declared.Kind == SymbolKind::Parameter ||
                    (declared.Kind == SymbolKind::Variable && declared.IsConstant == false))
                {
                    return true;
                }
                if (declared.Kind == SymbolKind::Field)
                {
                    if (mFunction.IsMutating == true)
                    {
                        return true;
                    }
                    report(
                        DiagnosticCode::NotAssignable,
                        node,
                        ComposeMessage("'", spellNode(node), "' is a field; mark the method [mutating] to assign it"));
                    return false;
                }
                report(DiagnosticCode::NotAssignable,
                       node,
                       ComposeMessage("'", spellNode(node), "' is a constant and cannot be assigned"));
                return false;
            }
            case SyntaxKind::MemberExpression: {
                if (mModel.NodeSymbols[node] == NO_SYMBOL)
                {
                    bool hasRepeat = false;
                    (void)GetSwizzleMask(spellNode(node), MAX_SWIZZLE_COMPONENTS, hasRepeat);
                    if (hasRepeat == true)
                    {
                        report(DiagnosticCode::NotAssignable,
                               node,
                               ComposeMessage("a swizzle that repeats a component cannot be assigned"));
                        return false;
                    }
                }
                return requireAssignable(firstChild(node));
            }
            case SyntaxKind::IndexExpression:
                return requireAssignable(firstChild(node));
            default:
                report(DiagnosticCode::NotAssignable,
                       node,
                       ComposeMessage("only variables, fields and elements can be assigned"));
                return false;
        }
    }

    void collectAccessPath(uint32 node, internal::AccessPath& outPath) noexcept
    {
        const SyntaxKind kind = getKind(node);
        if (kind == SyntaxKind::ParenthesizedExpression)
        {
            collectAccessPath(firstChild(node), outPath);
            return;
        }
        if (kind == SyntaxKind::Name)
        {
            outPath = internal::AccessPath{};
            outPath.Root = mModel.NodeSymbols[node];
            return;
        }
        collectAccessPath(firstChild(node), outPath);
        if (outPath.StepCount >= internal::MAX_ACCESS_STEPS)
        {
            return;
        }
        internal::AccessStep step;
        if (kind == SyntaxKind::MemberExpression)
        {
            const uint32 field = mModel.NodeSymbols[node];
            if (field != NO_SYMBOL)
            {
                step.Kind = internal::AccessStepKind::Field;
                step.Value = field;
            }
            else
            {
                bool hasRepeat = false;
                step.Kind = internal::AccessStepKind::Swizzle;
                step.Value = GetSwizzleMask(spellNode(node), MAX_SWIZZLE_COMPONENTS, hasRepeat);
            }
        }
        else
        {
            const uint32 index = nextSibling(firstChild(node));
            const IntegerConstant value = getConstant(index);
            step.Kind = value.IsKnown == true ? internal::AccessStepKind::ConstantIndex
                                              : internal::AccessStepKind::DynamicIndex;
            step.Value = value.IsKnown == true ? static_cast<uint32>(value.Value) : 0;
        }
        outPath.Steps[outPath.StepCount] = step;
        ++outPath.StepCount;
    }

    /// True when `node` is built only from literals, constants and constructions of constants.
    [[nodiscard]] bool isConstantExpression(uint32 node) const noexcept
    {
        if (node == NO_NODE)
        {
            return true;
        }
        switch (getKind(node))
        {
            case SyntaxKind::NumberLiteral:
            case SyntaxKind::BooleanLiteral:
            case SyntaxKind::ScopeExpression:
                return true;
            case SyntaxKind::Name: {
                const uint32 symbol = mModel.NodeSymbols[node];
                return symbol != NO_SYMBOL && mModel.Symbols[symbol].Kind == SymbolKind::Variable &&
                       mModel.Symbols[symbol].IsConstant == true;
            }
            case SyntaxKind::CallExpression: {
                if (mModel.NodeCallKinds[node] == CallKind::Function || mModel.NodeCallKinds[node] == CallKind::Method)
                {
                    return false;
                }
                for (uint32 argument = nextSibling(firstChild(node)); argument != NO_NODE;
                     argument = nextSibling(argument))
                {
                    if (isConstantExpression(firstChild(argument)) == false)
                    {
                        return false;
                    }
                }
                return true;
            }
            case SyntaxKind::ParenthesizedExpression:
            case SyntaxKind::UnaryExpression:
            case SyntaxKind::BinaryExpression:
            case SyntaxKind::ConditionalExpression:
            case SyntaxKind::MemberExpression:
            case SyntaxKind::IndexExpression:
            case SyntaxKind::InitializerList:
            case SyntaxKind::FieldInitializer: {
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    if (isConstantExpression(child) == false)
                    {
                        return false;
                    }
                }
                return true;
            }
            default:
                return false;
        }
    }

    const PreprocessRequest& mRequest;
    const PreprocessResult& mInput;
    const SyntaxTree& mTree;
    CheckOptions mOptions;
    SemanticModel& mModel;
    internal::DiagnosticSink mSink;
    TypeTable mTypes;
    ScopeTable mScopes;
    Array<uint32> mNextOverloads;
    FunctionContext mFunction;
    uint32 mIntrinsicCount = 0;
    uint32 mIncompleteStruct = NO_SYMBOL;
    /// The next Members slot that checkStruct reserved for a method of the struct being checked.
    uint32 mNextMethodSlot = 0;
    bool mIsInExtern = false;
    bool mIsFatal = false;
};
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

Status Check(const PreprocessRequest& request,
             const PreprocessResult& preprocessed,
             const SyntaxTree& tree,
             const CheckOptions& options,
             SemanticModel& outModel) noexcept
{
    outModel = SemanticModel{};
    Checker checker(request, preprocessed, tree, options, outModel);
    return checker.Run();
}

Status Check(const PreprocessRequest& request,
             const PreprocessResult& preprocessed,
             const SyntaxTree& tree,
             SemanticModel& outModel) noexcept
{
    return Check(request, preprocessed, tree, CheckOptions{}, outModel);
}

TypeNameText FormatTypeName(const SemanticModel& model,
                            uint32 type,
                            const PreprocessRequest& request,
                            const PreprocessResult& preprocessed) noexcept
{
    TypeNameText text;
    const auto append = [&text](std::string_view part) noexcept { // ludus-style: inferred-type: lambda closure
        const usize available = TypeNameText::GetCapacity() - text.GetSize();
        (void)text.TryAppend(part.substr(0, part.size() < available ? part.size() : available));
    };
    const auto appendNumber = [&append](uint32 value) noexcept { // ludus-style: inferred-type: lambda closure
        constexpr usize MAX_DIGITS = 10;
        constexpr uint32 DECIMAL_BASE = 10;
        foundation::StaticArray<char, MAX_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[MAX_DIGITS - 1 - count] = static_cast<char>('0' + value % DECIMAL_BASE);
            value /= DECIMAL_BASE;
            ++count;
        } while (value != 0);
        append(std::string_view(digits.GetData() + (MAX_DIGITS - count), count));
    };
    if (type >= model.Types.GetSize())
    {
        append("<error>");
        return text;
    }
    foundation::StaticArray<uint32, MAX_SYNTAX_DEPTH> dimensions{};
    usize dimensionCount = 0;
    while (model.Types[type].Kind == TypeKind::Array && model.Types[type].Element < model.Types.GetSize())
    {
        if (dimensionCount < dimensions.GetSize())
        {
            dimensions[dimensionCount] = model.Types[type].Count;
            ++dimensionCount;
        }
        type = model.Types[type].Element;
    }
    const SemanticType& shape = model.Types[type];
    switch (shape.Kind)
    {
        case TypeKind::Error:
            append("<error>");
            break;
        case TypeKind::Void:
            append("void");
            break;
        case TypeKind::UntypedInteger:
            append("an integer constant");
            break;
        case TypeKind::UntypedFloat:
            append("a floating constant");
            break;
        case TypeKind::Scalar:
            append(internal::GetScalarName(shape.Scalar));
            break;
        case TypeKind::Vector:
            append(internal::GetScalarName(shape.Scalar));
            append("x");
            appendNumber(shape.Rows);
            break;
        case TypeKind::Matrix:
            append(internal::GetScalarName(shape.Scalar));
            append("x");
            appendNumber(shape.Rows);
            append("x");
            appendNumber(shape.Columns);
            break;
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Array:
            if (shape.Symbol < model.Symbols.GetSize() &&
                model.Symbols[shape.Symbol].NameToken < preprocessed.Tokens.GetSize())
            {
                append(GetTokenSpelling(request,
                                        preprocessed,
                                        preprocessed.Tokens[model.Symbols[shape.Symbol].NameToken]));
            }
            break;
    }
    for (usize i = 0; i < dimensionCount; ++i)
    {
        append("[");
        appendNumber(dimensions[i]);
        append("]");
    }
    return text;
}
} // namespace ludus::tooling::shader_language
