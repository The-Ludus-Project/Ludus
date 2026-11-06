// Flow rules over the structured LUSL syntax tree. Definite assignment follows the "definitely
// assigned" analysis of the ECMA-334 C# Language Specification (section 9.4): every path from a
// declaration to a read must write the variable. LUSL has no goto, so the analysis walks the tree
// once, joining branch states by intersection; loops need no fixed point because a path's
// assigned set only grows.

#include "internal/analysis.hpp"

#include <ludus/foundation/base/core.h>

#include "internal/fix_builder.hpp"
#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

/// Bits per word of an assignment set.
constexpr uint32 BITS_PER_WORD = 64;
/// TrackedOfSymbol value of a symbol the analysis does not track.
constexpr uint32 UNTRACKED = 0xffffffffU;

/// Which tracked variables are definitely assigned on the paths reaching a point.
struct FlowState final
{
    Array<uint64> Words;
    bool IsReachable = true;
};

/// Statements a `break` or `continue` leaves, and the states they leave with.
struct JumpTarget final
{
    bool IsLoop = false;
    FlowState Breaks;
    bool HasBreak = false;
    FlowState Continues;
    bool HasContinue = false;
};

// The analyzer passes syntax-node and symbol indices, separate uint32 index spaces whose parameter
// names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
class FlowAnalyzer final
{
public:
    FlowAnalyzer(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept : mInputs(inputs), mSink(inoutSink)
    {
    }

    [[nodiscard]] bool Run() noexcept
    {
        const SemanticModel& model = mInputs.Model;
        if (mTrackedOfSymbol.TryResize(model.Symbols.GetSize(), UNTRACKED) == false ||
            mIsReported.TryResize(model.Symbols.GetSize(), 0) == false)
        {
            return false;
        }
        for (uint32 node = firstChild(0); node != NO_NODE && mIsOutOfMemory == false; node = nextSibling(node))
        {
            visitDeclaration(node);
        }
        checkRecursion();
        return mIsOutOfMemory == false;
    }

private:
    // ----- Tree access ---------------------------------------------------------------------------

    [[nodiscard]] const SyntaxNode& getNode(uint32 node) const noexcept
    {
        return mInputs.Tree.Nodes[node];
    }

    [[nodiscard]] uint32 firstChild(uint32 node) const noexcept
    {
        return mInputs.Tree.Nodes[node].FirstChild;
    }

    [[nodiscard]] uint32 nextSibling(uint32 node) const noexcept
    {
        return mInputs.Tree.Nodes[node].NextSibling;
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

    [[nodiscard]] uint32 getSymbol(uint32 node) const noexcept
    {
        return node == NO_NODE ? NO_SYMBOL : mInputs.Model.NodeSymbols[node];
    }

    [[nodiscard]] bool isConstantTrue(uint32 condition) const noexcept
    {
        if (condition == NO_NODE || getKind(condition) == SyntaxKind::Empty)
        {
            return true;
        }
        const IntegerConstant value = mInputs.Model.NodeIntegerConstants[condition];
        return value.IsKnown == true && value.Value != 0;
    }

    // ----- Diagnostics ---------------------------------------------------------------------------

    void report(DiagnosticCode code, uint32 token, const DiagnosticMessage& message, const DiagnosticFix& fix) noexcept
    {
        SourceLocation location;
        SourceLocation expansion;
        if (token != NO_TOKEN && token < mInputs.Preprocessed.Tokens.GetSize())
        {
            const Token& spelled = mInputs.Preprocessed.Tokens[token];
            location = spelled.Location;
            if (spelled.ExpansionIndex != NO_EXPANSION &&
                spelled.ExpansionIndex < mInputs.Preprocessed.Expansions.GetSize())
            {
                expansion = mInputs.Preprocessed.Expansions[spelled.ExpansionIndex];
            }
        }
        mSink.Report(Severity::Error, code, location, expansion, message, fix);
    }

    /// The fix ` = 0` before the `;` of declaration `node`, when its type has a zero value.
    [[nodiscard]] DiagnosticFix makeZeroFix(uint32 node, uint32 symbol) const noexcept
    {
        const SemanticModel& model = mInputs.Model;
        const uint32 type = model.Symbols[symbol].Type;
        const SemanticType& shape = model.Types[type];
        const bool isOpaque = shape.Kind == TypeKind::Struct && model.Symbols[shape.Symbol].IsOpaque == true;
        if (isOpaque == true || spell(getNode(node).LastToken) != ";")
        {
            return DiagnosticFix{};
        }
        FixBuilder fix(mInputs.Request, mInputs.Preprocessed);
        fix.InsertBefore(getNode(node).LastToken, " = 0");
        return fix.GetFix();
    }

    // ----- States --------------------------------------------------------------------------------

    [[nodiscard]] bool makeState(FlowState& outState) noexcept
    {
        outState.IsReachable = true;
        if (outState.Words.TryResize(mWordCount, 0) == false)
        {
            mIsOutOfMemory = true;
            return false;
        }
        for (uint64& word : outState.Words)
        {
            word = 0;
        }
        return true;
    }

    void copyState(const FlowState& source, FlowState& outState) noexcept
    {
        outState.IsReachable = source.IsReachable;
        if (outState.Words.TryResize(source.Words.GetSize(), 0) == false)
        {
            mIsOutOfMemory = true;
            return;
        }
        for (usize i = 0; i < source.Words.GetSize(); ++i)
        {
            outState.Words[i] = source.Words[i];
        }
    }

    /// `inoutState` becomes the state of a point reached from it or from `other`.
    void joinState(FlowState& inoutState, const FlowState& other) noexcept
    {
        if (other.IsReachable == false)
        {
            return;
        }
        if (inoutState.IsReachable == false)
        {
            copyState(other, inoutState);
            return;
        }
        for (usize i = 0; i < inoutState.Words.GetSize() && i < other.Words.GetSize(); ++i)
        {
            inoutState.Words[i] &= other.Words[i];
        }
    }

    [[nodiscard]] static bool isAssigned(const FlowState& state, uint32 tracked) noexcept
    {
        return (state.Words[tracked / BITS_PER_WORD] & (uint64{1} << (tracked % BITS_PER_WORD))) != 0;
    }

    static void setAssigned(FlowState& inoutState, uint32 tracked, bool isAssignedNow) noexcept
    {
        const uint64 bit = uint64{1} << (tracked % BITS_PER_WORD);
        if (isAssignedNow == true)
        {
            inoutState.Words[tracked / BITS_PER_WORD] |= bit;
        }
        else
        {
            inoutState.Words[tracked / BITS_PER_WORD] &= ~bit;
        }
    }

    [[nodiscard]] uint32 getTracked(uint32 symbol) const noexcept
    {
        return symbol == NO_SYMBOL || symbol >= mTrackedOfSymbol.GetSize() ? UNTRACKED : mTrackedOfSymbol[symbol];
    }

    // ----- Declarations --------------------------------------------------------------------------

    void visitDeclaration(uint32 node) noexcept
    {
        switch (getKind(node))
        {
            case SyntaxKind::FunctionDeclaration:
                analyzeFunction(node);
                return;
            case SyntaxKind::StructDeclaration:
                for (uint32 member = firstChild(node); member != NO_NODE; member = nextSibling(member))
                {
                    if (getKind(member) == SyntaxKind::FunctionDeclaration)
                    {
                        analyzeFunction(member);
                    }
                }
                return;
            case SyntaxKind::VariableDeclaration:
                checkGroupshared(node);
                return;
            default:
                // `extern slang` blocks declare no bodies; other declarations have no flow.
                return;
        }
    }

    [[nodiscard]] uint32 getInitializer(uint32 declaration) const noexcept
    {
        uint32 child = skipAttributes(firstChild(declaration));
        child = child == NO_NODE ? NO_NODE : nextSibling(child);
        while (child != NO_NODE && getKind(child) == SyntaxKind::ArraySize)
        {
            child = nextSibling(child);
        }
        return child;
    }

    void checkGroupshared(uint32 node) noexcept
    {
        if (getNode(node).IsGroupshared == false)
        {
            return;
        }
        const uint32 initializer = getInitializer(node);
        const bool isZero = initializer != NO_NODE && getKind(initializer) == SyntaxKind::NumberLiteral &&
                            spell(getNode(initializer).Token) == "0";
        const bool isVoid = initializer != NO_NODE && getKind(initializer) == SyntaxKind::VoidInitializer;
        if (isZero == true || isVoid == true)
        {
            return;
        }
        const uint32 symbol = getSymbol(node);
        const DiagnosticFix fix =
            initializer == NO_NODE && symbol != NO_SYMBOL ? makeZeroFix(node, symbol) : DiagnosticFix{};
        report(DiagnosticCode::GroupsharedInitializer,
               getNode(node).Token,
               ComposeMessage("declare 'groupshared ",
                              spell(getNode(node).Token),
                              "' with '= 0' (cleared before the group runs) or '= void' (left uninitialized)"),
               fix);
    }

    /// Track the locals declared without an initializer, and the `out` parameters, of `function`.
    void collectTracked(uint32 node, Array<uint32>& inoutTracked) noexcept
    {
        for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::VariableDeclaration && getInitializer(child) == NO_NODE)
            {
                const uint32 symbol = getSymbol(child);
                if (symbol != NO_SYMBOL && inoutTracked.TryAdd(symbol) == false)
                {
                    mIsOutOfMemory = true;
                    return;
                }
            }
            collectTracked(child, inoutTracked);
        }
    }

