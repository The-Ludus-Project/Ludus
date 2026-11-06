// Lowering from the checked syntax tree to the LUSL IR (docs/architecture/shading-language.md).
// Every rule without a direct Slang spelling is resolved here, so the emitter only spells nodes.

#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/array_growth.hpp"
#include "internal/include_path.hpp"
#include "internal/ir.hpp"
#include "internal/slang_names.hpp"
#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint8;
using foundation::usize;

/// Arguments of `[threads(x, y, z)]`.
constexpr usize THREAD_GROUP_DIMENSIONS = 3;
/// Largest thread-group dimension the IR stores. Slang rejects groups far smaller than this for
/// every target, so storing the clamped value never makes an invalid group valid.
constexpr int64 MAX_THREAD_GROUP_DIMENSION = 0xffffffff;

// Lowering passes syntax-node, IR-node, type and symbol indices, separate uint32 index spaces
// whose parameter names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)

class Lowering final
{
public:
    Lowering(const AnalysisInputs& inputs,
             const EmitOptions& options,
             DiagnosticSink& inoutSink,
             IrModule& outModule) noexcept
        : mInputs(inputs), mOptions(options), mSink(inoutSink), mModule(outModule), mModel(&inputs.Model)
    {
    }

    void Run() noexcept
    {
        mModule = IrModule{};
        findScalarTypes();
        if (addNode(IrKind::Module, NO_NODE) == NO_IR || collectClearedGroupshared() == false)
        {
            return;
        }
        for (uint32 declaration = firstChild(0); declaration != NO_NODE && mIsFatal == false;
             declaration = nextSibling(declaration))
        {
            lowerDeclaration(declaration, 0);
        }
        lowerSpecializations();
    }

private:
    // ----- Syntax and model access ---------------------------------------------------------------

    /// The model whose per-node facts lowering reads: the checked model, or while a specialization
    /// is lowered, its view from MakeSpecializedModel.
    [[nodiscard]] const SemanticModel& getModel() const noexcept
    {
        return *mModel;
    }

    /// Lower each specialization as a function of its own, from the specialization's facts.
    void lowerSpecializations() noexcept
    {
        for (uint32 index = 0; index < mInputs.Model.Specializations.GetSize() && mIsFatal == false; ++index)
        {
            SemanticModel specialized;
            if (MakeSpecializedModel(mInputs.Model, index, specialized) == false)
            {
                failOutOfMemory();
                return;
            }
            mModel = &specialized;
            lowerFunction(mInputs.Model.Specializations[index].FirstNode, 0);
            mModel = &mInputs.Model;
        }
    }

    [[nodiscard]] const SyntaxNode& getSyntax(uint32 node) const noexcept
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

