#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/emitter.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include <string_view>

#include "analysis.hpp"
#include "diagnostic_sink.hpp"

/// @file
/// The LUSL IR: a typed, target-independent tree produced by lowering and read by the Slang
/// emitter (docs/architecture/shading-language.md#compiler-architecture).
///
/// Lowering resolves every language rule that has no direct Slang spelling: named arguments,
/// defaults and evaluation order, untyped constants, the zero rule, designated initializers,
/// signed wrapping, shift masking, development bounds checks, `groupshared` clears, case
/// fallthrough and overload names. The emitter then only spells nodes. Types and symbols are
/// indices into the SemanticModel the IR was lowered from.
namespace ludus::tooling::shader_language::internal
{
/// IrNode::Value of a call inside a generic call adapter, whose type arguments are the adapter's
/// own type parameters.
inline constexpr foundation::int64 ADAPTER_TYPE_ARGUMENTS = -2;

/// IR index that names no node.
inline constexpr foundation::uint32 NO_IR = 0xffffffffU;
/// IrNode::Index of a node without a helper, failure code or ordinal.
inline constexpr foundation::uint32 NO_INDEX = 0xffffffffU;

/// What an IR node represents; the comment lists its children in order.
enum class IrKind : foundation::uint8
{
    /// Root. Children: the module items in source order; IrModule::Includes lists the includes.
    Module,
    /// A struct. Symbol: the struct. Children: Field* and Constant* in source order, then Function*
    /// (methods). Its interfaces are the SemanticModel::Conformances its declaration states.
    Struct,
    /// An interface. Symbol: the interface. Children: Constant* without initializers and Function*
    /// without bodies, the requirements.
    Interface,
    /// An extension. Symbol: the extension, whose Parent is the struct. Children: Constant* and
    /// Function*.
    Extension,
    /// A struct field. Symbol: the field. Type: its type.
    Field,
    /// An enum. Symbol: the enum. Children: Enumerator*.
    Enum,
    /// An enumerator. Symbol: the enumerator; Value: its value.
    Enumerator,
    /// A `static const`. Symbol and Type. Children: the initializer.
    Constant,
    /// A `groupshared` variable. Symbol and Type.
    Groupshared,
    /// A resource binding. Symbol and Type; the symbol's Slot is its binding.
    Resource,
    /// A function or method definition, or an interface requirement without a body. Symbol: the
    /// function; a template function's type parameters are its SemanticModel::TypeParameters. Value:
    /// the IrModule::ThreadGroups entry of an entry point. Flags: IR_ENTRY, IR_NOINLINE, IR_FORCEINLINE, IR_MUTATING,
    /// IR_HIDDEN_GROUP_INDEX. Children: Parameter*, then the body Block.
    Function,
    /// A parameter. Symbol and Type; Direction. Index: the system-value semantic for entry points.
    Parameter,

    /// `{ ... }`. Children: statements.
    Block,
    /// A local variable. Symbol and Type. Children: the initializer, absent for `= void` or none.
    Declare,
    /// `target op value`; Operator is `=` or a compound operator. Flags: IR_MASK_SHIFT. Children:
    /// target, value.
    Assign,
    /// `++`/`--`; Operator says which. Children: target.
    Increment,
    /// A wrapping compound assignment or increment of a signed integer. Index: the helper.
    /// Children: target, then the value for an assignment.
    WrappedUpdate,
    /// A call evaluated for its effect. Children: the call.
    Evaluate,
    /// Flags: IR_BRANCH, IR_FLATTEN. Children: condition, then statement, else statement or absent.
    If,
    /// Flags: IR_UNROLL (Value: count, 0 for none), IR_LOOP. Children: initializer, condition,
    /// increment and body, each Empty when absent.
    For,
    /// Flags as For. Children: condition, body.
    While,
    /// Flags as For. Children: body, condition.
    Do,
    /// Flags: IR_BRANCH, IR_FLATTEN. Children: selector, Case*.
    Switch,
    /// One case. Flags: IR_DEFAULT. Children: the label expression (absent for default), then the
    /// statements, including those of later cases it falls through to.
    Case,
    /// `break;`.
    Break,
    /// `continue;`.
    Continue,
    /// `return value?;`. Children: the value.
    Return,
    /// `discard;`.
    Discard,
    /// Zero-fill one `groupshared` variable cooperatively. Symbol: the variable. Index: the threads
    /// per group. Children: the flattened group thread index.
    ClearGroupshared,
    /// A group memory barrier, after the clears of an entry point.
    GroupBarrier,
    /// Placeholder for an absent For part.
    Empty,