    void analyzeFunction(uint32 node) noexcept
    {
        const uint32 function = getSymbol(node);
        uint32 body = NO_NODE;
        Array<uint32> tracked;
        Array<uint32> outParameters;
        for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Block)
            {
                body = child;
            }
            else if (getKind(child) == SyntaxKind::Parameter && getNode(child).Direction == ParameterDirection::Out)
            {
                const uint32 parameter = getSymbol(child);
                if (parameter != NO_SYMBOL &&
                    (tracked.TryAdd(parameter) == false || outParameters.TryAdd(parameter) == false))
                {
                    mIsOutOfMemory = true;
                    return;
                }
            }
        }
        if (body == NO_NODE || function == NO_SYMBOL)
        {
            return;
        }
        collectTracked(body, tracked);
        for (usize i = 0; i < tracked.GetSize(); ++i)
        {
            mTrackedOfSymbol[tracked[i]] = static_cast<uint32>(i);
        }
        mWordCount = (tracked.GetSize() + BITS_PER_WORD - 1) / BITS_PER_WORD;
        mOutParametersOrNull = &outParameters;
        mCallees.Clear();
        FlowState state;
        if (makeState(state) == true)
        {
            visitStatement(body, state);
            const uint32 returnType = mInputs.Model.Symbols[function].Type;
            if (state.IsReachable == true)
            {
                if (mInputs.Model.Types[returnType].Kind != TypeKind::Void)
                {
                    report(DiagnosticCode::MissingReturn,
                           getNode(node).Token,
                           ComposeMessage("'",
                                          spell(getNode(node).Token),
                                          "' can reach its end without returning a value"),
                           DiagnosticFix{});
                }
                checkOutParameters(state, getNode(node).Token);
            }
        }
        recordCallees(function);
        for (const uint32 symbol : tracked)
        {
            mTrackedOfSymbol[symbol] = UNTRACKED;
        }
        mOutParametersOrNull = nullptr;
    }

    void checkOutParameters(const FlowState& state, uint32 token) noexcept
    {
        if (mOutParametersOrNull == nullptr)
        {
            return;
        }
        for (const uint32 parameter : *mOutParametersOrNull)
        {
            const uint32 tracked = getTracked(parameter);
            if (tracked == UNTRACKED || isAssigned(state, tracked) == true || mIsReported[parameter] != 0)
            {
                continue;
            }
            mIsReported[parameter] = 1;
            const std::string_view name = spell(mInputs.Model.Symbols[parameter].NameToken);
            report(DiagnosticCode::OutParameterNotAssigned,
                   token,
                   ComposeMessage("out parameter '", name, "' may be unwritten here; assign it on every path"),
                   DiagnosticFix{});
        }
    }

    // ----- Statements ----------------------------------------------------------------------------

    void visitStatement(uint32 node, FlowState& inoutState) noexcept
    {
        if (node == NO_NODE || inoutState.IsReachable == false || mIsOutOfMemory == true)
        {
            return;
        }
        switch (getKind(node))
        {
            case SyntaxKind::Block:
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    visitStatement(child, inoutState);
                }
                return;
            case SyntaxKind::VariableDeclaration: {
                const uint32 initializer = getInitializer(node);
                if (initializer != NO_NODE && getKind(initializer) != SyntaxKind::VoidInitializer)
                {
                    visitExpression(initializer, inoutState);
                }
                const uint32 tracked = getTracked(getSymbol(node));
                if (tracked != UNTRACKED)
                {
                    setAssigned(inoutState, tracked, false);
                }
                return;
            }
            case SyntaxKind::ExpressionStatement:
                visitExpression(firstChild(node), inoutState);
                return;
            case SyntaxKind::AssignmentStatement: {
                const uint32 target = firstChild(node);
                const uint32 value = target == NO_NODE ? NO_NODE : nextSibling(target);
                if (spell(getNode(node).Token) != "=")
                {
                    visitExpression(target, inoutState);
                }
                visitExpression(value, inoutState);
                assignTarget(target, inoutState);
                return;
            }
            case SyntaxKind::IncrementStatement:
                visitExpression(firstChild(node), inoutState);
                assignTarget(firstChild(node), inoutState);
                return;
            case SyntaxKind::IfStatement:
                visitIf(node, inoutState);
                return;
            case SyntaxKind::ForStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement:
                visitLoop(node, inoutState);
                return;
            case SyntaxKind::SwitchStatement:
                visitSwitch(node, inoutState);
                return;
            case SyntaxKind::BreakStatement:
            case SyntaxKind::ContinueStatement:
                recordJump(getKind(node) == SyntaxKind::BreakStatement, inoutState);
                inoutState.IsReachable = false;
                return;
            case SyntaxKind::ReturnStatement:
                visitExpression(firstChild(node), inoutState);
                checkOutParameters(inoutState, getNode(node).FirstToken);
                inoutState.IsReachable = false;
                return;
            case SyntaxKind::DiscardStatement:
                inoutState.IsReachable = false;
                return;
            default:
                return;
        }
    }

    void recordJump(bool isBreak, const FlowState& state) noexcept
    {
        for (usize i = mJumpTargets.GetSize(); i > 0; --i)
        {
            JumpTarget& target = mJumpTargets[i - 1];
            if (isBreak == false && target.IsLoop == false)
            {
                continue;
            }
            FlowState& recorded = isBreak == true ? target.Breaks : target.Continues;
            bool& hasRecorded = isBreak == true ? target.HasBreak : target.HasContinue;
            if (hasRecorded == false)
            {
                copyState(state, recorded);
                hasRecorded = true;
            }
            else
            {
                joinState(recorded, state);
            }
            return;
        }
    }

    void visitIf(uint32 node, FlowState& inoutState) noexcept
    {
        const uint32 condition = skipAttributes(firstChild(node));
        const uint32 whenTrue = condition == NO_NODE ? NO_NODE : nextSibling(condition);
        const uint32 whenFalse = whenTrue == NO_NODE ? NO_NODE : nextSibling(whenTrue);
        visitExpression(condition, inoutState);
        FlowState trueState;
        copyState(inoutState, trueState);
        visitStatement(whenTrue, trueState);
        if (whenFalse != NO_NODE)
        {
            visitStatement(whenFalse, inoutState);
        }
        if (inoutState.IsReachable == false)
        {
            copyState(trueState, inoutState);
            return;
        }
        joinState(inoutState, trueState);
    }

    [[nodiscard]] bool pushJumpTarget(bool isLoop) noexcept
    {
        JumpTarget target;
        target.IsLoop = isLoop;
        if (mJumpTargets.TryAdd(foundation::Move(target)) == false)
        {
            mIsOutOfMemory = true;
            return false;
        }
        return true;
    }

    /// Leave a jump target: the state after it joins `exit` with the recorded breaks.
    void popJumpTarget(FlowState& inoutExit) noexcept
    {
        JumpTarget& target = mJumpTargets.GetLast();
        if (target.HasBreak == true)
        {
            joinState(inoutExit, target.Breaks);
        }
        mJumpTargets.RemoveLast();
    }

    void visitLoop(uint32 node, FlowState& inoutState) noexcept
    {
        const SyntaxKind kind = getKind(node);
        uint32 child = skipAttributes(firstChild(node));
        uint32 condition = NO_NODE;
        uint32 increment = NO_NODE;
        uint32 body = NO_NODE;
        if (kind == SyntaxKind::ForStatement)
        {
            const uint32 initializer = child;
            condition = initializer == NO_NODE ? NO_NODE : nextSibling(initializer);
            increment = condition == NO_NODE ? NO_NODE : nextSibling(condition);
            body = increment == NO_NODE ? NO_NODE : nextSibling(increment);
            if (getKind(initializer) != SyntaxKind::Empty)
            {
                visitStatement(initializer, inoutState);
            }
        }
        else if (kind == SyntaxKind::WhileStatement)
        {
            condition = child;
            body = condition == NO_NODE ? NO_NODE : nextSibling(condition);
        }
        else
        {
            body = child;
            condition = body == NO_NODE ? NO_NODE : nextSibling(body);
        }
        if (pushJumpTarget(true) == false)
        {
            return;
        }
        if (kind != SyntaxKind::DoStatement)
        {
            if (getKind(condition) != SyntaxKind::Empty)
            {
                visitExpression(condition, inoutState);
            }
            FlowState bodyState;
            copyState(inoutState, bodyState);
            visitStatement(body, bodyState);
            JumpTarget& target = mJumpTargets.GetLast();
            if (target.HasContinue == true)
            {
                joinState(bodyState, target.Continues);
            }
            if (increment != NO_NODE && getKind(increment) != SyntaxKind::Empty)
            {
                visitStatement(increment, bodyState);
            }
            // The loop may run zero times; with a constant true condition only a break leaves it.
            if (isConstantTrue(condition) == true)
            {
                inoutState.IsReachable = false;
            }
            popJumpTarget(inoutState);
            return;
        }
        visitStatement(body, inoutState);
        JumpTarget& target = mJumpTargets.GetLast();
        if (target.HasContinue == true)
        {
            joinState(inoutState, target.Continues);
        }
        if (inoutState.IsReachable == true)
        {
            visitExpression(condition, inoutState);
        }
        if (isConstantTrue(condition) == true)
        {
            inoutState.IsReachable = false;
        }
        popJumpTarget(inoutState);
    }

    void visitSwitch(uint32 node, FlowState& inoutState) noexcept
    {
        const uint32 selector = skipAttributes(firstChild(node));
        visitExpression(selector, inoutState);
        FlowState entry;
        copyState(inoutState, entry);
        if (pushJumpTarget(false) == false)
        {
            return;
        }
        bool hasDefault = false;
        FlowState exit;
        exit.IsReachable = false;
        for (uint32 clause = selector == NO_NODE ? NO_NODE : nextSibling(selector); clause != NO_NODE;
             clause = nextSibling(clause))
        {
            uint32 statement = firstChild(clause);
            if (spell(getNode(clause).Token) == "case")
            {
                statement = statement == NO_NODE ? NO_NODE : nextSibling(statement);
            }
            else
            {
                hasDefault = true;
            }
            FlowState clauseState;
            copyState(entry, clauseState);
            uint32 last = NO_NODE;
            for (; statement != NO_NODE; statement = nextSibling(statement))
            {
                visitStatement(statement, clauseState);
                last = statement;
            }
            const bool isLastClause = nextSibling(clause) == NO_NODE;
            const bool isFallthrough = last != NO_NODE && getKind(last) == SyntaxKind::FallthroughStatement;
            if (clauseState.IsReachable == true && isLastClause == false && isFallthrough == false)
            {
                report(DiagnosticCode::InvalidSwitch,
                       getNode(clause).Token,
                       ComposeMessage("end this case with 'break', 'return' or '[[fallthrough]]'"),
                       DiagnosticFix{});
            }
            if (isLastClause == true)
            {
                joinState(exit, clauseState);
            }
        }
        if (hasDefault == false)
        {
            joinState(exit, entry);
        }
        popJumpTarget(exit);
        copyState(exit, inoutState);
    }

    // ----- Expressions ---------------------------------------------------------------------------

    /// The variable a whole-variable target names, or NO_SYMBOL for a part of one.
    [[nodiscard]] uint32 getWholeVariable(uint32 target) const noexcept
    {
        while (getKind(target) == SyntaxKind::ParenthesizedExpression)
        {
            target = firstChild(target);
        }
        return getKind(target) == SyntaxKind::Name ? getSymbol(target) : NO_SYMBOL;
    }

    /// The variable at the root of a member or element target.
    [[nodiscard]] uint32 getRootNode(uint32 target) const noexcept
    {
        while (getKind(target) == SyntaxKind::ParenthesizedExpression ||
               getKind(target) == SyntaxKind::MemberExpression || getKind(target) == SyntaxKind::IndexExpression)
        {
            target = firstChild(target);
        }
        return getKind(target) == SyntaxKind::Name ? target : NO_NODE;
    }

    void reportUnassigned(uint32 nameNode, uint32 symbol, bool isPartialWrite) noexcept
    {
        if (mIsReported[symbol] != 0)
        {
            return;
        }
        mIsReported[symbol] = 1;
        const Symbol& declared = mInputs.Model.Symbols[symbol];
        const std::string_view name = spell(declared.NameToken);
        const DiagnosticFix fix =
            declared.Kind == SymbolKind::Variable ? makeZeroFix(declared.Declaration, symbol) : DiagnosticFix{};
        if (isPartialWrite == true)
        {
            report(DiagnosticCode::UseBeforeAssignment,
                   getNode(nameNode).Token,
                   ComposeMessage("'",
                                  name,
                                  "' is written in part before it is assigned; initialize it with '= 0', "
                                  "or declare it '= void'"),
                   fix);
            return;
        }
        report(DiagnosticCode::UseBeforeAssignment,
               getNode(nameNode).Token,
               ComposeMessage("'", name, "' may be read before it is assigned; initialize it with '= 0'"),
               fix);
    }

    /// Record a write of `target`, after its subexpressions have been read.
    void assignTarget(uint32 target, FlowState& inoutState) noexcept
    {
        const uint32 whole = getWholeVariable(target);
        const uint32 tracked = getTracked(whole);
        if (tracked != UNTRACKED)
        {
            setAssigned(inoutState, tracked, true);
            return;
        }
        if (whole != NO_SYMBOL)
        {
            return;
        }
        // A part of a variable: its index expressions are read, and the variable must exist.
        readTargetIndices(target, inoutState);
        const uint32 root = getRootNode(target);
        const uint32 rootTracked = getTracked(getSymbol(root));
        if (rootTracked != UNTRACKED && isAssigned(inoutState, rootTracked) == false)
        {
            reportUnassigned(root, getSymbol(root), true);
        }
    }

    void readTargetIndices(uint32 target, FlowState& inoutState) noexcept
    {
        while (getKind(target) == SyntaxKind::ParenthesizedExpression ||
               getKind(target) == SyntaxKind::MemberExpression || getKind(target) == SyntaxKind::IndexExpression)
        {
            if (getKind(target) == SyntaxKind::IndexExpression)
            {
                visitExpression(nextSibling(firstChild(target)), inoutState);
            }
            target = firstChild(target);
        }
    }

    void visitExpression(uint32 node, FlowState& inoutState) noexcept
    {
        if (node == NO_NODE || inoutState.IsReachable == false)
        {
            return;
        }
        switch (getKind(node))
        {
            case SyntaxKind::Name: {
                const uint32 symbol = getSymbol(node);
                const uint32 tracked = getTracked(symbol);
                if (tracked != UNTRACKED && isAssigned(inoutState, tracked) == false)
                {
                    reportUnassigned(node, symbol, false);
                }
                return;
            }
            case SyntaxKind::CallExpression:
                visitCall(node, inoutState);
                return;
            case SyntaxKind::ConditionalExpression: {
                const uint32 condition = firstChild(node);
                const uint32 whenTrue = condition == NO_NODE ? NO_NODE : nextSibling(condition);
                const uint32 whenFalse = whenTrue == NO_NODE ? NO_NODE : nextSibling(whenTrue);
                visitExpression(condition, inoutState);
                FlowState trueState;
                copyState(inoutState, trueState);
                visitExpression(whenTrue, trueState);
                visitExpression(whenFalse, inoutState);
                joinState(inoutState, trueState);
                return;
            }
            case SyntaxKind::ScopeExpression:
                return;
            default:
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    visitExpression(child, inoutState);
                }
                return;
        }
    }

    void visitCall(uint32 node, FlowState& inoutState) noexcept
    {
        const uint32 callee = firstChild(node);
        if (getKind(callee) == SyntaxKind::MemberExpression)
        {
            visitExpression(firstChild(callee), inoutState);
        }
        const CallKind kind = mInputs.Model.NodeCallKinds[node];
        if ((kind == CallKind::Function || kind == CallKind::Method) && getSymbol(node) != NO_SYMBOL &&
            mCallees.TryAdd(Callee{getSymbol(node), node}) == false)
        {
            mIsOutOfMemory = true;
        }
        // Arguments are read left to right; `out` arguments are written after every read.
        for (uint32 argument = callee == NO_NODE ? NO_NODE : nextSibling(callee); argument != NO_NODE;
             argument = nextSibling(argument))
        {
            const uint32 value = firstChild(argument);
            if (getNode(argument).Direction == ParameterDirection::Out)
            {
                readTargetIndices(value, inoutState);
            }
            else
            {
                visitExpression(value, inoutState);
            }
        }
        for (uint32 argument = callee == NO_NODE ? NO_NODE : nextSibling(callee); argument != NO_NODE;
             argument = nextSibling(argument))
        {
            const ParameterDirection direction = getNode(argument).Direction;
            if (direction == ParameterDirection::Out || direction == ParameterDirection::Inout)
            {
                assignTarget(firstChild(argument), inoutState);
            }
        }
    }

    // ----- Recursion -----------------------------------------------------------------------------

    struct Callee final
    {
        uint32 Function = NO_SYMBOL;
        uint32 Call = NO_NODE;
    };

    struct CalleeRange final
    {
        uint32 First = 0;
        uint32 Count = 0;
    };

    void recordCallees(uint32 function) noexcept
    {
        if (mCalleeRanges.GetSize() < mInputs.Model.Symbols.GetSize() &&
            mCalleeRanges.TryResize(mInputs.Model.Symbols.GetSize()) == false)
        {
            mIsOutOfMemory = true;
            return;
        }
        CalleeRange range;
        range.First = static_cast<uint32>(mCallGraph.GetSize());
        for (const Callee& callee : mCallees)
        {
            if (mCallGraph.TryAdd(callee) == false)
            {
                mIsOutOfMemory = true;
                return;
            }
        }
        range.Count = static_cast<uint32>(mCallees.GetSize());
        mCalleeRanges[function] = range;
    }

    void checkRecursion() noexcept
    {
        if (mCalleeRanges.IsEmpty() == true)
        {
            return;
        }
        // Iterative depth-first search: 0 unvisited, 1 on the stack, 2 finished.
        Array<foundation::uint8> colors;
        struct Frame final
        {
            uint32 Function = NO_SYMBOL;
            uint32 Next = 0;
        };
        Array<Frame> stack;
        if (colors.TryResize(mCalleeRanges.GetSize(), 0) == false)
        {
            mIsOutOfMemory = true;
            return;
        }
        constexpr foundation::uint8 ON_STACK = 1;
        constexpr foundation::uint8 FINISHED = 2;
        for (uint32 start = 0; start < mCalleeRanges.GetSize(); ++start)
        {
            if (colors[start] != 0 || mCalleeRanges[start].Count == 0)
            {
                continue;
            }
            if (stack.TryAdd(Frame{start, 0}) == false)
            {
                mIsOutOfMemory = true;
                return;
            }
            colors[start] = ON_STACK;
            while (stack.IsEmpty() == false)
            {
                Frame& frame = stack.GetLast();
                const CalleeRange& range = mCalleeRanges[frame.Function];
                if (frame.Next >= range.Count)
                {
                    colors[frame.Function] = FINISHED;
                    stack.RemoveLast();
                    continue;
                }
                const Callee callee = mCallGraph[range.First + frame.Next];
                ++frame.Next;
                if (callee.Function >= colors.GetSize())
                {
                    continue;
                }
                if (colors[callee.Function] == ON_STACK)
                {
                    const std::string_view name = spell(mInputs.Model.Symbols[callee.Function].NameToken);
                    report(DiagnosticCode::Recursion,
                           getNode(callee.Call).FirstToken,
                           ComposeMessage("this call makes '",
                                          name,
                                          "' recursive; GPU code has no call stack, so "
                                          "rewrite it as a loop"),
                           DiagnosticFix{});
                    continue;
                }
                if (colors[callee.Function] == 0)
                {
                    colors[callee.Function] = ON_STACK;
                    if (stack.TryAdd(Frame{callee.Function, 0}) == false)
                    {
                        mIsOutOfMemory = true;
                        return;
                    }
                }
            }
        }
    }

    const AnalysisInputs& mInputs;
    DiagnosticSink& mSink;
    Array<uint32> mTrackedOfSymbol;
    Array<foundation::uint8> mIsReported;
    Array<JumpTarget> mJumpTargets;
    Array<Callee> mCallees;
    Array<Callee> mCallGraph;
    Array<CalleeRange> mCalleeRanges;
    const Array<uint32>* mOutParametersOrNull = nullptr;
    usize mWordCount = 0;
    bool mIsOutOfMemory = false;
};
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

bool AnalyzeFlow(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept
{
    FlowAnalyzer analyzer(inputs, inoutSink);
    return analyzer.Run();
}
} // namespace ludus::tooling::shader_language::internal
