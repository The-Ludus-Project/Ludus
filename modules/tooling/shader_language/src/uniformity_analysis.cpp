// Uniformity rules over the checked LUSL syntax tree
// (docs/architecture/shading-language.md#uniformity).
//
// The rules follow the uniformity analysis of the W3C WebGPU Shading Language, edited by Alan
// Baker, Mehmet Oguz Derin and David Neto, section "Uniformity Analysis"
// (https://www.w3.org/TR/WGSL/#uniformity): derivative operations need uniform control flow;
// control flow becomes non-uniform at a decision on a varying value and stays so after a `return`
// in non-uniform flow; `discard` demotes the invocation to a helper and leaves later derivatives
// defined. LUSL departs from WGSL in two ways. The parameters of ordinary functions are varying
// unless declared `uniform`, where WGSL infers a requirement per parameter, so a contract is
// visible in the signature. Variables are tracked once per function, not per program point, so a
// variable that ever holds a varying value is varying everywhere in its function.
//
// Each function is summarized first, with its parameters uniform: whether it needs uniform control
// flow (it performs a derivative operation or calls a function that does) and whether its results
// vary for uniform arguments. Summaries are monotone and recursion is rejected earlier, so
// repeating the summaries until none changes terminates. A final pass checks every function with
// its declared parameter uniformity and reports.

#include "internal/analysis.hpp"

#include <ludus/foundation/base/core.h>

#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Why a value differs between invocations.
enum class VaryingKind : uint8
{
    None,           ///< The value is uniform.
    EntryInput,     ///< An entry point's input other than the compute `GroupId`.
    Parameter,      ///< A parameter of an ordinary function not declared `uniform`.
    Field,          ///< A field of a method's own struct.
    WritableMemory, ///< A read of an `RWTexture2D` or `RWStructuredBuffer`.
    Groupshared,    ///< A read of `groupshared` memory.
    MutableGlobal,  ///< A read of a mutable global variable.
};

/// The uniformity of a value: uniform, or varying because of the source it reads.
struct Varying final
{
    VaryingKind Kind = VaryingKind::None;
    /// The name the expression reads, nearest to the use.
    uint32 Read = NO_TOKEN;
    /// The varying source the value comes from.
    uint32 Source = NO_TOKEN;

    [[nodiscard]] bool IsVarying() const noexcept
    {
        return Kind != VaryingKind::None;
    }
};

/// Non-uniform control flow: the token of the deciding expression and the value it decided on.
struct Divergence final
{
    uint32 Condition = NO_TOKEN;
    Varying Cause;

    [[nodiscard]] bool IsDivergent() const noexcept
    {
        return Condition != NO_TOKEN;
    }
};

/// What callers need to know about a function.
struct FunctionSummary final
{
    /// The function performs a derivative operation, or calls a function that does.
    bool IsRequiringUniformFlow = false;
    /// The operation or call in the body that makes it so.
    uint32 RequirementToken = NO_TOKEN;
    /// A result (the return value or an `out`/`inout` parameter) varies for uniform arguments.
    bool IsProducingVarying = false;
    /// Why it varies.
    Varying Produced;
};

/// How a body is analyzed.
enum class Mode : uint8
{
    Summarize, ///< Parameters are uniform; update the function's summary.
    Check,     ///< Parameters are as declared; report.
};