    /// A typed integer or bool constant. Type and Value.
    Literal,
    /// A floating literal spelled by Token. Type: its float type.
    FloatLiteral,
    /// The zero value of Type.
    Zero,
    /// A variable or parameter. Symbol.
    Variable,
    /// A field of the enclosing method's struct, read without an object. Symbol.
    ThisField,
    /// An enumerator, `Enum::Name`. Symbol.
    EnumeratorReference,
    /// A constant member of a struct or a template parameter's constraint, `Owner::NAME`. Symbol: the
    /// constant. Index: the owner struct or type parameter symbol.
    TypeConstant,
    /// `object.field`. Symbol: the field. Children: object.
    Member,
    /// `object.xyz`. Token: the swizzle. Children: object.
    Swizzle,
    /// `object[index]`. Index: the failure code of a bounds check, or NO_INDEX. Children: object,
    /// index.
    IndexAccess,
    /// Prefix `+`, `-` or `~`. Operator. Children: operand.
    Unary,
    /// A binary operator. Operator. Flags: IR_MASK_SHIFT. Children: left, right.
    Binary,
    /// Linear-algebra product of a matrix with a matrix or vector. Children: left, right.
    MatrixProduct,
    /// A helper call that wraps a signed `+`, `-`, `*` or negation. Index: the helper. Children:
    /// the operands.
    WrappedOperation,
    /// `condition ? a : b`. Children: condition, a, b.
    Conditional,
    /// A call of a LUSL or extern function. Symbol: the function. Value: for a template function,
    /// the first of its type arguments in SemanticModel::TypeArguments, or ADAPTER_TYPE_ARGUMENTS in
    /// a generic adapter's body. Children: Argument* in parameter order, defaults included.
    Call,
    /// A method call. Symbol: the method. Flags: IR_IMPLICIT_THIS. Children: the object unless
    /// implicit, then Argument* in parameter order.
    MethodCall,
    /// A call through a generated adapter that evaluates arguments in written order. Index: the
    /// helper. Symbol: the function or method. Flags: IR_IMPLICIT_THIS. Children: the object for a
    /// method with one, then Argument* in written order.
    AdaptedCall,
    /// A built-in function. Symbol: the intrinsic. Children: the arguments.
    IntrinsicCall,
    /// An operation of a resource, such as `image.Sample(imageSampler, uv)`. Token: the operation
    /// name. Children: the resource, then the arguments.
    ResourceMethodCall,
    /// A vector or matrix construction. Type. Children: the components.
    Construct,
    /// An explicit conversion. Type. Children: the value.
    Convert,
    /// A designated initializer, lowered to a helper call. Index: the helper. Children: the field
    /// values in written order.
    Initialize,
    /// One call argument. Direction. Children: the value.
    Argument,
    /// The hidden `SV_GroupIndex` parameter of an entry point that clears `groupshared` memory.
    HiddenGroupIndex,
    /// Parameter Index of the generated helper whose body contains the node.
    HelperParameter,
};

/// IrNode::Flags bits.
inline constexpr foundation::uint16 IR_ENTRY = 1U << 0U;
inline constexpr foundation::uint16 IR_NOINLINE = 1U << 1U;
inline constexpr foundation::uint16 IR_FORCEINLINE = 1U << 2U;
inline constexpr foundation::uint16 IR_MUTATING = 1U << 3U;
inline constexpr foundation::uint16 IR_HIDDEN_GROUP_INDEX = 1U << 4U;
inline constexpr foundation::uint16 IR_MASK_SHIFT = 1U << 5U;
inline constexpr foundation::uint16 IR_BRANCH = 1U << 6U;
inline constexpr foundation::uint16 IR_FLATTEN = 1U << 7U;
inline constexpr foundation::uint16 IR_UNROLL = 1U << 8U;
inline constexpr foundation::uint16 IR_LOOP = 1U << 9U;
inline constexpr foundation::uint16 IR_DEFAULT = 1U << 10U;
inline constexpr foundation::uint16 IR_IMPLICIT_THIS = 1U << 11U;

/// System-value semantics of compute entry point parameters, stored in IrNode::Index.
enum class IrSemantic : foundation::uint8
{
    None,
    DispatchThreadId,
    GroupThreadId,
    GroupId,
    GroupIndex,
};

/// @brief One IR node. Children form a list from FirstChild through NextSibling.
struct IrNode final
{
    IrKind Kind = IrKind::Empty;
    /// Operator of Unary, Binary, Assign and Increment nodes.
    Punctuator Operator = Punctuator::None;
    /// Direction of a Parameter or Argument.
    ParameterDirection Direction = ParameterDirection::Unmarked;
    /// IR_* bits.
    foundation::uint16 Flags = 0;
    /// SemanticModel type of the value or declaration, or NO_TYPE.
    foundation::uint32 Type = NO_TYPE;
    /// SemanticModel symbol declared or referenced, or NO_SYMBOL.
    foundation::uint32 Symbol = NO_SYMBOL;
    /// The token that spells a FloatLiteral or Swizzle, or NO_TOKEN.
    foundation::uint32 Token = NO_TOKEN;
    /// Where the node comes from, for `#line` and the source map; invalid for generated nodes.
    SourceLocation Location;
    /// Helper, failure code, ordinal, semantic or thread count; see IrKind.
    foundation::uint32 Index = NO_INDEX;
    /// Integer value of a Literal or Enumerator, or the unroll count.
    foundation::int64 Value = 0;
    foundation::uint32 FirstChild = NO_IR;
    foundation::uint32 LastChild = NO_IR;
    foundation::uint32 NextSibling = NO_IR;
};

/// What a generated helper computes.
enum class IrHelperKind : foundation::uint8
{
    /// `Type _luslZeroN()`: the zero value of a struct or array Type.
    Zero,
    /// `Type _luslInitN(written fields)`: a designated initializer of struct Type. Fields: the
    /// written field symbols, in order. Body: a Block with one value per struct field.
    Initializer,
    /// `_luslWrapN(Type a, SecondType b)`: Operator (`+`, `-` or `*`) wrapping on every target.
    /// One operand may be the scalar of the other's vector type.
    WrapBinary,
    /// `Type _luslWrapN(Type a)`: wrapping negation.
    WrapNegate,
    /// `void _luslWrapN(inout Type target, SecondType value)`: wrapping compound assignment;
    /// Operator is the binary operator.
    WrapAssign,
    /// `void _luslWrapN(inout Type target)`: wrapping `++` or `--`; Operator is `+` or `-`.
    WrapIncrement,
    /// `uint _luslCheckIndexN(uint index, uint count, uint code)`: clamp and latch; Type is the
    /// 32-bit or 64-bit unsigned index type.
    CheckIndex,
    /// An adapter that takes arguments in written order. Symbol: the function or method; Type:
    /// its return type. Fields: the parameter position of each written argument. Body: the
    /// positional Call or MethodCall. A method's adapter is a method of the same struct.
    CallAdapter,
};

/// @brief One generated helper; names are `_lusl` + kind + index, so they are deterministic.
struct IrHelper final
{
    IrHelperKind Kind = IrHelperKind::Zero;
    Punctuator Operator = Punctuator::None;
    foundation::uint32 Type = NO_TYPE;
    /// Type of the second operand of WrapBinary and WrapAssign.
    foundation::uint32 SecondType = NO_TYPE;
    foundation::uint32 Symbol = NO_SYMBOL;
    /// IR subtree of an Initializer or CallAdapter body, outside the module tree; else NO_IR.
    foundation::uint32 Body = NO_IR;
    /// First entry in IrModule::HelperFields.
    foundation::uint32 FirstField = 0;
    /// Number of entries in IrModule::HelperFields.
    foundation::uint32 FieldCount = 0;
};

/// Thread-group size of a compute entry point, from `[threads(x, y, z)]`.
struct IrThreadGroup final
{
    foundation::uint32 X = 1;
    foundation::uint32 Y = 1;
    foundation::uint32 Z = 1;
};

/// @brief A lowered translation unit.
struct IrModule final
{
    /// Every node; node zero is the Module.
    foundation::Array<IrNode> Nodes;
    /// Generated helpers in first-use order.
    foundation::Array<IrHelper> Helpers;
    /// Field symbols or parameter positions of Initializer and CallAdapter helpers.
    foundation::Array<foundation::uint32> HelperFields;
    /// Location of each bounds check; failure code `i + 1` is entry `i`.
    foundation::Array<SourceLocation> BoundsChecks;
    /// Request files that `extern slang` blocks name, by index, deduplicated, in first-use order.
    foundation::Array<foundation::uint16> Includes;
    /// Thread-group sizes of entry points; an entry Function's Value indexes it.
    foundation::Array<IrThreadGroup> ThreadGroups;
};

/// The text of a borrowed character view, empty when the view is invalid.
[[nodiscard]] inline std::string_view ViewText(const foundation::BorrowedView<const char>& view) noexcept
{
    return view.IsValid() == true ? std::string_view(view.GetDataOrNull(), view.GetSize()) : std::string_view{};
}

/// @brief Lower a translation unit that checked without errors.
///
/// Reports lowering diagnostics (an `extern slang` file missing from the snapshot, limits) and
/// allocation failure into `inoutSink`.
/// @pre `inputs.Model` is Ready.
void LowerModule(const AnalysisInputs& inputs,
                 const EmitOptions& options,
                 DiagnosticSink& inoutSink,
                 IrModule& outModule) noexcept;

/// @brief Spell a lowered module as Slang, with `#line` directives and its source map.
///
/// Fills Text, SourceMap and BoundsChecks of `outResult`; reports a LoweringLimit when the text
/// would exceed `shader_compiler::MAX_SOURCE_BYTES`.
void EmitModule(const AnalysisInputs& inputs,
                const IrModule& module,
                DiagnosticSink& inoutSink,
                EmitResult& outResult) noexcept;
} // namespace ludus::tooling::shader_language::internal
