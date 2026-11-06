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
/// Symbol::Location and Symbol::Slot of a symbol that has none.
inline constexpr foundation::uint32 NO_LOCATION = 0xffffffffU;

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
    /// An interface; SemanticType::Symbol names it. Interfaces constrain template parameters and
    /// are not value types.
    Interface,
    /// A template type parameter; SemanticType::Symbol names it. Its constraints are the
    /// SemanticModel::Conformances whose Type it is.
    TypeParameter,
    /// A shader resource handle; SemanticType::Resource says which, and SemanticType::Element is
    /// its element type (NO_TYPE for a sampler).
    Resource,
};

/// @brief A kind of shader resource; see "Resources and bindings" in docs/architecture/shading-language.md.
enum class ResourceKind : foundation::uint8
{
    Texture2D,          ///< `Texture2D<T>`: sampled and loaded.
    RWTexture2D,        ///< `RWTexture2D<T>`: read and written by pixel.
    Sampler,            ///< `SamplerState`.
    ConstantBuffer,     ///< `ConstantBuffer<S>`: uniform values of a struct.
    StructuredBuffer,   ///< `StructuredBuffer<T>`: read by index.
    RWStructuredBuffer, ///< `RWStructuredBuffer<T>`: read and written by index.
};

/// The pipeline stage an entry point runs in.
enum class EntryStage : foundation::uint8
{
    None,     ///< Not an entry point.
    Compute,  ///< `[stage(compute)]`.
    Vertex,   ///< `[stage(vertex)]`.
    Fragment, ///< `[stage(fragment)]`.
};

/// @brief What a vertex or fragment entry point's parameter or struct field carries.
///
/// Locations of attributes, varyings and targets are assigned by rule; see Symbol::Location.
enum class StageValue : foundation::uint8
{
    None,          ///< Not part of a stage interface.
    Attribute,     ///< A vertex input attribute.
    Varying,       ///< A stage-interface field other than the clip position.
    ClipPosition,  ///< The stage interface's `: ClipPosition` field.
    Target,        ///< A fragment color target.
    Depth,         ///< A fragment output's `: Depth` field.
    VertexIndex,   ///< A vertex entry point's `: VertexIndex` parameter.
    InstanceIndex, ///< A vertex entry point's `: InstanceIndex` parameter.
    FrontFacing,   ///< A fragment entry point's `: FrontFacing` parameter.
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
    /// Kind of a Resource.
    ResourceKind Resource = ResourceKind::Texture2D;
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
    /// An `interface`; its members are its method and constant requirements.
    Interface,
    /// An `extension` of a struct; its members are the methods and constants it adds.
    Extension,
    /// A template type parameter of a function or concept.
    TypeParameter,
    /// A `concept`; its type parameters are TypeParameterCount symbols from FirstTypeParameter, and
    /// Declaration is the ConceptDeclaration whose last child is the constraint.
    Concept,
};

/// @brief A declared entity.
///
/// A function's parameters; a struct's fields, constants and methods; an interface's requirements;
/// an extension's methods and constants; and an enum's enumerators are its members: MemberCount
/// symbol indices starting at FirstMember in SemanticModel::Members, in declaration order. A
/// template function's type parameters are TypeParameterCount symbols starting at
/// FirstTypeParameter in SemanticModel::TypeParameters.
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
    /// True for a parameter declared `uniform`: every invocation that makes a call passes the same
    /// value, so the body may decide on it before a derivative operation.
    bool IsUniform = false;
    /// True for a function parameter with a default value.
    bool HasDefault = false;
    /// True when IntegerValue holds the value of an integer or enumerator constant.
    bool HasIntegerValue = false;
    /// True for a function with a body.
    bool IsDefined = false;
    /// True for a template that is specialized per concrete type rather than lowered to a Slang
    /// generic: it has concept or `requires` constraints, uses `if constexpr`, or calls such a
    /// template. Its body is checked once per SemanticModel::Specializations entry.
    bool IsSpecialized = false;
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
    /// First type parameter of a template function in SemanticModel::TypeParameters.
    foundation::uint32 FirstTypeParameter = 0;
    /// Number of type parameters; zero for a function that is not a template.
    foundation::uint32 TypeParameterCount = 0;
    /// Stage of an entry point function.
    EntryStage Stage = EntryStage::None;
    /// What a stage-interface parameter or field carries.
    StageValue Value = StageValue::None;
    /// Location of an Attribute, Varying or Target, assigned in declaration order; NO_LOCATION
    /// otherwise.
    foundation::uint32 Location = NO_LOCATION;
    /// Binding slot of a resource variable, assigned in declaration order; NO_LOCATION for
    /// variables that are not bindings, including `extern slang` resources.
    foundation::uint32 Slot = NO_LOCATION;
};

/// @brief One concrete instance of a specialized template.
///
/// The template's syntax nodes [FirstNode, FirstNode + NodeCount) are checked again with its type
/// parameters bound to TypeArguments, and the facts of that check are stored in the Specialized*
/// arrays of the model from FirstFact on, in node order.
struct Specialization final
{
    /// The template function symbol.
    foundation::uint32 Template = NO_SYMBOL;
    /// The function symbol the instance declares, with concrete parameter types.
    foundation::uint32 Function = NO_SYMBOL;
    /// First of the template's TypeParameterCount type arguments in SemanticModel::TypeArguments.
    foundation::uint32 FirstTypeArgument = 0;
    /// First syntax node of the template's FunctionDeclaration subtree.
    foundation::uint32 FirstNode = NO_NODE;
    /// Number of nodes in that subtree.
    foundation::uint32 NodeCount = 0;
    /// First entry of this instance in the Specialized* arrays.
    foundation::uint32 FirstFact = 0;
};