// The analyzer passes syntax-node, token and symbol indices, separate uint32 index spaces whose
// parameter names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
class UniformityAnalyzer final
{
public:
    UniformityAnalyzer(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept
        : mInputs(inputs), mSink(inoutSink)
    {
    }

    [[nodiscard]] bool Run() noexcept
    {
        const SemanticModel& model = mInputs.Model;
        const usize symbolCount = model.Symbols.GetSize();
        const usize nodeCount = mInputs.Tree.Nodes.GetSize();
        if (mSummaries.TryResize(symbolCount) == false || mVarying.TryResize(symbolCount) == false ||
            mIsLocal.TryResize(symbolCount, 0) == false || mRegions.TryResize(symbolCount, NO_TOKEN) == false ||
            mLoopDivergence.TryResize(nodeCount) == false ||
            mReportedTokens.TryResize(mInputs.Preprocessed.Tokens.GetSize(), 0) == false)
        {
            return false;
        }
        if (CollectFunctionBodies(mInputs.Tree, model, mBodies) == false ||
            MakeSpecializedModels(model, mSpecializedModels) == false)
        {
            return false;
        }
        // Each repetition sets at least one summary flag, and there are two per function.
        const usize maximumRounds = 2 * mBodies.GetSize() + 1;
        for (usize round = 0; round < maximumRounds && mIsOutOfMemory == false; ++round)
        {
            mHasSummaryChanged = false;
            for (const FunctionBody& body : mBodies)
            {
                analyzeBody(body, Mode::Summarize);
            }
            if (mHasSummaryChanged == false)
            {
                break;
            }
        }
        for (usize i = 0; i < mBodies.GetSize() && mIsOutOfMemory == false; ++i)
        {
            analyzeBody(mBodies[i], Mode::Check);
        }
        return mIsOutOfMemory == false;
    }

private:
    // ----- Tree and model access -----------------------------------------------------------------

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

    [[nodiscard]] uint64 getLine(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return 0;
        }
        return mInputs.Preprocessed.Tokens[token].Location.Line;
    }

    [[nodiscard]] const SemanticModel& getModel() const noexcept
    {
        return *mModelOrNull;
    }

    [[nodiscard]] uint32 getSymbol(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_SYMBOL : getModel().NodeSymbols[node];
    }

    [[nodiscard]] const SemanticType* getTypeOrNull(uint32 type) const noexcept
    {
        return type < getModel().Types.GetSize() ? &getModel().Types[type] : nullptr;
    }

    /// True when expression `node` has a writable resource type.
    [[nodiscard]] bool isWritableResource(uint32 node) const noexcept
    {
        const SemanticType* typeOrNull = node == NO_NODE ? nullptr : getTypeOrNull(getModel().NodeTypes[node]);
        return typeOrNull != nullptr && typeOrNull->Kind == TypeKind::Resource &&
               IsWritableResource(typeOrNull->Resource) == true;
    }

    // ----- Bodies --------------------------------------------------------------------------------

    // ----- Per-body state ------------------------------------------------------------------------

    void resetBody() noexcept
    {
        for (const uint32 symbol : mTouchedSymbols)
        {
            mVarying[symbol] = Varying{};
            mIsLocal[symbol] = 0;
            mRegions[symbol] = NO_TOKEN;
        }
        mTouchedSymbols.Clear();
        for (const uint32 node : mTouchedNodes)
        {
            mLoopDivergence[node] = Divergence{};
        }
        mTouchedNodes.Clear();
    }

    void touchSymbol(uint32 symbol) noexcept
    {
        if (mTouchedSymbols.TryAdd(symbol) == false)
        {
            mIsOutOfMemory = true;
        }
    }

    /// Mark `symbol` varying for `value`; records a change for the body's fixed point.
    void setVarying(uint32 symbol, const Varying& value) noexcept
    {
        if (symbol >= mVarying.GetSize() || value.IsVarying() == false || mVarying[symbol].IsVarying() == true)
        {
            return;
        }
        touchSymbol(symbol);
        mVarying[symbol] = value;
        mHasBodyChanged = true;
    }

    /// Record that loop or `switch` `node` runs its body in non-uniform control flow.
    void setLoopDivergence(uint32 node, const Divergence& divergence) noexcept
    {
        if (divergence.IsDivergent() == false || mLoopDivergence[node].IsDivergent() == true)
        {
            return;
        }
        if (mTouchedNodes.TryAdd(node) == false)
        {
            mIsOutOfMemory = true;
            return;
        }
        mLoopDivergence[node] = divergence;
        mHasBodyChanged = true;
    }

