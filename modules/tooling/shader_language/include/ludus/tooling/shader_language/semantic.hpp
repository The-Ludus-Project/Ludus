#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/status.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

/// @file
/// The LUSL semantic checker and its model: the third front-end stage
/// (docs/modules/tooling/shader-language.md).
namespace ludus::tooling::shader_language
{
/// Type index that names no type.
inline constexpr foundation::uint32 NO_TYPE = 0xffffffffU;
/// Symbol index that names no symbol.
inline constexpr foundation::uint32 NO_SYMBOL = 0xffffffffU;

/// Category of a semantic type.
enum class TypeKind : foundation::uint8
{
    /// Produced where checking failed; compatible with everything to avoid cascading diagnostics.
    Error,
    /// `void`, the return type of a function without a value.
    Void,
    /// A scalar; SemanticType::Scalar says which.
    Scalar,
    /// A vector of SemanticType::Rows components.
    Vector,
    /// A matrix of SemanticType::Rows rows and SemanticType::Columns columns.
    Matrix,
    /// A fixed-size array of SemanticType::Count elements of SemanticType::Element.
    Array,
    /// A struct declared in LUSL or in an `extern slang` block; SemanticType::Symbol names it.
    Struct,
    /// An `enum class`; SemanticType::Symbol names it and SemanticType::Scalar is its underlying type.
    Enum,
    /// An integer literal or integer constant expression that has not met a target type yet.
    UntypedInteger,
    /// A floating literal or floating constant expression that has not met a target type yet.
    UntypedFloat,
};

/// Element type of a scalar, vector or matrix, and the underlying type of an enum.
enum class ScalarKind : foundation::uint8
{
    Bool,    ///< `bool`.
    Int16,   ///< `int16`.
    Uint16,  ///< `uint16`.
    Int32,   ///< `int32`, alias `int`.
    Uint32,  ///< `uint32`, alias `uint`.
    Int64,   ///< `int64`.
    Uint64,  ///< `uint64`.
    Float16, ///< `float16`, alias `half`.
    Float32, ///< `float32`, alias `float`.
    Float64, ///< `float64`, alias `double`.
};

/// @brief One entry of SemanticModel::Types. Types are interned: equal types have equal indices.
struct SemanticType final
{
    /// Category.
    TypeKind Kind = TypeKind::Error;
    /// Element type of a Scalar, Vector or Matrix; underlying type of an Enum.
    ScalarKind Scalar = ScalarKind::Bool;
    /// Components of a Vector, rows of a Matrix; one otherwise.
    foundation::uint8 Rows = 1;
    /// Columns of a Matrix; one otherwise.
    foundation::uint8 Columns = 1;
    /// Element type of an Array.
    foundation::uint32 Element = NO_TYPE;
    /// Element count of an Array.
    foundation::uint32 Count = 0;
    /// Declaring symbol of a Struct or Enum.
    foundation::uint32 Symbol = NO_SYMBOL;
};

/// What a symbol declares.
enum class SymbolKind : foundation::uint8
{
    /// A local, global or `static const` variable.
    Variable,
    /// A function parameter.
    Parameter,
    /// A function or method, declared in LUSL or an `extern slang` block.
    Function,
    /// A built-in function such as `dot` or `normalize`, lowered to the Slang intrinsic of that name.
    Intrinsic,
    /// A struct.
    Struct,
    /// A struct field.
    Field,
    /// An `enum class`.
    Enum,
    /// An enumerator.
    Enumerator,
};

/// @brief A declared entity.
///
/// A function's parameters, a struct's fields and methods, and an enum's enumerators are its
/// members: MemberCount symbol indices starting at FirstMember in SemanticModel::Members, in
/// declaration order.
struct Symbol final
{
    /// What the symbol declares.
    SymbolKind Kind = SymbolKind::Variable;
    /// Direction of a Parameter.
    ParameterDirection Direction = ParameterDirection::Unmarked;
    /// True for a `static const` or `const` variable, an enumerator, or a constant parameter default.
    bool IsConstant = false;
    /// True when declared inside an `extern slang` block.
    bool IsExtern = false;
    /// True for an `extern slang` struct declared without a body (`struct Name;`), whose layout
    /// LUSL does not know.
    bool IsOpaque = false;
    /// True for a method marked `[mutating]`, which may assign the fields of its struct.
    bool IsMutating = false;
    /// True for a `groupshared` variable.
    bool IsGroupshared = false;
    /// True for a function parameter with a default value.
    bool HasDefault = false;
    /// True when IntegerValue holds the value of an integer or enumerator constant.
    bool HasIntegerValue = false;
    /// True for a function with a body.
    bool IsDefined = false;
    /// The declaring syntax node, or NO_NODE for built-ins.
    foundation::uint32 Declaration = NO_NODE;
    /// The name token in the preprocessed tokens, or NO_TOKEN for built-ins.
    foundation::uint32 NameToken = NO_TOKEN;
    /// Type of a variable, parameter or field; return type of a function; the type an enum,
    /// enumerator or struct declares.
    foundation::uint32 Type = NO_TYPE;
    /// Struct of a field or method; enum of an enumerator; function of a parameter.
    foundation::uint32 Parent = NO_SYMBOL;
    /// First member in SemanticModel::Members.
    foundation::uint32 FirstMember = 0;
    /// Number of members.
    foundation::uint32 MemberCount = 0;
    /// Value of an integer constant or enumerator.
    foundation::int64 IntegerValue = 0;
};

/// How a CallExpression is lowered.
enum class CallKind : foundation::uint8
{
    /// Not a call, or the call did not check.
    None,
    /// A LUSL or `extern slang` function; SemanticModel::NodeSymbols names it.
    Function,
    /// A method of a struct value; SemanticModel::NodeSymbols names it.
    Method,
    /// A built-in function; SemanticModel::NodeSymbols names it.
    Intrinsic,
    /// A vector or matrix built from components, such as `float32x3(x, y, z)` or `float32x3(1.0)`.
    Construction,
    /// An explicit conversion of one value, such as `float32(count)` or `Mode(word)`.
    Conversion,
};

/// The value of an integer or boolean constant expression, as SemanticModel records it per node.
struct IntegerConstant final
{
    /// True when the node is a constant expression whose value is known.
    bool IsKnown = false;
    /// The value; booleans are 0 and 1.
    foundation::int64 Value = 0;
};

/// Text of a type name such as `float32x4x4`; long names are clipped.
inline constexpr foundation::usize MAX_TYPE_NAME_BYTES = 128;
/// Bounded spelling of a type, as written by FormatTypeName.
using TypeNameText = foundation::StaticString<MAX_TYPE_NAME_BYTES>;

/// @brief Output of Check: types, symbols and per-node facts.
///
/// Per-node arrays have one entry per SyntaxTree node, in the tree's order.
struct SemanticModel final
{
    /// Final outcome.
    Status Outcome = Status::InvalidRequest;
    /// Interned types.
    foundation::Array<SemanticType> Types;
    /// Every declared symbol, built-ins first.
    foundation::Array<Symbol> Symbols;
    /// Members of functions, structs and enums; see Symbol::FirstMember.
    foundation::Array<foundation::uint32> Members;
    /// Type of each expression node, and the declared type of each declaration; NO_TYPE elsewhere.
    foundation::Array<foundation::uint32> NodeTypes;
    /// Symbol a Name, MemberExpression, ScopeExpression or CallExpression refers to, the symbol a
    /// declaration declares, or the field a FieldInitializer sets; NO_SYMBOL elsewhere.
    foundation::Array<foundation::uint32> NodeSymbols;
    /// Parameter position each Argument binds to; NO_SYMBOL for other nodes.
    foundation::Array<foundation::uint32> NodeParameters;
    /// How each CallExpression is lowered; CallKind::None for other nodes.
    foundation::Array<CallKind> NodeCallKinds;
    /// Value of each integer or boolean constant expression node. Untyped integer arithmetic is
    /// exact (`1 / 2` is 0), so later stages emit these values rather than the expression.
    foundation::Array<IntegerConstant> NodeIntegerConstants;
    /// Reported problems in the order found; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Check the types, names and calls of a parsed translation unit.
///
/// Resolves names in C order (a name is visible after its declaration), interns types, gives every
/// expression a type, and binds calls to overloads, intrinsics or conversions. Enforces the L1
/// rules the design lists under conversions, literals, zero initialization, calls and arrays:
/// no implicit conversions except untyped constants that fit their target, the literal `0`
/// zero-filling any non-opaque type, matched `out`/`inout` marks, no aliasing between writable
/// arguments, named arguments with defaults, designated initializers, named array sizes and
/// `bool` conditions. Flow rules (definite assignment, `= void`, returns on every path) and style
/// rules arrive in a later stage.
///
/// Deterministic and reentrant; keeps no state between calls.
/// @param request The request given to Preprocess; its file texts must still be alive.
/// @param preprocessed The Ready Preprocess result the tree was parsed from.
/// @param tree A Ready result of Parse for `preprocessed`.
/// @param outModel Receives the model; any previous contents are replaced.
/// @return The same value as `outModel.Outcome`; InvalidRequest when `preprocessed` or `tree` is not
/// Ready.
[[nodiscard]] Status Check(const PreprocessRequest& request,
                           const PreprocessResult& preprocessed,
                           const SyntaxTree& tree,
                           SemanticModel& outModel) noexcept;

/// @brief Write the LUSL spelling of `type`, such as `float32x3`, `Hit` or `uint32[4]`.
/// @param model The model that owns `type`.
/// @param type A type index of `model`, or NO_TYPE.
/// @param request The request whose tokens spell declared names.
/// @param preprocessed The preprocessing result whose tokens spell declared names.
/// @return The spelling, clipped to MAX_TYPE_NAME_BYTES; `<error>` for an invalid index.
[[nodiscard]] TypeNameText FormatTypeName(const SemanticModel& model,
                                          foundation::uint32 type,
                                          const PreprocessRequest& request,
                                          const PreprocessResult& preprocessed) noexcept;
} // namespace ludus::tooling::shader_language
