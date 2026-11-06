#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/status.hpp>

#include <string_view>

/// @file
/// The LUSL syntax tree and parser: the second front-end stage (docs/modules/tooling/shader-language.md).
namespace ludus::tooling::shader_language
{
/// SyntaxNode index that names no node.
inline constexpr foundation::uint32 NO_NODE = 0xffffffffU;
/// SyntaxNode::Token of a node without a principal token, such as a positional Argument.
inline constexpr foundation::uint32 NO_TOKEN = 0xffffffffU;

/// @brief What a syntax node represents, and the order of its children.
///
/// "Attribute*" means zero or more Attribute children come first. A child marked "?" may be absent;
/// where a later child depends on position, an Empty node holds the place instead.
enum class SyntaxKind : foundation::uint8
{
    /// Root. Children: the top-level declarations in source order.
    TranslationUnit,

    /// `static const`, `const`, `groupshared` or plain variable. Token: the name. Children:
    /// Attribute*, Type, ArraySize*, then the initializer expression? (VoidInitializer for `= void`).
    VariableDeclaration,
    /// Function or method. Token: the name. Children: Attribute*, the return Type, Parameter*, then
    /// the body Block? (absent for a declaration that ends in `;`).
    FunctionDeclaration,
    /// Function parameter. Token: the name. Children: Type, ArraySize*, Semantic?, default value?.
    /// SyntaxNode::Direction says `in`, `out` or `inout`.
    Parameter,
    /// System-value annotation such as `: DispatchThreadId`. Token: the semantic name.
    Semantic,
    /// `struct Name { ... };`. Token: the name. Children: Attribute*, then VariableDeclaration and
    /// FunctionDeclaration members. `struct Name;` declares an opaque `extern slang` type: the
    /// token after the name is `;` and there are no members.
    StructDeclaration,
    /// `enum class Name : Type { ... };`. Token: the name. Children: the underlying Type, Enumerator*.
    EnumDeclaration,
    /// One enumerator. Token: the name. Children: the value expression?.
    Enumerator,
    /// `extern slang "path" { ... }`. Token: the path string. Children: the declarations. In
    /// `extern slang { ... }`, whose definitions the including Slang code provides, Token is `slang`.
    ExternSlangBlock,
    /// `static_assert(condition, "message");`. Token: `static_assert`. Children: the condition and the
    /// message StringLiteral.
    StaticAssert,
    /// `[name]` or `[name(arguments)]`. Token: the name. Children: the argument expressions.
    Attribute,

    /// A type name, possibly scoped (`A::B`) and generic (`Texture2D<float32x4>`). Token: the last
    /// name; FirstToken to LastToken spell it. Children: the generic arguments, each a Type or an
    /// expression.
    Type,
    /// `[size]` after a declared name. Children: the size expression.
    ArraySize,

    /// `{ ... }`. Children: the statements.
    Block,
    /// A call used as a statement. Children: the CallExpression.
    ExpressionStatement,
    /// `target = value;` or a compound assignment. Token: the operator. Children: target, value.
    AssignmentStatement,
    /// `target++;`, `++target;` and the `--` forms. Token: the operator. Children: target.
    IncrementStatement,
    /// Children: Attribute*, condition, the statement when true, the `else` statement?.
    IfStatement,
    /// Children: Attribute*, initializer statement, condition, increment statement, body; an absent
    /// part is Empty.
    ForStatement,
    /// Children: Attribute*, condition, body.
    WhileStatement,
    /// `do body while (condition);`. Children: Attribute*, body, condition.
    DoStatement,
    /// Children: Attribute*, the selector, CaseClause*.
    SwitchStatement,
    /// `case value:` or `default:` and its statements. Token: `case` or `default`. Children: the
    /// value expression (for `case` only), then the statements.
    CaseClause,
    /// `break;`.
    BreakStatement,
    /// `continue;`.
    ContinueStatement,
    /// `return value?;`. Children: the value?.
    ReturnStatement,
    /// `discard;`.
    DiscardStatement,
    /// `[[fallthrough]];` at the end of a CaseClause.
    FallthroughStatement,