    [[nodiscard]] bool isComputeGroupId(uint32 parameter) const noexcept
    {
        const uint32 declaration = getModel().Symbols[parameter].Declaration;
        for (uint32 child = firstChild(declaration); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Semantic)
            {
                return spell(getNode(child).Token) == "GroupId";
            }
        }
        return false;
    }

    void initializeParameters(const FunctionBody& body, Mode mode) noexcept
    {
        const Symbol& function = getModel().Symbols[body.Function];
        for (uint32 i = 0; i < function.MemberCount; ++i)
        {
            const uint32 parameter = getModel().Members[function.FirstMember + i];
            const Symbol& declared = getModel().Symbols[parameter];
            touchSymbol(parameter);
            mIsLocal[parameter] = 1;
            if (function.Stage != EntryStage::None)
            {
                if (isComputeGroupId(parameter) == false)
                {
                    mVarying[parameter] = Varying{VaryingKind::EntryInput, declared.NameToken, declared.NameToken};
                }
            }
            else if (mode == Mode::Check && declared.IsUniform == false)
            {
                mVarying[parameter] = Varying{VaryingKind::Parameter, declared.NameToken, declared.NameToken};
            }
        }
    }

    void analyzeBody(const FunctionBody& body, Mode mode) noexcept
    {
        mModelOrNull =
            body.Specialization == NO_SPECIALIZATION ? &mInputs.Model : &mSpecializedModels[body.Specialization];
        mCurrentFunction = body.Function;
        mMode = mode;
        resetBody();
        initializeParameters(body, mode);
        uint32 block = NO_NODE;
        for (uint32 child = firstChild(body.Node); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Block)
            {
                block = child;
            }
        }
        // Every repetition marks one more symbol or loop, so the walk reaches a fixed point.
        const usize maximumWalks = mVarying.GetSize() + mLoopDivergence.GetSize() + 1;
        mIsReporting = false;
        for (usize walk = 0; walk < maximumWalks && mIsOutOfMemory == false; ++walk)
        {
            mHasBodyChanged = false;
            walkBody(block);
            if (mHasBodyChanged == false)
            {
                break;
            }
        }
        if (mode == Mode::Summarize)
        {
            summarize(body);
            return;
        }
        mIsReporting = true;
        walkBody(block);
        mIsReporting = false;
    }

    void walkBody(uint32 block) noexcept
    {
        mFlow = Divergence{};
        mReturnDivergence = Divergence{};
        mReturned = Varying{};
        mJumpTargets.Clear();
        visitStatement(block);
    }

    void summarize(const FunctionBody& body) noexcept
    {
        FunctionSummary& summary = mSummaries[body.Function];
        Varying produced = mReturned;
        const Symbol& function = getModel().Symbols[body.Function];
        for (uint32 i = 0; i < function.MemberCount && produced.IsVarying() == false; ++i)
        {
            const uint32 parameter = getModel().Members[function.FirstMember + i];
            const ParameterDirection direction = getModel().Symbols[parameter].Direction;
            if (direction == ParameterDirection::Out || direction == ParameterDirection::Inout)
            {
                produced = mVarying[parameter];
            }
        }
        if (produced.IsVarying() == true && summary.IsProducingVarying == false)
        {
            summary.IsProducingVarying = true;
            summary.Produced = produced;
            mHasSummaryChanged = true;
        }
    }

    void requireUniformFlow(uint32 token) noexcept
    {
        if (mMode != Mode::Summarize || mCurrentFunction == NO_SYMBOL)
        {
            return;
        }
        FunctionSummary& summary = mSummaries[mCurrentFunction];
        if (summary.IsRequiringUniformFlow == false)
        {
            summary.IsRequiringUniformFlow = true;
            summary.RequirementToken = token;
            mHasSummaryChanged = true;
        }
    }

    // ----- Diagnostics ---------------------------------------------------------------------------

    void report(DiagnosticCode code, uint32 token, const DiagnosticMessage& message) noexcept
    {
        if (mIsReporting == false || token == NO_TOKEN || token >= mReportedTokens.GetSize())
        {
            return;
        }
        // A specialized template reports once, not once per specialization.
        const uint8 bit = code == DiagnosticCode::NonUniformDerivative ? 1 : 2;
        if ((mReportedTokens[token] & bit) != 0)
        {
            return;
        }
        mReportedTokens[token] = static_cast<uint8>(mReportedTokens[token] | bit);
        const Token& spelled = mInputs.Preprocessed.Tokens[token];
        SourceLocation expansion;
        if (spelled.ExpansionIndex != NO_EXPANSION &&
            spelled.ExpansionIndex < mInputs.Preprocessed.Expansions.GetSize())
        {
            expansion = mInputs.Preprocessed.Expansions[spelled.ExpansionIndex];
        }
        mSink.Report(Severity::Error, code, spelled.Location, expansion, message, DiagnosticFix{});
    }

    [[nodiscard]] static std::string_view describe(VaryingKind kind) noexcept
    {
        switch (kind)
        {
            case VaryingKind::EntryInput:
                return "an entry-point input";
            case VaryingKind::Parameter:
                return "a parameter not declared 'uniform'";
            case VaryingKind::Field:
                return "a field of the method's own struct";
            case VaryingKind::WritableMemory:
                return "writable memory";
            case VaryingKind::Groupshared:
                return "groupshared memory";
            case VaryingKind::MutableGlobal:
                return "a mutable global";
            case VaryingKind::None:
                break;
        }
        return "a varying value";
    }

    /// Append "reads 'x' (why)" or "reads 'x', which depends on 'y' (why) at line N".
    void appendCause(DiagnosticMessage& inoutMessage, const Varying& cause) const noexcept
    {
        const std::string_view read = spell(cause.Read);
        const std::string_view source = spell(cause.Source);
        AppendPart(inoutMessage, "reads '");
        AppendPart(inoutMessage, read);
        if (read == source)
        {
            AppendPart(inoutMessage, "' (");
            AppendPart(inoutMessage, describe(cause.Kind));
            AppendPart(inoutMessage, ")");
            return;
        }
        AppendPart(inoutMessage, "', which depends on '");
        AppendPart(inoutMessage, source);
        AppendPart(inoutMessage, "' (");
        AppendPart(inoutMessage, describe(cause.Kind));
        AppendPart(inoutMessage, ") at line ");
        AppendPart(inoutMessage, getLine(cause.Source));
    }

    /// Report a derivative operation or a call at `token` in non-uniform control flow.
    void reportDivergentDerivative(uint32 token, uint32 requirementOrNone, std::string_view repair) noexcept
    {
        if (mFlow.IsDivergent() == false || mIsReporting == false)
        {
            return;
        }
        DiagnosticMessage message = ComposeMessage("'", spell(token), "' needs uniform control flow");
        if (requirementOrNone != NO_TOKEN)
        {
            AppendPart(message, " because it calls '");
            AppendPart(message, spell(requirementOrNone));
            AppendPart(message, "' at line ");
            AppendPart(message, getLine(requirementOrNone));
        }
        AppendPart(message, ", but it runs after the decision at line ");
        AppendPart(message, getLine(mFlow.Condition));
        AppendPart(message, ", which ");
        appendCause(message, mFlow.Cause);
        AppendPart(message, "; ");
        AppendPart(message, repair);
        report(DiagnosticCode::NonUniformDerivative, token, message);
    }

    // ----- Statements ----------------------------------------------------------------------------

    /// Enter a new region of non-uniform control flow when `value` decides it. Nested decisions
    /// open regions of their own, so a local declared in an outer region varies when a nested
    /// region assigns it.
    void decide(uint32 conditionNode, const Varying& value) noexcept
    {
        if (value.IsVarying() == true)
        {
            mFlow = Divergence{getNode(conditionNode).FirstToken, value};
        }
    }

    void visitStatement(uint32 node) noexcept
    {
        if (node == NO_NODE || mIsOutOfMemory == true)
        {
            return;
        }
        switch (getKind(node))
        {
            case SyntaxKind::Block:
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    visitStatement(child);
                    // After a `return` in non-uniform flow, the rest of the function is non-uniform.
                    if (mFlow.IsDivergent() == false && mReturnDivergence.IsDivergent() == true)
                    {
                        mFlow = mReturnDivergence;
                    }
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
                Varying value = evaluate(nextSibling(target));
                if (spell(getNode(node).Token) != "=")
                {
                    value = join(value, evaluate(target));
                }
                assign(target, value);
                return;
            }
            case SyntaxKind::IncrementStatement:
                assign(firstChild(node), evaluate(firstChild(node)));
                return;
            case SyntaxKind::IfStatement:
                visitIf(node);
                return;
            case SyntaxKind::ForStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement:
                visitLoop(node);
                return;
            case SyntaxKind::SwitchStatement:
                visitSwitch(node);
                return;
            case SyntaxKind::BreakStatement:
            case SyntaxKind::ContinueStatement:
                visitJump(getKind(node) == SyntaxKind::BreakStatement);
                return;
            case SyntaxKind::ReturnStatement:
                visitReturn(node);
                return;
            default:
                // `discard` demotes to a helper invocation and leaves the flow's uniformity alone.
                return;
        }
    }

    [[nodiscard]] uint32 getInitializer(uint32 declaration) const noexcept
    {
        uint32 child = skipAttributes(firstChild(declaration));
        child = nextSibling(child);
        while (child != NO_NODE && getKind(child) == SyntaxKind::ArraySize)
        {
            child = nextSibling(child);
        }
        return child;
    }

    void visitDeclaration(uint32 node) noexcept
    {
        const uint32 symbol = getSymbol(node);
        if (symbol == NO_SYMBOL)
        {
            return;
        }
        touchSymbol(symbol);
        mIsLocal[symbol] = 1;
        // A local is uniform in the flow it is declared in; later assignments in more divergent
        // flow make it varying.
        mRegions[symbol] = mFlow.Condition;
        const uint32 initializer = getInitializer(node);
        if (initializer != NO_NODE && getKind(initializer) != SyntaxKind::VoidInitializer)
        {
            setVarying(symbol, evaluate(initializer));
        }
    }

    void visitIf(uint32 node) noexcept
    {
        const uint32 condition = skipAttributes(firstChild(node));
        const uint32 whenTrue = nextSibling(condition);
        const uint32 whenFalse = nextSibling(whenTrue);
        if (getNode(node).IsConstexpr == true)
        {
            const bool isTrue = getModel().NodeIntegerConstants[node].Value != 0;
            visitStatement(isTrue == true ? whenTrue : whenFalse);
            return;
        }
        const Divergence saved = mFlow;
        decide(condition, evaluate(condition));
        const Divergence branch = mFlow;
        visitStatement(whenTrue);
        mFlow = branch;
        visitStatement(whenFalse);
        mFlow = saved;
    }

    [[nodiscard]] bool pushJumpTarget(uint32 node, bool isLoop) noexcept
    {
        if (mJumpTargets.TryAdd(JumpTarget{node, isLoop}) == false)
        {
            mIsOutOfMemory = true;
            return false;
        }
        return true;
    }

    void visitLoop(uint32 node) noexcept
    {
        const SyntaxKind kind = getKind(node);
        const uint32 child = skipAttributes(firstChild(node));
        uint32 condition = NO_NODE;
        uint32 increment = NO_NODE;
        uint32 body = NO_NODE;
        if (kind == SyntaxKind::ForStatement)
        {
            condition = nextSibling(child);
            increment = nextSibling(condition);
            body = nextSibling(increment);
            if (getKind(child) != SyntaxKind::Empty)
            {
                visitStatement(child);
            }
        }
        else if (kind == SyntaxKind::WhileStatement)
        {
            condition = child;
            body = nextSibling(condition);
        }
        else
        {
            body = child;
            condition = nextSibling(body);
        }
        const Divergence saved = mFlow;
        // Every iteration after a divergent exit or condition runs in non-uniform flow.
        if (mLoopDivergence[node].IsDivergent() == true)
        {
            mFlow = mLoopDivergence[node];
        }
        if (pushJumpTarget(node, true) == false)
        {
            return;
        }
        if (kind != SyntaxKind::DoStatement && getKind(condition) != SyntaxKind::Empty)
        {
            decide(condition, evaluate(condition));
            setLoopDivergence(node, mFlow);
        }
        visitStatement(body);
        if (increment != NO_NODE && getKind(increment) != SyntaxKind::Empty)
        {
            visitStatement(increment);
        }
        if (kind == SyntaxKind::DoStatement)
        {
            decide(condition, evaluate(condition));
            setLoopDivergence(node, mFlow);
        }
        mJumpTargets.RemoveLast();
        mFlow = saved;
    }

    void visitSwitch(uint32 node) noexcept
    {
        const uint32 selector = skipAttributes(firstChild(node));
        const Divergence saved = mFlow;
        if (mLoopDivergence[node].IsDivergent() == true)
        {
            mFlow = mLoopDivergence[node];
        }
        decide(selector, evaluate(selector));
        const Divergence clauses = mFlow;
        if (pushJumpTarget(node, false) == false)
        {
            return;
        }
        for (uint32 clause = nextSibling(selector); clause != NO_NODE; clause = nextSibling(clause))
        {
            mFlow = clauses;
            uint32 statement = firstChild(clause);
            if (spell(getNode(clause).Token) == "case")
            {
                statement = nextSibling(statement);
            }
            for (; statement != NO_NODE; statement = nextSibling(statement))
            {
                visitStatement(statement);
            }
        }
        mJumpTargets.RemoveLast();
        mFlow = saved;
    }

    void visitJump(bool isBreak) noexcept
    {
        for (usize i = mJumpTargets.GetSize(); i > 0; --i)
        {
            const JumpTarget& target = mJumpTargets[i - 1];
            if (isBreak == true || target.IsLoop == true)
            {
                setLoopDivergence(target.Node, mFlow);
                return;
            }
        }
    }

    void visitReturn(uint32 node) noexcept
    {
        const Varying value = evaluate(firstChild(node));
        mReturned = join(mReturned, join(value, mFlow.Cause));
        if (mFlow.IsDivergent() == false)
        {
            return;
        }
        if (mReturnDivergence.IsDivergent() == false)
        {
            mReturnDivergence = mFlow;
        }
        for (const JumpTarget& target : mJumpTargets)
        {
            if (target.IsLoop == true)
            {
                setLoopDivergence(target.Node, mFlow);
            }
        }
    }

    // ----- Expressions ---------------------------------------------------------------------------

    [[nodiscard]] static Varying join(const Varying& first, const Varying& second) noexcept
    {
        return first.IsVarying() == true ? first : second;
    }

    /// The uniformity of reading `symbol` at name token `token`.
    [[nodiscard]] Varying readSymbol(uint32 symbol, uint32 token) const noexcept
    {
        if (symbol == NO_SYMBOL || symbol >= mVarying.GetSize())
        {
            return Varying{};
        }
        const Symbol& declared = getModel().Symbols[symbol];
        if (mIsLocal[symbol] != 0)
        {
            const Varying& stored = mVarying[symbol];
            return stored.IsVarying() == true ? Varying{stored.Kind, token, stored.Source} : Varying{};
        }
        if (declared.IsConstant == true || declared.IsExtern == true)
        {
            return Varying{};
        }
        if (declared.Kind == SymbolKind::Field)
        {
            return Varying{VaryingKind::Field, token, token};
        }
        if (declared.Kind != SymbolKind::Variable)
        {
            return Varying{};
        }
        if (declared.IsGroupshared == true)
        {
            return Varying{VaryingKind::Groupshared, token, token};
        }
        const SemanticType* typeOrNull = getTypeOrNull(declared.Type);
        if (typeOrNull != nullptr && typeOrNull->Kind == TypeKind::Resource)
        {
            // A binding is one handle for every invocation; reads of writable ones vary.
            return Varying{};
        }
        return Varying{VaryingKind::MutableGlobal, token, token};
    }

    [[nodiscard]] Varying evaluateChildren(uint32 node) noexcept
    {
        Varying value;
        for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
        {
            value = join(value, evaluate(child));
        }
        return value;
    }

    [[nodiscard]] Varying evaluate(uint32 node) noexcept
    {
        if (node == NO_NODE || mIsOutOfMemory == true)
        {
            return Varying{};
        }
        switch (getKind(node))
        {
            case SyntaxKind::Name:
                return readSymbol(getSymbol(node), getNode(node).Token);
            case SyntaxKind::NumberLiteral:
            case SyntaxKind::BooleanLiteral:
            case SyntaxKind::StringLiteral:
            case SyntaxKind::ScopeExpression:
            case SyntaxKind::ConformanceTest:
            case SyntaxKind::ConceptApplication:
            case SyntaxKind::VoidInitializer:
            case SyntaxKind::Empty:
                return Varying{};
            case SyntaxKind::IndexExpression: {
                const uint32 object = firstChild(node);
                const Varying index = evaluate(nextSibling(object));
                if (isWritableResource(object) == true)
                {
                    const uint32 token = getNode(object).Token;
                    return Varying{VaryingKind::WritableMemory, token, token};
                }
                return join(evaluate(object), index);
            }
            case SyntaxKind::BinaryExpression:
                return evaluateBinary(node);
            case SyntaxKind::ConditionalExpression: {
                const uint32 condition = firstChild(node);
                const uint32 whenTrue = nextSibling(condition);
                const Varying decision = evaluate(condition);
                const Divergence saved = mFlow;
                decide(condition, decision);
                const Varying trueValue = evaluate(whenTrue);
                const Varying falseValue = evaluate(nextSibling(whenTrue));
                mFlow = saved;
                return join(decision, join(trueValue, falseValue));
            }
            case SyntaxKind::CallExpression:
                return evaluateCall(node);
            default:
                return evaluateChildren(node);
        }
    }

    [[nodiscard]] Varying evaluateBinary(uint32 node) noexcept
    {
        const uint32 left = firstChild(node);
        const Varying leftValue = evaluate(left);
        const std::string_view operation = spell(getNode(node).Token);
        if (operation != "&&" && operation != "||")
        {
            return join(leftValue, evaluate(nextSibling(left)));
        }
        // The right operand runs only for some invocations when the left one varies.
        const Divergence saved = mFlow;
        decide(left, leftValue);
        const Varying rightValue = evaluate(nextSibling(left));
        mFlow = saved;
        return join(leftValue, rightValue);
    }

    /// The function symbol a call reaches: its specialization's, for a specialized template.
    [[nodiscard]] uint32 getCallee(uint32 call) const noexcept
    {
        const uint32 function = getSymbol(call);
        if (function == NO_SYMBOL)
        {
            return NO_SYMBOL;
        }
        return ResolveSpecialization(mInputs.Model, function, getModel().NodeTypeArguments[call]);
    }

    [[nodiscard]] Varying evaluateCall(uint32 node) noexcept
    {
        const uint32 callee = firstChild(node);
        const CallKind kind = getModel().NodeCallKinds[node];
        const bool isMember = getKind(callee) == SyntaxKind::MemberExpression;
        const uint32 object = isMember == true ? firstChild(callee) : NO_NODE;
        Varying value = evaluate(object);
        if (kind == CallKind::ResourceMethod && isWritableResource(object) == true)
        {
            const uint32 token = getNode(object).Token;
            value = Varying{VaryingKind::WritableMemory, token, token};
        }
        const uint32 function = kind == CallKind::Function || kind == CallKind::Method ? getCallee(node) : NO_SYMBOL;
        for (uint32 argument = nextSibling(callee); argument != NO_NODE; argument = nextSibling(argument))
        {
            if (getNode(argument).Direction != ParameterDirection::Out)
            {
                const Varying argumentValue = evaluate(firstChild(argument));
                checkUniformArgument(function, argument, argumentValue);
                value = join(value, argumentValue);
            }
        }
        const uint32 token = getNode(callee).Token;
        if (kind == CallKind::Intrinsic && (spell(token) == "ddx" || spell(token) == "ddy"))
        {
            requireUniformFlow(token);
            reportDivergentDerivative(token, NO_TOKEN, "compute the derivative before the branch");
        }
        else if (kind == CallKind::ResourceMethod && spell(token) == "Sample")
        {
            requireUniformFlow(token);
            reportDivergentDerivative(token,
                                      NO_TOKEN,
                                      "move the call before the branch, or use 'SampleLevel' with an explicit level");
        }
        else if (function != NO_SYMBOL && function < mSummaries.GetSize())
        {
            const FunctionSummary& summary = mSummaries[function];
            if (summary.IsRequiringUniformFlow == true)
            {
                requireUniformFlow(token);
                reportDivergentDerivative(token,
                                          summary.RequirementToken,
                                          "call it before the branch, or decide on a uniform value");
            }
            if (summary.IsProducingVarying == true)
            {
                value = join(value, Varying{summary.Produced.Kind, token, summary.Produced.Source});
            }
        }
        // Results written through `out`, `inout` and a mutating method's object.
        for (uint32 argument = nextSibling(callee); argument != NO_NODE; argument = nextSibling(argument))
        {
            const ParameterDirection direction = getNode(argument).Direction;
            if (direction == ParameterDirection::Out || direction == ParameterDirection::Inout)
            {
                assign(firstChild(argument), value);
            }
        }
        if (function != NO_SYMBOL && getModel().Symbols[function].IsMutating == true && object != NO_NODE)
        {
            assign(object, value);
        }
        return value;
    }

    void checkUniformArgument(uint32 function, uint32 argument, const Varying& value) noexcept
    {
        const uint32 position = getModel().NodeParameters[argument];
        if (mIsReporting == false || value.IsVarying() == false || function == NO_SYMBOL || position == NO_SYMBOL)
        {
            return;
        }
        const Symbol& declared = getModel().Symbols[function];
        if (position >= declared.MemberCount)
        {
            return;
        }
        const uint32 parameter = getModel().Members[declared.FirstMember + position];
        if (getModel().Symbols[parameter].IsUniform == false)
        {
            return;
        }
        const std::string_view name = spell(getModel().Symbols[parameter].NameToken);
        DiagnosticMessage message = ComposeMessage("parameter '",
                                                   name,
                                                   "' of '",
                                                   spell(declared.NameToken),
                                                   "' is 'uniform', but this argument ");
        appendCause(message, value);
        AppendPart(message,
                   "; pass a constant, a ConstantBuffer field or a 'uniform' parameter, or remove 'uniform' from '");
        AppendPart(message, name);
        AppendPart(message, "'");
        report(DiagnosticCode::NonUniformArgument, getNode(firstChild(argument)).FirstToken, message);
    }

    /// Record a write of `value` to `target`: its root variable varies when the value, an index
    /// on the way, or the control flow (more divergent than at its declaration) varies.
    void assign(uint32 target, const Varying& value) noexcept
    {
        Varying written = value;
        while (getKind(target) == SyntaxKind::ParenthesizedExpression ||
               getKind(target) == SyntaxKind::MemberExpression || getKind(target) == SyntaxKind::IndexExpression)
        {
            if (getKind(target) == SyntaxKind::IndexExpression)
            {
                written = join(written, evaluate(nextSibling(firstChild(target))));
            }
            target = firstChild(target);
        }
        if (getKind(target) != SyntaxKind::Name)
        {
            return;
        }
        const uint32 symbol = getSymbol(target);
        if (symbol == NO_SYMBOL || symbol >= mIsLocal.GetSize() || mIsLocal[symbol] == 0)
        {
            // Writable memory, `groupshared` memory and fields are varying already.
            return;
        }
        if (mFlow.IsDivergent() == true && mFlow.Condition != mRegions[symbol])
        {
            written = join(written, mFlow.Cause);
        }
        setVarying(symbol, written);
    }

    struct JumpTarget final
    {
        uint32 Node = NO_NODE;
        bool IsLoop = false;
    };

    const AnalysisInputs& mInputs;
    DiagnosticSink& mSink;
    Array<FunctionBody> mBodies;
    Array<SemanticModel> mSpecializedModels;
    Array<FunctionSummary> mSummaries;
    Array<Varying> mVarying;
    Array<uint8> mIsLocal;
    /// For each local, the deciding token of the flow it was declared in.
    Array<uint32> mRegions;
    Array<Divergence> mLoopDivergence;
    Array<uint32> mTouchedSymbols;
    Array<uint32> mTouchedNodes;
    Array<JumpTarget> mJumpTargets;
    Array<uint8> mReportedTokens;
    const SemanticModel* mModelOrNull = nullptr;
    uint32 mCurrentFunction = NO_SYMBOL;
    Divergence mFlow;
    Divergence mReturnDivergence;
    Varying mReturned;
    Mode mMode = Mode::Summarize;
    bool mIsReporting = false;
    bool mHasBodyChanged = false;
    bool mHasSummaryChanged = false;
    bool mIsOutOfMemory = false;
};
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

bool AnalyzeUniformity(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept
{
    UniformityAnalyzer analyzer(inputs, inoutSink);
    return analyzer.Run();
}
} // namespace ludus::tooling::shader_language::internal