/// @brief A type that conforms to an interface.
///
/// Recorded for a struct's base list, an extension, an interface's base interfaces and a template
/// parameter's constraint. Conformance is transitive through interface bases.
struct Conformance final
{
    /// The conforming struct, interface or type parameter type.
    foundation::uint32 Type = NO_TYPE;
    /// The interface symbol it conforms to.
    foundation::uint32 Interface = NO_SYMBOL;
    /// The declaration that states it: a struct, interface, extension or template parameter node.
    foundation::uint32 Declaration = NO_NODE;
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
    /// An operation of a resource, such as `image.Sample(imageSampler, uv)`; lowered to the Slang
    /// method of that name.
    ResourceMethod,
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
    /// For a call of a template function, the first of its deduced type arguments in
    /// TypeArguments, one per type parameter; NO_SYMBOL for other nodes.
    foundation::Array<foundation::uint32> NodeTypeArguments;
    /// Deduced type arguments of template calls; see NodeTypeArguments.
    foundation::Array<foundation::uint32> TypeArguments;
    /// Type parameter symbols of template functions; see Symbol::FirstTypeParameter.
    foundation::Array<foundation::uint32> TypeParameters;
    /// Every stated conformance, in declaration order.
    foundation::Array<Conformance> Conformances;
    /// Instances of specialized templates, in the order calls first required them.
    foundation::Array<Specialization> Specializations;
    /// Resource variables that are bindings, in slot order: entry `k` has Symbol::Slot `k`.
    foundation::Array<foundation::uint32> Bindings;
    /// NodeTypes of each specialization's nodes; see Specialization::FirstFact.
    foundation::Array<foundation::uint32> SpecializedNodeTypes;
    /// NodeSymbols of each specialization's nodes.
    foundation::Array<foundation::uint32> SpecializedNodeSymbols;
    /// NodeParameters of each specialization's nodes.
    foundation::Array<foundation::uint32> SpecializedNodeParameters;
    /// NodeCallKinds of each specialization's nodes.
    foundation::Array<CallKind> SpecializedNodeCallKinds;
    /// NodeIntegerConstants of each specialization's nodes; an `if constexpr` node holds its
    /// condition's value.
    foundation::Array<IntegerConstant> SpecializedNodeIntegerConstants;
    /// NodeTypeArguments of each specialization's nodes.
    foundation::Array<foundation::uint32> SpecializedNodeTypeArguments;
    /// Reported problems in the order found; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// How LUSL style rules (naming and bare `bool` operands) report.
enum class StyleSetting : foundation::uint8
{
    /// Style findings are errors. Always used for the engine repository; the default.
    Strict,
    /// Style findings are warnings.
    Warn,
    /// Style findings are not reported.
    Off,
};

/// Options of Check.
struct CheckOptions final
{
    /// How style rules report; language rules are errors regardless.
    StyleSetting Style = StyleSetting::Strict;
    /// Most template specializations the translation unit may create. Values above
    /// MAX_SPECIALIZATIONS mean MAX_SPECIALIZATIONS. A call that needs one more is a
    /// `specialization-budget` error, so a package keeps its compile cost within what it declared.
    foundation::usize SpecializationBudget = MAX_SPECIALIZATIONS;
};

/// @brief Check the types, names and calls of a parsed translation unit.
///
/// Resolves names in C order (a name is visible after its declaration), interns types, gives every
/// expression a type, and binds calls to overloads, intrinsics or conversions. Enforces the L1
/// rules the design lists under conversions, literals, zero initialization, calls and arrays:
/// no implicit conversions except untyped constants that fit their target, the literal `0`
/// zero-filling any non-opaque type, matched `out`/`inout` marks, no aliasing between writable
/// arguments, named arguments with defaults, designated initializers, named array sizes and
/// `bool` conditions.
///
/// When those pass, flow and style rules run: definite assignment (`= void` opts out), `out`
/// parameters written before every return, a return on every path, no recursion, `groupshared`
/// declared `= 0` or `= void`, `switch` cases that end explicitly, then the naming and bare-`bool`
/// style rules as `options.Style` says. When the flow rules pass, the uniformity rules reject
/// derivative operations (`Sample`, `ddx`, `ddy`, and calls of functions that perform them) in
/// non-uniform control flow and varying arguments of `uniform` parameters
/// (docs/architecture/shading-language.md, "Uniformity"). Diagnostics carry a DiagnosticFix where
/// a repair is safe.
///
/// Deterministic and reentrant; keeps no state between calls.
/// @param request The request given to Preprocess; its file texts must still be alive.
/// @param preprocessed The Ready Preprocess result the tree was parsed from.
/// @param tree A Ready result of Parse for `preprocessed`.
/// @param options Style setting.
/// @param outModel Receives the model; any previous contents are replaced.
/// @return The same value as `outModel.Outcome`; InvalidRequest when `preprocessed` or `tree` is not
/// Ready. Style warnings alone leave the outcome Ready.
[[nodiscard]] Status Check(const PreprocessRequest& request,
                           const PreprocessResult& preprocessed,
                           const SyntaxTree& tree,
                           const CheckOptions& options,
                           SemanticModel& outModel) noexcept;

/// @brief Check with the default options (strict style).
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