    /// An identifier in an expression. Token: the identifier.
    Name,
    /// `left::right`. Token: `::`. Children: the left Name or ScopeExpression, the right Name.
    ScopeExpression,
    /// A preprocessing number. Token: the number; later stages check its value.
    NumberLiteral,
    /// `true` or `false`. Token: the keyword.
    BooleanLiteral,
    /// A string. Token: the string; strings appear in attributes and `static_assert` only.
    StringLiteral,
    /// `(expression)`. Children: the expression.
    ParenthesizedExpression,
    /// Prefix `+`, `-` or `~`. Token: the operator. Children: the operand.
    UnaryExpression,
    /// Binary operator. Token: the operator. Children: left, right.
    BinaryExpression,
    /// `condition ? whenTrue : whenFalse`. Children: the three operands.
    ConditionalExpression,
    /// A call or construction. Children: the callee expression, Argument*.
    CallExpression,
    /// One call argument. Token: the parameter name of a named argument, otherwise NO_TOKEN.
    /// Children: the value. SyntaxNode::Direction records an `out` or `inout` mark.
    Argument,
    /// `object.member`. Token: the member name. Children: the object.
    MemberExpression,
    /// `object[index]`. Children: object, index.
    IndexExpression,
    /// Designated initializer `{ .A = a, .B = b }`. Token: `{`. Children: FieldInitializer*.
    InitializerList,
    /// `.Name = value` in an InitializerList. Token: the field name. Children: the value.
    FieldInitializer,
    /// `void` in `= void`, which leaves a variable uninitialized. Token: `void`.
    VoidInitializer,

    /// Placeholder for an absent positional child, such as an omitted `for` condition.
    Empty,
    /// Placeholder where a syntax error left no valid node; a diagnostic describes it.
    Error,
};

/// Direction of a Parameter, or the mark on an Argument.
enum class ParameterDirection : foundation::uint8
{
    /// No keyword: `in` for a parameter, an ordinary argument.
    Unmarked,
    /// `in`, written explicitly on a parameter.
    In,
    /// `out`.
    Out,
    /// `inout`.
    Inout,
};

/// @brief One node of a syntax tree.
///
/// Token indices refer to the PreprocessResult the tree was parsed from. Children form a list from
/// FirstChild through NextSibling, in the order SyntaxKind documents.
struct SyntaxNode final
{
    /// What the node represents.
    SyntaxKind Kind = SyntaxKind::Error;
    /// `in`, `out` or `inout` on a Parameter or Argument.
    ParameterDirection Direction = ParameterDirection::Unmarked;
    /// `static` on a VariableDeclaration.
    bool IsStatic = false;
    /// `const` on a VariableDeclaration.
    bool IsConst = false;
    /// `groupshared` on a VariableDeclaration.
    bool IsGroupshared = false;
    /// The principal token SyntaxKind names, or NO_TOKEN.
    foundation::uint32 Token = NO_TOKEN;
    /// First token of the node.
    foundation::uint32 FirstToken = NO_TOKEN;
    /// Last token of the node, inclusive.
    foundation::uint32 LastToken = NO_TOKEN;
    /// First child, or NO_NODE.
    foundation::uint32 FirstChild = NO_NODE;
    /// Next child of the same parent, or NO_NODE.
    foundation::uint32 NextSibling = NO_NODE;
};

/// @brief Output of Parse.
///
/// On Ready, node zero is the TranslationUnit. On Failed the tree is still published, with Error
/// nodes where recovery skipped tokens, so editors can show structure; later stages must not
/// consume a failed tree.
struct SyntaxTree final
{
    /// Final outcome.
    Status Outcome = Status::InvalidRequest;
    /// Every node, linked into a tree from node zero.
    foundation::Array<SyntaxNode> Nodes;
    /// Reported problems in the order found; at most MAX_DIAGNOSTICS.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Parse a preprocessed translation unit.
///
/// The grammar enforces LUSL's syntactic rules: one declarator per declaration, assignment and
/// `++`/`--` only as statements, no unary `!`, no C casts, positional arguments before named ones,
/// and only calls as expression statements. Constructs of later phases (`interface`, `concept`,
/// `template`, ...) are reported as such. After an error the parser skips to the next statement or
/// declaration and continues, so one request reports several independent errors.
///
/// Deterministic and reentrant; keeps no state between calls.
/// @param request The request given to Preprocess; its file texts must still be alive.
/// @param preprocessed A Ready result of Preprocess for `request`.
/// @param outTree Receives the tree; any previous contents are replaced.
/// @return The same value as `outTree.Outcome`; InvalidRequest when `preprocessed` is not Ready.
[[nodiscard]] Status
Parse(const PreprocessRequest& request, const PreprocessResult& preprocessed, SyntaxTree& outTree) noexcept;

/// @brief Return the stable kebab-case name of a node kind, such as `variable-declaration`.
/// @return An empty view for a value outside the enumerators.
[[nodiscard]] std::string_view GetSyntaxKindName(SyntaxKind kind) noexcept;
} // namespace ludus::tooling::shader_language