    [[nodiscard]] std::string_view spell(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mInputs.Request, mInputs.Preprocessed, mInputs.Preprocessed.Tokens[token]);
    }

    [[nodiscard]] std::string_view spellNode(uint32 node) const noexcept
    {
        return spell(getSyntax(node).Token);
    }

    [[nodiscard]] uint32 skipAttributes(uint32 child) const noexcept
    {
        while (child != NO_NODE && getKind(child) == SyntaxKind::Attribute)
        {
            child = nextSibling(child);
        }
        return child;
    }

    [[nodiscard]] const SemanticType& getType(uint32 type) const noexcept
    {
        return getModel().Types[type];
    }

    [[nodiscard]] const Symbol& getSymbol(uint32 symbol) const noexcept
    {
        return getModel().Symbols[symbol];
    }

    [[nodiscard]] uint32 typeOf(uint32 node) const noexcept
    {
        return getModel().NodeTypes[node];
    }

    [[nodiscard]] uint32 symbolOf(uint32 node) const noexcept
    {
        return getModel().NodeSymbols[node];
    }

    [[nodiscard]] bool isUntyped(uint32 type) const noexcept
    {
        return type == TypeTable::UNTYPED_INTEGER_TYPE || type == TypeTable::UNTYPED_FLOAT_TYPE;
    }

    /// The location a construct starting at `node` maps to: its first token, or the outermost
    /// macro use when a macro produced it.
    [[nodiscard]] SourceLocation locate(uint32 node) const noexcept
    {
        if (node == NO_NODE)
        {
            return SourceLocation{};
        }
        const uint32 token =
            getSyntax(node).FirstToken != NO_TOKEN ? getSyntax(node).FirstToken : getSyntax(node).Token;
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return SourceLocation{};
        }
        const Token& spelled = mInputs.Preprocessed.Tokens[token];
        if (spelled.ExpansionIndex != NO_EXPANSION &&
            spelled.ExpansionIndex < mInputs.Preprocessed.Expansions.GetSize())
        {
            return mInputs.Preprocessed.Expansions[spelled.ExpansionIndex];
        }
        return spelled.Location;
    }

    void findScalarTypes() noexcept
    {
        for (uint32 type = 0; type < getModel().Types.GetSize(); ++type)
        {
            const SemanticType& shape = getType(type);
            const uint32 kind = static_cast<uint32>(shape.Scalar);
            if (shape.Kind == TypeKind::Scalar && kind < SCALAR_KIND_COUNT && mScalarTypes[kind] == NO_TYPE)
            {
                mScalarTypes[kind] = type;
            }
        }
    }

    [[nodiscard]] uint32 getScalarType(ScalarKind kind) const noexcept
    {
        return mScalarTypes[static_cast<uint32>(kind)];
    }

    /// The scalar element type of a scalar, vector or matrix `type`, or NO_TYPE.
    [[nodiscard]] uint32 getElementScalar(uint32 type) const noexcept
    {
        if (type == NO_TYPE)
        {
            return NO_TYPE;
        }
        const SemanticType& shape = getType(type);
        if (shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector || shape.Kind == TypeKind::Matrix)
        {
            return getScalarType(shape.Scalar);
        }
        return NO_TYPE;
    }

    /// The type an expression is spelled with: its checked type, or for an untyped constant the
    /// scalar of the context it meets, defaulting to int32 or float32.
    [[nodiscard]] uint32 resolveType(uint32 node, uint32 contextType) const noexcept
    {
        const uint32 type = typeOf(node);
        if (isUntyped(type) == false)
        {
            return type;
        }
        const uint32 element = getElementScalar(contextType);
        if (element != NO_TYPE && getType(element).Scalar != ScalarKind::Bool)
        {
            const bool isFloatContext = IsFloatScalar(getType(element).Scalar);
            if (type == TypeTable::UNTYPED_INTEGER_TYPE || isFloatContext == true)
            {
                return element;
            }
        }
        return type == TypeTable::UNTYPED_FLOAT_TYPE ? getScalarType(ScalarKind::Float32)
                                                     : getScalarType(ScalarKind::Int32);
    }

    /// True for a signed integer scalar or vector, whose `+`, `-` and `*` must wrap.
    [[nodiscard]] bool isSignedIntegerValue(uint32 type) const noexcept
    {
        if (type == NO_TYPE)
        {
            return false;
        }
        const SemanticType& shape = getType(type);
        return (shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector) &&
               IsIntegerScalar(shape.Scalar) == true && IsSignedScalar(shape.Scalar) == true;
    }

    // ----- IR construction -----------------------------------------------------------------------

    void fail(DiagnosticCode code, uint32 node, const DiagnosticMessage& message) noexcept
    {
        mSink.ReportError(code, locate(node), SourceLocation{}, message);
        mIsFatal = true;
    }

    void failOutOfMemory() noexcept
    {
        mSink.ReportOutOfMemory();
        mIsFatal = true;
    }

    /// Append a node with `kind`, located at syntax node `origin`; NO_IR on failure.
    [[nodiscard]] uint32 addNode(IrKind kind, uint32 origin) noexcept
    {
        if (mIsFatal == true)
        {
            return NO_IR;
        }
        if (mModule.Nodes.GetSize() >= MAX_IR_NODES)
        {
            fail(DiagnosticCode::LoweringLimit,
                 origin,
                 ComposeMessage("lowering needs more than ", foundation::uint64{MAX_IR_NODES}, " IR nodes"));
            return NO_IR;
        }
        if (TryReserveGrowing(mModule.Nodes, mModule.Nodes.GetSize() + 1) == false)
        {
            failOutOfMemory();
            return NO_IR;
        }
        IrNode node;
        node.Kind = kind;
        node.Location = locate(origin);
        mModule.Nodes.Add(node);
        return static_cast<uint32>(mModule.Nodes.GetSize() - 1);
    }

    [[nodiscard]] IrNode& at(uint32 node) noexcept
    {
        return mModule.Nodes[node];
    }

    void appendChild(uint32 parent, uint32 child) noexcept
    {
        if (parent == NO_IR || child == NO_IR)
        {
            return;
        }
        if (at(parent).LastChild == NO_IR)
        {
            at(parent).FirstChild = child;
        }
        else
        {
            at(at(parent).LastChild).NextSibling = child;
        }
        at(parent).LastChild = child;
    }

    /// Add a node as the last child of `parent`; NO_IR on failure.
    [[nodiscard]] uint32 addChild(uint32 parent, IrKind kind, uint32 origin) noexcept
    {
        const uint32 node = addNode(kind, origin);
        appendChild(parent, node);
        return node;
    }

    /// Find or add a helper equal to `helper` with `fields`; NO_INDEX on failure.
    [[nodiscard]] uint32
    findOrAddHelper(const IrHelper& helper, foundation::BorrowedView<const uint32> fields, uint32 origin) noexcept
    {
        for (uint32 index = 0; index < mModule.Helpers.GetSize(); ++index)
        {
            const IrHelper& existing = mModule.Helpers[index];
            if (existing.Kind != helper.Kind || existing.Operator != helper.Operator || existing.Type != helper.Type ||
                existing.SecondType != helper.SecondType || existing.Symbol != helper.Symbol ||
                existing.FieldCount != fields.GetSize())
            {
                continue;
            }
            bool isSame = true;
            for (uint32 i = 0; i < existing.FieldCount && isSame == true; ++i)
            {
                isSame = mModule.HelperFields[existing.FirstField + i] == fields[i];
            }
            if (isSame == true)
            {
                return index;
            }
        }
        if (mModule.Helpers.GetSize() >= MAX_GENERATED_HELPERS)
        {
            fail(DiagnosticCode::LoweringLimit,
                 origin,
                 ComposeMessage("lowering needs more than ", foundation::uint64{MAX_GENERATED_HELPERS}, " helpers"));
            return NO_INDEX;
        }
        IrHelper added = helper;
        added.FirstField = static_cast<uint32>(mModule.HelperFields.GetSize());
        added.FieldCount = static_cast<uint32>(fields.GetSize());
        if (TryReserveGrowing(mModule.HelperFields, mModule.HelperFields.GetSize() + fields.GetSize()) == false ||
            TryReserveGrowing(mModule.Helpers, mModule.Helpers.GetSize() + 1) == false)
        {
            failOutOfMemory();
            return NO_INDEX;
        }
        for (usize i = 0; i < fields.GetSize(); ++i)
        {
            mModule.HelperFields.Add(fields[i]);
        }
        mModule.Helpers.Add(added);
        return static_cast<uint32>(mModule.Helpers.GetSize() - 1);
    }

    [[nodiscard]] uint32 findOrAddHelper(const IrHelper& helper, uint32 origin) noexcept
    {
        return findOrAddHelper(helper, foundation::BorrowedView<const uint32>{}, origin);
    }

    /// Make sure the zero helper of struct or array `type` exists, with those of its parts.
    [[nodiscard]] bool requireZero(uint32 type, uint32 origin) noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Kind == TypeKind::Array)
        {
            if (requireZero(shape.Element, origin) == false)
            {
                return false;
            }
        }
        else if (shape.Kind == TypeKind::Struct)
        {
            const Symbol& declared = getSymbol(shape.Symbol);
            for (uint32 i = 0; i < declared.MemberCount; ++i)
            {
                const Symbol& member = getSymbol(getModel().Members[declared.FirstMember + i]);
                if (member.Kind == SymbolKind::Field && requireZero(member.Type, origin) == false)
                {
                    return false;
                }
            }
        }
        else
        {
            return true;
        }
        IrHelper helper;
        helper.Kind = IrHelperKind::Zero;
        helper.Type = type;
        return findOrAddHelper(helper, origin) != NO_INDEX;
    }

    // ----- Declarations --------------------------------------------------------------------------

    /// Record the `groupshared` variables declared `= 0`, which entry points clear.
    [[nodiscard]] bool collectClearedGroupshared() noexcept
    {
        for (uint32 declaration = firstChild(0); declaration != NO_NODE; declaration = nextSibling(declaration))
        {
            if (getKind(declaration) != SyntaxKind::VariableDeclaration ||
                getSyntax(declaration).IsGroupshared == false)
            {
                continue;
            }
            const uint32 initializer = findInitializer(declaration);
            if (initializer != NO_NODE && getKind(initializer) == SyntaxKind::NumberLiteral &&
                mClearedGroupshared.TryAdd(symbolOf(declaration)) == false)
            {
                failOutOfMemory();
                return false;
            }
        }
        return true;
    }

    /// The initializer of a variable declaration: the child after the type and array sizes.
    [[nodiscard]] uint32 findInitializer(uint32 declaration) const noexcept
    {
        uint32 child = nextSibling(skipAttributes(firstChild(declaration)));
        while (child != NO_NODE && getKind(child) == SyntaxKind::ArraySize)
        {
            child = nextSibling(child);
        }
        return child;
    }

    void lowerDeclaration(uint32 declaration, uint32 parent) noexcept
    {
        switch (getKind(declaration))
        {
            case SyntaxKind::VariableDeclaration:
                lowerGlobalVariable(declaration, parent);
                return;
            case SyntaxKind::FunctionDeclaration:
                lowerFunction(declaration, parent);
                return;
            case SyntaxKind::StructDeclaration:
                lowerStruct(declaration, parent);
                return;
            case SyntaxKind::InterfaceDeclaration:
                lowerMembers(declaration, addAggregate(IrKind::Interface, declaration, parent));
                return;
            case SyntaxKind::ExtensionDeclaration:
                lowerMembers(declaration, addAggregate(IrKind::Extension, declaration, parent));
                return;
            case SyntaxKind::TemplateDeclaration:
                for (uint32 child = firstChild(declaration); child != NO_NODE; child = nextSibling(child))
                {
                    // A specialized template lowers through its specializations instead.
                    if (getKind(child) == SyntaxKind::FunctionDeclaration && symbolOf(child) != NO_SYMBOL &&
                        getSymbol(symbolOf(child)).IsSpecialized == false)
                    {
                        lowerFunction(child, parent);
                    }
                }
                return;
            case SyntaxKind::EnumDeclaration:
                lowerEnum(declaration, parent);
                return;
            case SyntaxKind::ExternSlangBlock:
                lowerExternBlock(declaration);
                return;
            default:
                return;
        }
    }

    void lowerGlobalVariable(uint32 declaration, uint32 parent) noexcept
    {
        const uint32 symbol = symbolOf(declaration);
        const SyntaxNode& syntax = getSyntax(declaration);
        const bool isResource = getType(getSymbol(symbol).Type).Kind == TypeKind::Resource;
        const IrKind kind = isResource == true             ? IrKind::Resource
                            : syntax.IsGroupshared == true ? IrKind::Groupshared
                                                           : IrKind::Constant;
        const uint32 node = addChild(parent, kind, declaration);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Symbol = symbol;
        at(node).Type = getSymbol(symbol).Type;
        if (kind == IrKind::Constant)
        {
            const uint32 value = lowerValue(findInitializer(declaration), at(node).Type);
            appendChild(node, value);
        }
    }

    void lowerEnum(uint32 declaration, uint32 parent) noexcept
    {
        const uint32 symbol = symbolOf(declaration);
        const uint32 node = addChild(parent, IrKind::Enum, declaration);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Symbol = symbol;
        at(node).Type = getSymbol(symbol).Type;
        const Symbol& declared = getSymbol(symbol);
        for (uint32 i = 0; i < declared.MemberCount; ++i)
        {
            const uint32 enumerator = getModel().Members[declared.FirstMember + i];
            const uint32 child = addChild(node, IrKind::Enumerator, getSymbol(enumerator).Declaration);
            if (child == NO_IR)
            {
                return;
            }
            at(child).Symbol = enumerator;
            at(child).Value = getSymbol(enumerator).IntegerValue;
        }
    }

    [[nodiscard]] uint32 addAggregate(IrKind kind, uint32 declaration, uint32 parent) noexcept
    {
        const uint32 symbol = symbolOf(declaration);
        if (symbol == NO_SYMBOL)
        {
            return NO_IR;
        }
        const uint32 node = addChild(parent, kind, declaration);
        if (node != NO_IR)
        {
            at(node).Symbol = symbol;
            at(node).Type = getSymbol(symbol).Type;
        }
        return node;
    }

    void lowerStruct(uint32 declaration, uint32 parent) noexcept
    {
        const uint32 symbol = symbolOf(declaration);
        if (symbol == NO_SYMBOL || getSymbol(symbol).IsExtern == true)
        {
            return;
        }
        lowerMembers(declaration, addAggregate(IrKind::Struct, declaration, parent));
    }

    /// Lower the fields and constants, then the methods, of a struct, interface or extension.
    void lowerMembers(uint32 declaration, uint32 node) noexcept
    {
        if (node == NO_IR)
        {
            return;
        }
        for (uint32 member = skipAttributes(firstChild(declaration)); member != NO_NODE && mIsFatal == false;
             member = nextSibling(member))
        {
            if (getKind(member) != SyntaxKind::VariableDeclaration || symbolOf(member) == NO_SYMBOL)
            {
                continue;
            }
            const bool isConstant = getSymbol(symbolOf(member)).Kind == SymbolKind::Variable;
            const uint32 child = addChild(node, isConstant == true ? IrKind::Constant : IrKind::Field, member);
            if (child == NO_IR)
            {
                return;
            }
            at(child).Symbol = symbolOf(member);
            at(child).Type = typeOf(member);
            const uint32 initializer = findInitializer(member);
            if (isConstant == true && initializer != NO_NODE)
            {
                appendChild(child, lowerValue(initializer, at(child).Type));
            }
        }
        for (uint32 member = skipAttributes(firstChild(declaration)); member != NO_NODE && mIsFatal == false;
             member = nextSibling(member))
        {
            if (getKind(member) == SyntaxKind::FunctionDeclaration)
            {
                lowerFunction(member, node);
            }
        }
    }

    /// Resolve `extern slang "path"` to a request file with the search `#include` uses: beside the
    /// declaring file, from the snapshot root, then below each include directory.
    void lowerExternBlock(uint32 declaration) noexcept
    {
        const uint32 token = getSyntax(declaration).Token;
        // `extern slang { ... }` names no file: the including Slang code provides its definitions.
        if (token == NO_TOKEN || mInputs.Preprocessed.Tokens[token].Kind != TokenKind::String)
        {
            return;
        }
        const std::string_view quoted = spellNode(declaration);
        if (quoted.size() < 2)
        {
            return;
        }
        const std::string_view path = quoted.substr(1, quoted.size() - 2);
        const SourceLocation location = locate(declaration);
        const std::string_view declaringPath = location.FileIndex < mInputs.Request.Files.GetSize()
                                                   ? ViewText(mInputs.Request.Files[location.FileIndex].Path)
                                                   : std::string_view{};
        uint16 found = NO_SOURCE;
        const IncludePathOutcome outcome = FindSnapshotFile(mInputs.Request, declaringPath, path, mPathScratch, found);
        if (outcome == IncludePathOutcome::OutOfMemory)
        {
            failOutOfMemory();
            return;
        }
        if (outcome != IncludePathOutcome::Ok)
        {
            mSink.ReportError(DiagnosticCode::ExternSlangNotFound,
                              location,
                              SourceLocation{},
                              ComposeMessage("'",
                                             path,
                                             "' is not in the snapshot: not beside this file, at its root or in an "
                                             "include directory"));
            return;
        }
        for (const uint16 existing : mModule.Includes)
        {
            if (existing == found)
            {
                return;
            }
        }
        if (mModule.Includes.TryAdd(found) == false)
        {
            failOutOfMemory();
        }
    }

    [[nodiscard]] uint32 findBody(uint32 declaration) const noexcept
    {
        for (uint32 child = firstChild(declaration); child != NO_NODE; child = nextSibling(child))
        {
            if (getKind(child) == SyntaxKind::Block)
            {
                return child;
            }
        }
        return NO_NODE;
    }

    [[nodiscard]] uint16
    readFunctionAttributes(uint32 declaration,
                           foundation::StaticArray<int64, THREAD_GROUP_DIMENSIONS>& outThreads) const noexcept
    {
        uint16 flags = 0;
        for (uint32 attribute = firstChild(declaration);
             attribute != NO_NODE && getKind(attribute) == SyntaxKind::Attribute;
             attribute = nextSibling(attribute))
        {
            const std::string_view name = spellNode(attribute);
            if (name == "stage")
            {
                flags |= IR_ENTRY;
            }
            else if (name == "threads")
            {
                usize dimension = 0;
                for (uint32 argument = firstChild(attribute);
                     argument != NO_NODE && dimension < THREAD_GROUP_DIMENSIONS;
                     argument = nextSibling(argument))
                {
                    outThreads[dimension] = getModel().NodeIntegerConstants[argument].Value;
                    ++dimension;
                }
            }
            else if (name == "noinline")
            {
                flags |= IR_NOINLINE;
            }
            else if (name == "forceinline")
            {
                flags |= IR_FORCEINLINE;
            }
            else if (name == "mutating")
            {
                flags |= IR_MUTATING;
            }
        }
        return flags;
    }

    [[nodiscard]] static uint32 clampThreads(int64 count) noexcept
    {
        if (count < 1)
        {
            return 1;
        }
        return static_cast<uint32>(count < MAX_THREAD_GROUP_DIMENSION ? count : MAX_THREAD_GROUP_DIMENSION);
    }

    [[nodiscard]] static IrSemantic getSemantic(std::string_view name) noexcept
    {
        if (name == "DispatchThreadId")
        {
            return IrSemantic::DispatchThreadId;
        }
        if (name == "GroupThreadId")
        {
            return IrSemantic::GroupThreadId;
        }
        if (name == "GroupId")
        {
            return IrSemantic::GroupId;
        }
        if (name == "GroupIndex")
        {
            return IrSemantic::GroupIndex;
        }
        return IrSemantic::None;
    }

    void lowerFunction(uint32 declaration, uint32 parent) noexcept
    {
        const uint32 body = findBody(declaration);
        const uint32 symbol = symbolOf(declaration);
        if (symbol == NO_SYMBOL || getSymbol(symbol).IsExtern == true)
        {
            return;
        }
        // Interface requirements are declared without a body; any other function needs one here.
        const uint32 owner = getSymbol(symbol).Parent;
        const bool isRequirement = owner != NO_SYMBOL && getSymbol(owner).Kind == SymbolKind::Interface;
        if (body == NO_NODE && isRequirement == false)
        {
            return;
        }
        const uint32 node = addChild(parent, IrKind::Function, declaration);
        if (node == NO_IR)
        {
            return;
        }
        foundation::StaticArray<int64, THREAD_GROUP_DIMENSIONS> threads{};
        at(node).Symbol = symbol;
        at(node).Type = getSymbol(symbol).Type;
        at(node).Flags = readFunctionAttributes(declaration, threads);
        uint32 groupIndexParameter = NO_SYMBOL;
        for (uint32 child = skipAttributes(firstChild(declaration)); child != NO_NODE && mIsFatal == false;
             child = nextSibling(child))
        {
            if (getKind(child) != SyntaxKind::Parameter)
            {
                continue;
            }
            const uint32 parameter = addChild(node, IrKind::Parameter, child);
            if (parameter == NO_IR)
            {
                return;
            }
            at(parameter).Symbol = symbolOf(child);
            at(parameter).Type = typeOf(child);
            at(parameter).Direction = getSyntax(child).Direction;
            IrSemantic semantic = IrSemantic::None;
            for (uint32 part = firstChild(child); part != NO_NODE; part = nextSibling(part))
            {
                if (getKind(part) == SyntaxKind::Semantic)
                {
                    semantic = getSemantic(spellNode(part));
                }
            }
            at(parameter).Index = static_cast<uint32>(semantic);
            if (semantic == IrSemantic::GroupIndex)
            {
                groupIndexParameter = symbolOf(child);
            }
        }
        if (body == NO_NODE)
        {
            return;
        }
        const uint32 block = addChild(node, IrKind::Block, body);
        if (block == NO_IR)
        {
            return;
        }
        mReturnType = getSymbol(symbol).Type;
        if ((at(node).Flags & IR_ENTRY) != 0 && getSymbol(symbol).Stage == EntryStage::Compute)
        {
            IrThreadGroup group;
            group.X = clampThreads(threads[0]);
            group.Y = clampThreads(threads[1]);
            group.Z = clampThreads(threads[2]);
            if (mModule.ThreadGroups.TryAdd(group) == false)
            {
                failOutOfMemory();
                return;
            }
            at(node).Value = static_cast<int64>(mModule.ThreadGroups.GetSize() - 1);
            const foundation::uint64 total = foundation::uint64{group.X} * group.Y;
            // Compare in uint64: MSVC rejects the signed/unsigned comparison (C4018).
            constexpr foundation::uint64 MAX_THREAD_COUNT = static_cast<foundation::uint64>(MAX_THREAD_GROUP_DIMENSION);
            const foundation::uint64 threadCount =
                total > MAX_THREAD_COUNT / group.Z ? MAX_THREAD_COUNT : total * group.Z;
            lowerGroupsharedClears(symbol, node, block, groupIndexParameter, static_cast<uint32>(threadCount));
        }
        for (uint32 statement = firstChild(body); statement != NO_NODE && mIsFatal == false;
             statement = nextSibling(statement))
        {
            lowerStatement(statement, block);
        }
    }

    // ----- groupshared clears --------------------------------------------------------------------

    /// Clear, at the start of entry point `entry`, every `groupshared` variable declared `= 0` that
    /// the entry point reaches through calls.
    void lowerGroupsharedClears(uint32 entry,
                                uint32 function,
                                uint32 block,
                                uint32 groupIndexParameter,
                                uint32 threadCount) noexcept
    {
        if (mClearedGroupshared.IsEmpty() == true)
        {
            return;
        }
        Array<uint8> isVisited;
        Array<uint8> isReferenced;
        Array<uint32> pending;
        Array<uint32> stack;
        const usize symbolCount = getModel().Symbols.GetSize();
        if (isVisited.TryResize(symbolCount, 0) == false || isReferenced.TryResize(symbolCount, 0) == false ||
            pending.TryAdd(entry) == false)
        {
            failOutOfMemory();
            return;
        }
        isVisited[entry] = 1;
        while (pending.IsEmpty() == false)
        {
            const uint32 current = pending.GetLast();
            pending.RemoveLast();
            const uint32 body = findBody(getSymbol(current).Declaration);
            if (body == NO_NODE)
            {
                continue;
            }
            if (stack.TryAdd(body) == false)
            {
                failOutOfMemory();
                return;
            }
            while (stack.IsEmpty() == false)
            {
                const uint32 node = stack.GetLast();
                stack.RemoveLast();
                const uint32 symbol = symbolOf(node);
                const CallKind call = getModel().NodeCallKinds[node];
                if (getKind(node) == SyntaxKind::Name && symbol != NO_SYMBOL)
                {
                    isReferenced[symbol] = 1;
                }
                if ((call == CallKind::Function || call == CallKind::Method) && symbol != NO_SYMBOL &&
                    isVisited[symbol] == 0 && getSymbol(symbol).IsExtern == false)
                {
                    isVisited[symbol] = 1;
                    if (pending.TryAdd(symbol) == false)
                    {
                        failOutOfMemory();
                        return;
                    }
                }
                for (uint32 child = firstChild(node); child != NO_NODE; child = nextSibling(child))
                {
                    if (stack.TryAdd(child) == false)
                    {
                        failOutOfMemory();
                        return;
                    }
                }
            }
        }
        bool hasClear = false;
        for (const uint32 variable : mClearedGroupshared)
        {
            if (isReferenced[variable] == 0)
            {
                continue;
            }
            const uint32 clear = addChild(block, IrKind::ClearGroupshared, getSymbol(variable).Declaration);
            if (clear == NO_IR)
            {
                return;
            }
            at(clear).Symbol = variable;
            at(clear).Type = getSymbol(variable).Type;
            at(clear).Index = threadCount;
            uint32 index = NO_IR;
            if (groupIndexParameter != NO_SYMBOL)
            {
                index = addChild(clear, IrKind::Variable, NO_NODE);
                if (index != NO_IR)
                {
                    at(index).Symbol = groupIndexParameter;
                    at(index).Type = getScalarType(ScalarKind::Uint32);
                }
            }
            else
            {
                index = addChild(clear, IrKind::HiddenGroupIndex, NO_NODE);
                at(function).Flags |= IR_HIDDEN_GROUP_INDEX;
            }
            if (index == NO_IR || requireZero(getElementOf(getSymbol(variable).Type), NO_NODE) == false)
            {
                return;
            }
            hasClear = true;
        }
        if (hasClear == true)
        {
            (void)addChild(block, IrKind::GroupBarrier, NO_NODE);
        }
    }

    /// The innermost element type of an array, or `type` itself.
    [[nodiscard]] uint32 getElementOf(uint32 type) const noexcept
    {
        while (type != NO_TYPE && getType(type).Kind == TypeKind::Array)
        {
            type = getType(type).Element;
        }
        return type;
    }

    // ----- Statements ----------------------------------------------------------------------------

    [[nodiscard]] uint16 readStatementAttributes(uint32 statement, int64& outUnrollCount) const noexcept
    {
        uint16 flags = 0;
        outUnrollCount = 0;
        for (uint32 attribute = firstChild(statement);
             attribute != NO_NODE && getKind(attribute) == SyntaxKind::Attribute;
             attribute = nextSibling(attribute))
        {
            const std::string_view name = spellNode(attribute);
            if (name == "unroll")
            {
                flags |= IR_UNROLL;
                const uint32 count = firstChild(attribute);
                if (count != NO_NODE)
                {
                    outUnrollCount = getModel().NodeIntegerConstants[count].Value;
                }
            }
            else if (name == "loop")
            {
                flags |= IR_LOOP;
            }
            else if (name == "branch")
            {
                flags |= IR_BRANCH;
            }
            else if (name == "flatten")
            {
                flags |= IR_FLATTEN;
            }
        }
        return flags;
    }

    /// Lower an optional part of a `for`, or an Empty node.
    void lowerForPart(uint32 part, uint32 parent, bool isStatement) noexcept
    {
        if (part == NO_NODE || getKind(part) == SyntaxKind::Empty)
        {
            (void)addChild(parent, IrKind::Empty, part);
            return;
        }
        if (isStatement == true)
        {
            lowerStatement(part, parent);
            return;
        }
        appendChild(parent, lowerValue(part, getScalarType(ScalarKind::Bool)));
    }

    void lowerStatement(uint32 statement, uint32 parent) noexcept
    {
        switch (getKind(statement))
        {
            case SyntaxKind::Block: {
                const uint32 block = addChild(parent, IrKind::Block, statement);
                for (uint32 child = firstChild(statement); child != NO_NODE && mIsFatal == false;
                     child = nextSibling(child))
                {
                    lowerStatement(child, block);
                }
                return;
            }
            case SyntaxKind::VariableDeclaration:
                lowerLocal(statement, parent);
                return;
            case SyntaxKind::ExpressionStatement: {
                const uint32 evaluate = addChild(parent, IrKind::Evaluate, statement);
                appendChild(evaluate, lowerValue(firstChild(statement), NO_TYPE));
                return;
            }
            case SyntaxKind::AssignmentStatement:
                lowerAssignment(statement, parent);
                return;
            case SyntaxKind::IncrementStatement:
                lowerIncrement(statement, parent);
                return;
            case SyntaxKind::IfStatement: {
                if (getSyntax(statement).IsConstexpr == true)
                {
                    // Only the branch the condition selected was checked; it is the whole statement.
                    const uint32 condition = skipAttributes(firstChild(statement));
                    const uint32 whenTrue = nextSibling(condition);
                    const uint32 whenFalse = whenTrue == NO_NODE ? NO_NODE : nextSibling(whenTrue);
                    const uint32 taken = getModel().NodeIntegerConstants[statement].Value != 0 ? whenTrue : whenFalse;
                    if (taken != NO_NODE)
                    {
                        const uint32 block = addChild(parent, IrKind::Block, taken);
                        lowerStatement(taken, block);
                    }
                    return;
                }
                int64 unroll = 0;
                const uint32 node = addChild(parent, IrKind::If, statement);
                if (node == NO_IR)
                {
                    return;
                }
                at(node).Flags = readStatementAttributes(statement, unroll);
                const uint32 condition = skipAttributes(firstChild(statement));
                appendChild(node, lowerValue(condition, getScalarType(ScalarKind::Bool)));
                const uint32 whenTrue = nextSibling(condition);
                lowerStatement(whenTrue, node);
                const uint32 whenFalse = nextSibling(whenTrue);
                if (whenFalse != NO_NODE)
                {
                    lowerStatement(whenFalse, node);
                }
                return;
            }
            case SyntaxKind::ForStatement: {
                int64 unroll = 0;
                const uint32 node = addChild(parent, IrKind::For, statement);
                if (node == NO_IR)
                {
                    return;
                }
                at(node).Flags = readStatementAttributes(statement, unroll);
                at(node).Value = unroll;
                const uint32 initializer = skipAttributes(firstChild(statement));
                const uint32 condition = nextSibling(initializer);
                const uint32 increment = nextSibling(condition);
                const uint32 body = nextSibling(increment);
                lowerForPart(initializer, node, true);
                lowerForPart(condition, node, false);
                lowerForPart(increment, node, true);
                lowerStatement(body, node);
                return;
            }
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement: {
                const bool isWhile = getKind(statement) == SyntaxKind::WhileStatement;
                int64 unroll = 0;
                const uint32 node = addChild(parent, isWhile == true ? IrKind::While : IrKind::Do, statement);
                if (node == NO_IR)
                {
                    return;
                }
                at(node).Flags = readStatementAttributes(statement, unroll);
                at(node).Value = unroll;
                const uint32 first = skipAttributes(firstChild(statement));
                const uint32 second = nextSibling(first);
                if (isWhile == true)
                {
                    appendChild(node, lowerValue(first, getScalarType(ScalarKind::Bool)));
                    lowerStatement(second, node);
                }
                else
                {
                    lowerStatement(first, node);
                    appendChild(node, lowerValue(second, getScalarType(ScalarKind::Bool)));
                }
                return;
            }
            case SyntaxKind::SwitchStatement:
                lowerSwitch(statement, parent);
                return;
            case SyntaxKind::BreakStatement:
                (void)addChild(parent, IrKind::Break, statement);
                return;
            case SyntaxKind::ContinueStatement:
                (void)addChild(parent, IrKind::Continue, statement);
                return;
            case SyntaxKind::ReturnStatement: {
                const uint32 node = addChild(parent, IrKind::Return, statement);
                const uint32 value = firstChild(statement);
                if (value != NO_NODE)
                {
                    appendChild(node, lowerValue(value, mReturnType));
                }
                return;
            }
            case SyntaxKind::DiscardStatement:
                (void)addChild(parent, IrKind::Discard, statement);
                return;
            default:
                // static_assert is checked by the front end; [[fallthrough]] is resolved by lowerSwitch.
                return;
        }
    }

    void lowerLocal(uint32 statement, uint32 parent) noexcept
    {
        const uint32 node = addChild(parent, IrKind::Declare, statement);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Symbol = symbolOf(statement);
        at(node).Type = typeOf(statement);
        const uint32 initializer = findInitializer(statement);
        if (initializer != NO_NODE && getKind(initializer) != SyntaxKind::VoidInitializer)
        {
            appendChild(node, lowerValue(initializer, at(node).Type));
        }
    }

    [[nodiscard]] static Punctuator getCompoundOperator(Punctuator assignment) noexcept
    {
        switch (assignment)
        {
            case Punctuator::PlusEqual:
                return Punctuator::Plus;
            case Punctuator::MinusEqual:
                return Punctuator::Minus;
            case Punctuator::StarEqual:
                return Punctuator::Star;
            case Punctuator::SlashEqual:
                return Punctuator::Slash;
            case Punctuator::PercentEqual:
                return Punctuator::Percent;
            case Punctuator::AmpersandEqual:
                return Punctuator::Ampersand;
            case Punctuator::PipeEqual:
                return Punctuator::Pipe;
            case Punctuator::CaretEqual:
                return Punctuator::Caret;
            case Punctuator::LessLessEqual:
                return Punctuator::LessLess;
            case Punctuator::GreaterGreaterEqual:
                return Punctuator::GreaterGreater;
            default:
                return Punctuator::None;
        }
    }

    [[nodiscard]] static bool isWrappingOperator(Punctuator operation) noexcept
    {
        return operation == Punctuator::Plus || operation == Punctuator::Minus || operation == Punctuator::Star;
    }

    [[nodiscard]] Punctuator getTokenSymbol(uint32 node) const noexcept
    {
        const uint32 token = getSyntax(node).Token;
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return Punctuator::None;
        }
        return mInputs.Preprocessed.Tokens[token].Symbol;
    }

    [[nodiscard]] bool isConstant(uint32 node) const noexcept
    {
        return getModel().NodeIntegerConstants[node].IsKnown == true;
    }

    void lowerAssignment(uint32 statement, uint32 parent) noexcept
    {
        const uint32 target = firstChild(statement);
        const uint32 value = nextSibling(target);
        const Punctuator assignment = getTokenSymbol(statement);
        const Punctuator operation = getCompoundOperator(assignment);
        const uint32 targetType = typeOf(target);
        if (isWrappingOperator(operation) == true && isSignedIntegerValue(targetType) == true)
        {
            IrHelper helper;
            helper.Kind = IrHelperKind::WrapAssign;
            helper.Operator = operation;
            helper.Type = targetType;
            helper.SecondType = resolveType(value, targetType);
            const uint32 index = findOrAddHelper(helper, statement);
            const uint32 node = addChild(parent, IrKind::WrappedUpdate, statement);
            if (node == NO_IR || index == NO_INDEX)
            {
                return;
            }
            at(node).Index = index;
            appendChild(node, lowerValue(target, NO_TYPE));
            appendChild(node, lowerValue(value, targetType));
            return;
        }
        const uint32 node = addChild(parent, IrKind::Assign, statement);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Operator = assignment;
        if ((operation == Punctuator::LessLess || operation == Punctuator::GreaterGreater) &&
            isConstant(value) == false)
        {
            at(node).Flags |= IR_MASK_SHIFT;
        }
        appendChild(node, lowerValue(target, NO_TYPE));
        appendChild(node, lowerValue(value, targetType));
    }

    void lowerIncrement(uint32 statement, uint32 parent) noexcept
    {
        const uint32 target = firstChild(statement);
        const Punctuator operation = getTokenSymbol(statement);
        const uint32 targetType = typeOf(target);
        if (isSignedIntegerValue(targetType) == true)
        {
            IrHelper helper;
            helper.Kind = IrHelperKind::WrapIncrement;
            helper.Operator = operation == Punctuator::PlusPlus ? Punctuator::Plus : Punctuator::Minus;
            helper.Type = targetType;
            const uint32 index = findOrAddHelper(helper, statement);
            const uint32 node = addChild(parent, IrKind::WrappedUpdate, statement);
            if (node == NO_IR || index == NO_INDEX)
            {
                return;
            }
            at(node).Index = index;
            appendChild(node, lowerValue(target, NO_TYPE));
            return;
        }
        const uint32 node = addChild(parent, IrKind::Increment, statement);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Operator = operation;
        appendChild(node, lowerValue(target, NO_TYPE));
    }

    /// Lower a switch. A case that ends in `[[fallthrough]]` receives a copy of the next case's
    /// statements, each case's statements in their own block, so no target needs fallthrough.
    void lowerSwitch(uint32 statement, uint32 parent) noexcept
    {
        int64 unroll = 0;
        const uint32 node = addChild(parent, IrKind::Switch, statement);
        if (node == NO_IR)
        {
            return;
        }
        at(node).Flags = readStatementAttributes(statement, unroll);
        const uint32 selector = skipAttributes(firstChild(statement));
        const uint32 selectorType = typeOf(selector);
        appendChild(node, lowerValue(selector, NO_TYPE));
        for (uint32 clause = nextSibling(selector); clause != NO_NODE && mIsFatal == false;
             clause = nextSibling(clause))
        {
            const uint32 caseNode = addChild(node, IrKind::Case, clause);
            if (caseNode == NO_IR)
            {
                return;
            }
            uint32 statements = firstChild(clause);
            if (spellNode(clause) == "case")
            {
                appendChild(caseNode, lowerValue(statements, selectorType));
                statements = nextSibling(statements);
            }
            else
            {
                at(caseNode).Flags |= IR_DEFAULT;
            }
            uint32 current = clause;
            while (current != NO_NODE && mIsFatal == false)
            {
                const uint32 block = addChild(caseNode, IrKind::Block, current);
                bool isFallingThrough = false;
                for (; statements != NO_NODE && mIsFatal == false; statements = nextSibling(statements))
                {
                    if (getKind(statements) == SyntaxKind::FallthroughStatement)
                    {
                        isFallingThrough = true;
                        continue;
                    }
                    lowerStatement(statements, block);
                }
                current = isFallingThrough == true ? nextSibling(current) : NO_NODE;
                statements = firstChild(current);
                if (current != NO_NODE && spellNode(current) == "case")
                {
                    statements = nextSibling(statements);
                }
            }
        }
    }

    // ----- Expressions ---------------------------------------------------------------------------

    [[nodiscard]] bool isZeroLiteral(uint32 node) const noexcept
    {
        return getKind(node) == SyntaxKind::NumberLiteral && spellNode(node) == "0";
    }

    /// Lower an expression whose value meets `contextType` (or NO_TYPE); returns NO_IR on failure.
    [[nodiscard]] uint32 lowerValue(uint32 node, uint32 contextType) noexcept
    {
        if (node == NO_NODE || mIsFatal == true)
        {
            return NO_IR;
        }
        const SyntaxKind kind = getKind(node);
        const uint32 type = resolveType(node, contextType);
        if (kind == SyntaxKind::ParenthesizedExpression)
        {
            return lowerValue(firstChild(node), contextType);
        }
        const SemanticType& shape = getType(type);
        if (isZeroLiteral(node) == true && shape.Kind != TypeKind::Scalar)
        {
            const uint32 zero = addNode(IrKind::Zero, node);
            if (zero != NO_IR && requireZero(type, node) == true)
            {
                at(zero).Type = type;
            }
            return zero;
        }
        // Constant integer and bool expressions are emitted as their exact folded value.
        const IntegerConstant constant = getModel().NodeIntegerConstants[node];
        if (constant.IsKnown == true && shape.Kind == TypeKind::Scalar && kind != SyntaxKind::Name &&
            kind != SyntaxKind::ScopeExpression)
        {
            const uint32 literal = addNode(IrKind::Literal, node);
            if (literal != NO_IR)
            {
                at(literal).Type = type;
                at(literal).Value = constant.Value;
            }
            return literal;
        }
        switch (kind)
        {
            case SyntaxKind::NumberLiteral: {
                const uint32 literal = addNode(IrKind::FloatLiteral, node);
                if (literal != NO_IR)
                {
                    at(literal).Type = type;
                    at(literal).Token = getSyntax(node).Token;
                }
                return literal;
            }
            case SyntaxKind::Name:
                return lowerName(node, type);
            case SyntaxKind::ScopeExpression: {
                const uint32 symbol = symbolOf(node);
                const bool isEnumerator = symbol != NO_SYMBOL && getSymbol(symbol).Kind == SymbolKind::Enumerator;
                const uint32 reference =
                    addNode(isEnumerator == true ? IrKind::EnumeratorReference : IrKind::TypeConstant, node);
                if (reference != NO_IR)
                {
                    at(reference).Symbol = symbol;
                    at(reference).Type = type;
                    at(reference).Index = symbolOf(firstChild(node));
                }
                return reference;
            }
            case SyntaxKind::UnaryExpression:
                return lowerUnary(node, type);
            case SyntaxKind::BinaryExpression:
                return lowerBinary(node, type);
            case SyntaxKind::ConditionalExpression: {
                const uint32 conditional = addNode(IrKind::Conditional, node);
                if (conditional == NO_IR)
                {
                    return NO_IR;
                }
                at(conditional).Type = type;
                const uint32 condition = firstChild(node);
                const uint32 whenTrue = nextSibling(condition);
                appendChild(conditional, lowerValue(condition, getScalarType(ScalarKind::Bool)));
                appendChild(conditional, lowerValue(whenTrue, type));
                appendChild(conditional, lowerValue(nextSibling(whenTrue), type));
                return conditional;
            }
            case SyntaxKind::CallExpression:
                return lowerCall(node, type);
            case SyntaxKind::MemberExpression: {
                const uint32 field = symbolOf(node);
                const bool isField = field != NO_SYMBOL && getSymbol(field).Kind == SymbolKind::Field;
                const uint32 member = addNode(isField == true ? IrKind::Member : IrKind::Swizzle, node);
                if (member == NO_IR)
                {
                    return NO_IR;
                }
                at(member).Type = type;
                at(member).Symbol = isField == true ? field : NO_SYMBOL;
                at(member).Token = getSyntax(node).Token;
                appendChild(member, lowerValue(firstChild(node), NO_TYPE));
                return member;
            }
            case SyntaxKind::IndexExpression:
                return lowerIndex(node, type);
            case SyntaxKind::InitializerList:
                return lowerInitializerList(node, type);
            default:
                return NO_IR;
        }
    }

    [[nodiscard]] uint32 lowerName(uint32 node, uint32 type) noexcept
    {
        const uint32 symbol = symbolOf(node);
        const bool isField = symbol != NO_SYMBOL && getSymbol(symbol).Kind == SymbolKind::Field;
        const uint32 name = addNode(isField == true ? IrKind::ThisField : IrKind::Variable, node);
        if (name != NO_IR)
        {
            at(name).Symbol = symbol;
            at(name).Type = type;
        }
        return name;
    }

    [[nodiscard]] uint32 lowerUnary(uint32 node, uint32 type) noexcept
    {
        const Punctuator operation = getTokenSymbol(node);
        const uint32 operand = firstChild(node);
        if (operation == Punctuator::Plus)
        {
            return lowerValue(operand, type);
        }
        if (operation == Punctuator::Minus && isSignedIntegerValue(type) == true)
        {
            IrHelper helper;
            helper.Kind = IrHelperKind::WrapNegate;
            helper.Type = type;
            const uint32 index = findOrAddHelper(helper, node);
            const uint32 wrapped = addNode(IrKind::WrappedOperation, node);
            if (wrapped == NO_IR || index == NO_INDEX)
            {
                return NO_IR;
            }
            at(wrapped).Type = type;
            at(wrapped).Index = index;
            appendChild(wrapped, lowerValue(operand, type));
            return wrapped;
        }
        const uint32 unary = addNode(IrKind::Unary, node);
        if (unary != NO_IR)
        {
            at(unary).Type = type;
            at(unary).Operator = operation;
            appendChild(unary, lowerValue(operand, type));
        }
        return unary;
    }

    [[nodiscard]] static bool isComparison(Punctuator operation) noexcept
    {
        return operation == Punctuator::EqualEqual || operation == Punctuator::ExclamationEqual ||
               operation == Punctuator::Less || operation == Punctuator::LessEqual ||
               operation == Punctuator::Greater || operation == Punctuator::GreaterEqual;
    }

    [[nodiscard]] uint32 lowerBinary(uint32 node, uint32 type) noexcept
    {
        const Punctuator operation = getTokenSymbol(node);
        const uint32 left = firstChild(node);
        const uint32 right = nextSibling(left);
        // Operands of a comparison or `&&`/`||` meet each other, not the bool result.
        const bool isBoolResult = isComparison(operation) == true || operation == Punctuator::AmpersandAmpersand ||
                                  operation == Punctuator::PipePipe;
        const uint32 leftType = resolveType(left, isBoolResult == true ? typeOf(right) : type);
        const uint32 rightType = resolveType(right, isBoolResult == true ? leftType : type);
        const TypeKind leftKind = getType(leftType).Kind;
        const TypeKind rightKind = getType(rightType).Kind;
        IrKind irKind = IrKind::Binary;
        uint32 helperIndex = NO_INDEX;
        if (operation == Punctuator::Star && leftKind == TypeKind::Matrix &&
            (rightKind == TypeKind::Matrix || rightKind == TypeKind::Vector))
        {
            irKind = IrKind::MatrixProduct;
        }
        else if (isWrappingOperator(operation) == true && isSignedIntegerValue(type) == true)
        {
            IrHelper helper;
            helper.Kind = IrHelperKind::WrapBinary;
            helper.Operator = operation;
            helper.Type = leftType;
            helper.SecondType = rightType;
            helperIndex = findOrAddHelper(helper, node);
            if (helperIndex == NO_INDEX)
            {
                return NO_IR;
            }
            irKind = IrKind::WrappedOperation;
        }
        const uint32 binary = addNode(irKind, node);
        if (binary == NO_IR)
        {
            return NO_IR;
        }
        at(binary).Type = type;
        at(binary).Operator = operation;
        at(binary).Index = helperIndex;
        if ((operation == Punctuator::LessLess || operation == Punctuator::GreaterGreater) &&
            isConstant(right) == false)
        {
            at(binary).Flags |= IR_MASK_SHIFT;
        }
        appendChild(binary, lowerValue(left, leftType));
        appendChild(binary, lowerValue(right, rightType));
        return binary;
    }

    [[nodiscard]] uint32 lowerIndex(uint32 node, uint32 type) noexcept
    {
        const uint32 object = firstChild(node);
        const uint32 index = nextSibling(object);
        const uint32 access = addNode(IrKind::IndexAccess, node);
        if (access == NO_IR)
        {
            return NO_IR;
        }
        at(access).Type = type;
        const uint32 objectType = getModel().NodeTypes[object];
        if (objectType != NO_TYPE && getType(objectType).Kind == TypeKind::Resource)
        {
            // Resource elements are not bounds-checked; out-of-range buffer reads follow the target's
            // robustness rules.
            appendChild(access, lowerValue(object, NO_TYPE));
            const bool isBuffer = getType(objectType).Resource != ResourceKind::RWTexture2D;
            appendChild(access, lowerValue(index, isBuffer == true ? getScalarType(ScalarKind::Uint32) : NO_TYPE));
            return access;
        }
        if (mOptions.ShouldCheckBounds == true && isConstant(index) == false)
        {
            const SemanticType& indexShape = getType(resolveType(index, getScalarType(ScalarKind::Uint32)));
            const bool isWide = indexShape.Scalar == ScalarKind::Int64 || indexShape.Scalar == ScalarKind::Uint64;
            IrHelper helper;
            helper.Kind = IrHelperKind::CheckIndex;
            helper.Type = getScalarType(isWide == true ? ScalarKind::Uint64 : ScalarKind::Uint32);
            const uint32 helperIndex = findOrAddHelper(helper, node);
            if (helperIndex == NO_INDEX ||
                TryReserveGrowing(mModule.BoundsChecks, mModule.BoundsChecks.GetSize() + 1) == false)
            {
                if (helperIndex != NO_INDEX)
                {
                    failOutOfMemory();
                }
                return NO_IR;
            }
            mModule.BoundsChecks.Add(locate(index));
            at(access).Index = static_cast<uint32>(mModule.BoundsChecks.GetSize());
            at(access).Value = helperIndex;
        }
        appendChild(access, lowerValue(object, NO_TYPE));
        appendChild(access, lowerValue(index, getScalarType(ScalarKind::Uint32)));
        return access;
    }

    [[nodiscard]] uint32 lowerInitializerList(uint32 node, uint32 type) noexcept
    {
        Array<uint32> fields;
        for (uint32 field = firstChild(node); field != NO_NODE; field = nextSibling(field))
        {
            if (fields.TryAdd(symbolOf(field)) == false)
            {
                failOutOfMemory();
                return NO_IR;
            }
        }
        if (requireDefaults(type, node) == false)
        {
            return NO_IR;
        }
        IrHelper helper;
        helper.Kind = IrHelperKind::Initializer;
        helper.Type = type;
        const uint32 helperIndex =
            findOrAddHelper(helper, foundation::BorrowedView<const uint32>(fields.GetData(), fields.GetSize()), node);
        const uint32 initialize = addNode(IrKind::Initialize, node);
        if (initialize == NO_IR || helperIndex == NO_INDEX)
        {
            return NO_IR;
        }
        at(initialize).Type = type;
        at(initialize).Index = helperIndex;
        for (uint32 field = firstChild(node); field != NO_NODE && mIsFatal == false; field = nextSibling(field))
        {
            appendChild(initialize, lowerValue(firstChild(field), getSymbol(symbolOf(field)).Type));
        }
        if (mModule.Helpers[helperIndex].Body == NO_IR)
        {
            lowerInitializerBody(helperIndex, type, node);
        }
        return initialize;
    }

    /// Make sure every zero value an initializer of `type` may need exists.
    [[nodiscard]] bool requireDefaults(uint32 type, uint32 origin) noexcept
    {
        const Symbol& declared = getSymbol(getType(type).Symbol);
        for (uint32 i = 0; i < declared.MemberCount; ++i)
        {
            const Symbol& member = getSymbol(getModel().Members[declared.FirstMember + i]);
            if (member.Kind == SymbolKind::Field && member.HasDefault == false &&
                requireZero(member.Type, origin) == false)
            {
                return false;
            }
        }
        return true;
    }

    /// The body of an initializer helper: one value per struct field in declaration order, each a
    /// helper parameter, the field's default or its zero.
    void lowerInitializerBody(uint32 helperIndex, uint32 type, uint32 origin) noexcept
    {
        const uint32 body = addNode(IrKind::Block, origin);
        if (body == NO_IR)
        {
            return;
        }
        mModule.Helpers[helperIndex].Body = body;
        const IrHelper helper = mModule.Helpers[helperIndex];
        const Symbol& declared = getSymbol(getType(type).Symbol);
        for (uint32 i = 0; i < declared.MemberCount && mIsFatal == false; ++i)
        {
            const uint32 field = getModel().Members[declared.FirstMember + i];
            const Symbol& member = getSymbol(field);
            if (member.Kind != SymbolKind::Field)
            {
                continue;
            }
            uint32 written = NO_INDEX;
            for (uint32 k = 0; k < helper.FieldCount; ++k)
            {
                if (mModule.HelperFields[helper.FirstField + k] == field)
                {
                    written = k;
                }
            }
            uint32 value = NO_IR;
            if (written != NO_INDEX)
            {
                value = addNode(IrKind::HelperParameter, NO_NODE);
                if (value != NO_IR)
                {
                    at(value).Index = written;
                    at(value).Type = member.Type;
                }
            }
            else if (member.HasDefault == true)
            {
                value = lowerValue(findInitializer(member.Declaration), member.Type);
            }
            else
            {
                value = addNode(IrKind::Zero, NO_NODE);
                if (value != NO_IR)
                {
                    at(value).Type = member.Type;
                }
            }
            appendChild(body, value);
        }
    }

    // ----- Calls ---------------------------------------------------------------------------------

    /// True when the subtree at `node` calls a LUSL or extern function, which may have effects.
    [[nodiscard]] bool containsCall(uint32 node) noexcept
    {
        mStack.Clear();
        if (mStack.TryAdd(node) == false)
        {
            failOutOfMemory();
            return false;
        }
        while (mStack.IsEmpty() == false)
        {
            const uint32 current = mStack.GetLast();
            mStack.RemoveLast();
            const CallKind call = getModel().NodeCallKinds[current];
            if (call == CallKind::Function || call == CallKind::Method)
            {
                return true;
            }
            for (uint32 child = firstChild(current); child != NO_NODE; child = nextSibling(child))
            {
                if (mStack.TryAdd(child) == false)
                {
                    failOutOfMemory();
                    return false;
                }
            }
        }
        return false;
    }

    /// The default value of parameter symbol `parameter`: its declaration's child after the type,
    /// array sizes and semantic.
    [[nodiscard]] uint32 findDefault(uint32 parameter) const noexcept
    {
        uint32 child = nextSibling(firstChild(getSymbol(parameter).Declaration));
        while (child != NO_NODE && (getKind(child) == SyntaxKind::ArraySize || getKind(child) == SyntaxKind::Semantic))
        {
            child = nextSibling(child);
        }
        return child;
    }

    [[nodiscard]] uint32 lowerCall(uint32 node, uint32 type) noexcept
    {
        const CallKind call = getModel().NodeCallKinds[node];
        const uint32 callee = firstChild(node);
        switch (call)
        {
            case CallKind::Intrinsic:
                return lowerPositional(node, IrKind::IntrinsicCall, type, type);
            case CallKind::Construction:
                return lowerPositional(node, IrKind::Construct, type, getElementScalar(type));
            case CallKind::Conversion:
                return lowerPositional(node, IrKind::Convert, type, NO_TYPE);
            case CallKind::ResourceMethod: {
                // The only untyped argument a resource operation takes is SampleLevel's float level.
                const uint32 result =
                    lowerPositional(node, IrKind::ResourceMethodCall, type, getScalarType(ScalarKind::Float32));
                if (result == NO_IR)
                {
                    return NO_IR;
                }
                const uint32 object = lowerValue(firstChild(callee), NO_TYPE);
                if (object == NO_IR)
                {
                    return NO_IR;
                }
                at(object).NextSibling = at(result).FirstChild;
                at(result).FirstChild = object;
                if (at(result).LastChild == NO_IR)
                {
                    at(result).LastChild = object;
                }
                at(result).Token = getSyntax(callee).Token;
                return result;
            }
            case CallKind::Function:
            case CallKind::Method:
                break;
            default:
                return NO_IR;
        }
        // A specialized template is reached through the specialization for these type arguments.
        const uint32 function =
            ResolveSpecialization(mInputs.Model, symbolOf(node), getModel().NodeTypeArguments[node]);
        const Symbol& declared = getSymbol(function);
        const uint32 object = getKind(callee) == SyntaxKind::MemberExpression ? firstChild(callee) : NO_NODE;
        const bool isImplicitThis = call == CallKind::Method && object == NO_NODE;
        Array<uint32> written;
        bool isReordered = false;
        bool hasCall = false;
        for (uint32 argument = nextSibling(callee); argument != NO_NODE; argument = nextSibling(argument))
        {
            const uint32 position = getModel().NodeParameters[argument];
            isReordered = isReordered == true || (written.IsEmpty() == false && position < written.GetLast());
            hasCall = hasCall == true || containsCall(firstChild(argument)) == true;
            if (written.TryAdd(position) == false)
            {
                failOutOfMemory();
                return NO_IR;
            }
        }
        if (isReordered == true && hasCall == true)
        {
            return lowerAdaptedCall(node, function, object, isImplicitThis, written, type);
        }
        const uint32 result = addNode(call == CallKind::Method ? IrKind::MethodCall : IrKind::Call, node);
        if (result == NO_IR)
        {
            return NO_IR;
        }
        at(result).Type = type;
        at(result).Symbol = function;
        at(result).Value = static_cast<int64>(getModel().NodeTypeArguments[node]);
        if (isImplicitThis == true)
        {
            at(result).Flags |= IR_IMPLICIT_THIS;
        }
        if (object != NO_NODE)
        {
            appendChild(result, lowerValue(object, NO_TYPE));
        }
        for (uint32 position = 0; position < declared.MemberCount && mIsFatal == false; ++position)
        {
            const uint32 parameter = getModel().Members[declared.FirstMember + position];
            uint32 value = NO_NODE;
            for (uint32 argument = nextSibling(callee); argument != NO_NODE; argument = nextSibling(argument))
            {
                if (getModel().NodeParameters[argument] == position)
                {
                    value = firstChild(argument);
                }
            }
            if (value == NO_NODE)
            {
                value = findDefault(parameter);
            }
            appendChild(result, lowerArgument(value, parameter));
        }
        return result;
    }

    [[nodiscard]] uint32 lowerArgument(uint32 value, uint32 parameter) noexcept
    {
        const uint32 argument = addNode(IrKind::Argument, value);
        if (argument == NO_IR)
        {
            return NO_IR;
        }
        at(argument).Direction = getSymbol(parameter).Direction;
        at(argument).Type = getSymbol(parameter).Type;
        appendChild(argument, lowerValue(value, getSymbol(parameter).Type));
        return argument;
    }

    /// A call whose named arguments are written out of parameter order and may have effects:
    /// a generated adapter takes them in written order, so they are evaluated as written.
    [[nodiscard]] uint32 lowerAdaptedCall(uint32 node,
                                          uint32 function,
                                          uint32 object,
                                          bool isImplicitThis,
                                          const Array<uint32>& written,
                                          uint32 type) noexcept
    {
        IrHelper helper;
        helper.Kind = IrHelperKind::CallAdapter;
        helper.Symbol = function;
        helper.Type = type;
        const uint32 helperIndex =
            findOrAddHelper(helper, foundation::BorrowedView<const uint32>(written.GetData(), written.GetSize()), node);
        const uint32 result = addNode(IrKind::AdaptedCall, node);
        if (result == NO_IR || helperIndex == NO_INDEX)
        {
            return NO_IR;
        }
        at(result).Type = type;
        at(result).Symbol = function;
        at(result).Index = helperIndex;
        at(result).Value = static_cast<int64>(getModel().NodeTypeArguments[node]);
        if (isImplicitThis == true)
        {
            at(result).Flags |= IR_IMPLICIT_THIS;
        }
        if (object != NO_NODE)
        {
            appendChild(result, lowerValue(object, NO_TYPE));
        }
        const Symbol& declared = getSymbol(function);
        for (uint32 argument = nextSibling(firstChild(node)); argument != NO_NODE && mIsFatal == false;
             argument = nextSibling(argument))
        {
            const uint32 position = getModel().NodeParameters[argument];
            appendChild(result,
                        lowerArgument(firstChild(argument), getModel().Members[declared.FirstMember + position]));
        }
        if (mModule.Helpers[helperIndex].Body == NO_IR)
        {
            lowerAdapterBody(helperIndex, function, node);
        }
        return result;
    }

    /// The body of a call adapter: the positional call, taking written arguments from the
    /// adapter's parameters and defaults for the rest.
    void lowerAdapterBody(uint32 helperIndex, uint32 function, uint32 origin) noexcept
    {
        const Symbol& declared = getSymbol(function);
        const bool isMethod = declared.Parent != NO_SYMBOL;
        const uint32 body = addNode(isMethod == true ? IrKind::MethodCall : IrKind::Call, origin);
        if (body == NO_IR)
        {
            return;
        }
        mModule.Helpers[helperIndex].Body = body;
        at(body).Symbol = function;
        at(body).Type = declared.Type;
        at(body).Value = ADAPTER_TYPE_ARGUMENTS;
        if (isMethod == true)
        {
            at(body).Flags |= IR_IMPLICIT_THIS;
        }
        const IrHelper helper = mModule.Helpers[helperIndex];
        for (uint32 position = 0; position < declared.MemberCount && mIsFatal == false; ++position)
        {
            const uint32 parameter = getModel().Members[declared.FirstMember + position];
            uint32 written = NO_INDEX;
            for (uint32 k = 0; k < helper.FieldCount; ++k)
            {
                if (mModule.HelperFields[helper.FirstField + k] == position)
                {
                    written = k;
                }
            }
            if (written == NO_INDEX)
            {
                appendChild(body, lowerArgument(findDefault(parameter), parameter));
                continue;
            }
            const uint32 argument = addChild(body, IrKind::Argument, NO_NODE);
            const uint32 value = addChild(argument, IrKind::HelperParameter, NO_NODE);
            if (value == NO_IR)
            {
                return;
            }
            at(argument).Direction = getSymbol(parameter).Direction;
            at(argument).Type = getSymbol(parameter).Type;
            at(value).Index = written;
            at(value).Type = getSymbol(parameter).Type;
        }
    }

    /// A built-in call, construction or conversion: positional values, each meeting
    /// `argumentContext` when it is untyped.
    [[nodiscard]] uint32 lowerPositional(uint32 node, IrKind kind, uint32 type, uint32 argumentContext) noexcept
    {
        const uint32 result = addNode(kind, node);
        if (result == NO_IR)
        {
            return NO_IR;
        }
        at(result).Type = type;
        at(result).Symbol = symbolOf(node);
        for (uint32 argument = nextSibling(firstChild(node)); argument != NO_NODE && mIsFatal == false;
             argument = nextSibling(argument))
        {
            appendChild(result, lowerValue(firstChild(argument), argumentContext));
        }
        return result;
    }

    const AnalysisInputs& mInputs;
    const EmitOptions& mOptions;
    DiagnosticSink& mSink;
    IrModule& mModule;
    foundation::StaticArray<uint32, SCALAR_KIND_COUNT> mScalarTypes = {{
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
        NO_TYPE,
    }};
    Array<uint32> mClearedGroupshared;
    Array<uint32> mStack;
    Array<char> mPathScratch;
    const SemanticModel* mModel;
    uint32 mReturnType = NO_TYPE;
    bool mIsFatal = false;
};

// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

void LowerModule(const AnalysisInputs& inputs,
                 const EmitOptions& options,
                 DiagnosticSink& inoutSink,
                 IrModule& outModule) noexcept
{
    Lowering lowering(inputs, options, inoutSink, outModule);
    lowering.Run();
}
} // namespace ludus::tooling::shader_language::internal
