// LUSL style rules (docs/architecture/shading-language.md#style-rules): the engine's naming rules
// and explicit boolean comparisons. They report as the style setting says; language rules do not
// depend on it.

#include "internal/analysis.hpp"

#include <ludus/foundation/strings/static_string.hpp>

#include "internal/fix_builder.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::uint32;
using foundation::usize;

/// Longest suggested name in a naming message.
constexpr usize MAX_SUGGESTION_BYTES = 128;
using SuggestionText = foundation::StaticString<MAX_SUGGESTION_BYTES>;

[[nodiscard]] bool IsUpper(char value) noexcept
{
    return value >= 'A' && value <= 'Z';
}

[[nodiscard]] bool IsLower(char value) noexcept
{
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] char ToUpper(char value) noexcept
{
    return IsLower(value) == true ? static_cast<char>(value - 'a' + 'A') : value;
}

[[nodiscard]] char ToLower(char value) noexcept
{
    return IsUpper(value) == true ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool IsPascal(std::string_view name) noexcept
{
    return name.empty() == false && IsUpper(name[0]) == true && name.find('_') == std::string_view::npos;
}

[[nodiscard]] bool IsCamel(std::string_view name) noexcept
{
    return name.empty() == false && IsLower(name[0]) == true && name.find('_') == std::string_view::npos;
}

[[nodiscard]] bool IsUpperCase(std::string_view name) noexcept
{
    if (name.empty() == true || IsUpper(name[0]) == false)
    {
        return false;
    }
    for (const char value : name)
    {
        if (IsLower(value) == true)
        {
            return false;
        }
    }
    return true;
}

/// True when `name` starts with `prefix` followed by an uppercase letter.
[[nodiscard]] bool HasWordPrefix(std::string_view name, std::string_view prefix) noexcept
{
    return name.size() > prefix.size() && name.starts_with(prefix) == true && IsUpper(name[prefix.size()]) == true;
}

/// True when a boolean name reads as a question: is, has, should or can, then an uppercase letter.
[[nodiscard]] bool IsQuestion(std::string_view name, bool isPascal) noexcept
{
    if (isPascal == true)
    {
        return HasWordPrefix(name, "Is") == true || HasWordPrefix(name, "Has") == true ||
               HasWordPrefix(name, "Should") == true || HasWordPrefix(name, "Can") == true;
    }
    return HasWordPrefix(name, "is") == true || HasWordPrefix(name, "has") == true ||
           HasWordPrefix(name, "should") == true || HasWordPrefix(name, "can") == true;
}

/// The words of `name`, split at underscores and lowercase-to-uppercase steps, in PascalCase.
[[nodiscard]] SuggestionText MakePascal(std::string_view name) noexcept
{
    SuggestionText text;
    bool isWordStart = true;
    for (usize i = 0; i < name.size(); ++i)
    {
        const char value = name[i];
        if (value == '_')
        {
            isWordStart = true;
            continue;
        }
        const char spelled = isWordStart == true ? ToUpper(value) : value;
        (void)text.TryAppend(std::string_view(&spelled, 1));
        isWordStart = false;
    }
    return text;
}

[[nodiscard]] SuggestionText MakeCamel(std::string_view name) noexcept
{
    SuggestionText text = MakePascal(name);
    if (text.IsEmpty() == false)
    {
        SuggestionText lowered;
        const char first = ToLower(text.GetView()[0]);
        (void)lowered.TryAppend(std::string_view(&first, 1));
        (void)lowered.TryAppend(text.GetView().substr(1));
        return lowered;
    }
    return text;
}

[[nodiscard]] SuggestionText MakeUpperCase(std::string_view name) noexcept
{
    SuggestionText text;
    for (usize i = 0; i < name.size(); ++i)
    {
        const char value = name[i];
        if (i > 0 && IsUpper(value) == true && IsLower(name[i - 1]) == true)
        {
            (void)text.TryAppend("_");
        }
        const char spelled = ToUpper(value);
        (void)text.TryAppend(std::string_view(&spelled, 1));
    }
    return text;
}

/// `prefix` followed by `name` in PascalCase, such as `outRadiance` or `isVisible`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prefix, then the name it precedes.
[[nodiscard]] SuggestionText MakePrefixed(std::string_view prefix, std::string_view name) noexcept
{
    SuggestionText text;
    (void)text.TryAssign(prefix);
    const SuggestionText rest = MakePascal(name);
    (void)text.TryAppend(rest.GetView());
    return text;
}

class StyleChecker final
{
public:
    StyleChecker(const AnalysisInputs& inputs, Severity level, DiagnosticSink& inoutSink) noexcept
        : mInputs(inputs), mLevel(level), mSink(inoutSink)
    {
    }

    void Run() noexcept
    {
        for (uint32 symbol = 0; symbol < mInputs.Model.Symbols.GetSize(); ++symbol)
        {
            checkName(symbol);
        }
        for (uint32 node = 0; node < mInputs.Tree.Nodes.GetSize(); ++node)
        {
            checkConditions(node);
        }
    }

private:
    [[nodiscard]] const SyntaxNode& getNode(uint32 node) const noexcept
    {
        return mInputs.Tree.Nodes[node];
    }

    [[nodiscard]] std::string_view spell(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mInputs.Request, mInputs.Preprocessed, mInputs.Preprocessed.Tokens[token]);
    }

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
        mSink.Report(mLevel, code, location, expansion, message, fix);
    }

    [[nodiscard]] bool isBoolType(uint32 type) const noexcept
    {
        if (type == NO_TYPE || type >= mInputs.Model.Types.GetSize())
        {
            return false;
        }
        const SemanticType& shape = mInputs.Model.Types[type];
        return shape.Kind == TypeKind::Scalar && shape.Scalar == ScalarKind::Bool;
    }

    void reportName(const Symbol& symbol, std::string_view rule, const SuggestionText& suggestion) noexcept
    {
        const std::string_view name = spell(symbol.NameToken);
        report(DiagnosticCode::NamingStyle,
               symbol.NameToken,
               ComposeMessage("'", name, "' should be ", rule, ", for example '", suggestion.GetView(), "'"),
               DiagnosticFix{});
    }

    void checkName(uint32 index) noexcept
    {
        const Symbol& symbol = mInputs.Model.Symbols[index];
        if (symbol.Declaration == NO_NODE || symbol.IsExtern == true || symbol.NameToken == NO_TOKEN ||
            mInputs.Model.NodeSymbols[symbol.Declaration] != index)
        {
            // Built-ins, `extern slang` mirrors of Slang names, and definitions merged into an
            // earlier declaration are not checked.
            return;
        }
        const std::string_view name = spell(symbol.NameToken);
        const bool isBool = isBoolType(symbol.Type);
        switch (symbol.Kind)
        {
            case SymbolKind::Struct:
            case SymbolKind::Enum:
            case SymbolKind::Function:
            case SymbolKind::Enumerator:
            case SymbolKind::Interface:
            case SymbolKind::TypeParameter:
            case SymbolKind::Concept:
            case SymbolKind::Space:
                if (IsPascal(name) == false)
                {
                    reportName(symbol, "PascalCase", MakePascal(name));
                }
                return;
            case SymbolKind::Field:
                if (IsPascal(name) == false)
                {
                    reportName(symbol, "PascalCase", MakePascal(name));
                }
                else if (isBool == true && IsQuestion(name, true) == false)
                {
                    reportName(symbol, "a question, starting with Is, Has, Should or Can", MakePrefixed("Is", name));
                }
                return;
            case SymbolKind::Parameter:
                checkParameter(symbol, name, isBool);
                return;
            case SymbolKind::Variable: {
                const bool isConstant = getNode(symbol.Declaration).IsStatic == true && symbol.IsConstant == true;
                if (isConstant == true)
                {
                    if (IsUpperCase(name) == false)
                    {
                        reportName(symbol, "UPPER_CASE, as a constant", MakeUpperCase(name));
                    }
                    return;
                }
                if (IsCamel(name) == false)
                {
                    reportName(symbol, "lowerCamelCase", MakeCamel(name));
                }
                else if (isBool == true && IsQuestion(name, false) == false)
                {
                    reportName(symbol, "a question, starting with is, has, should or can", MakePrefixed("is", name));
                }
                return;
            }
            default:
                return;
        }
    }

    void checkParameter(const Symbol& symbol, std::string_view name, bool isBool) noexcept
    {
        if (symbol.Direction == ParameterDirection::Out || symbol.Direction == ParameterDirection::Inout)
        {
            const std::string_view prefix = symbol.Direction == ParameterDirection::Out ? "out" : "inout";
            if (HasWordPrefix(name, prefix) == false || name.find('_') != std::string_view::npos)
            {
                reportName(symbol,
                           symbol.Direction == ParameterDirection::Out ? "'out' and PascalCase"
                                                                       : "'inout' and PascalCase",
                           MakePrefixed(prefix, name));
                return;
            }
            const std::string_view rest = name.substr(prefix.size());
            if (isBool == true && IsQuestion(rest, true) == false)
            {
                SuggestionText suggestion;
                (void)suggestion.TryAssign(prefix);
                const SuggestionText question = MakePrefixed("Is", rest);
                (void)suggestion.TryAppend(question.GetView());
                reportName(symbol, "a question after its prefix, such as outIsVisible", suggestion);
            }
            return;
        }
        if (IsCamel(name) == false)
        {
            reportName(symbol, "lowerCamelCase", MakeCamel(name));
        }
        else if (isBool == true && IsQuestion(name, false) == false)
        {
            reportName(symbol, "a question, starting with is, has, should or can", MakePrefixed("is", name));
        }
    }

    /// True for a boolean expression that is already a comparison, a logical operation or a literal.
    [[nodiscard]] bool isExplicit(uint32 node) const noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        switch (syntax.Kind)
        {
            case SyntaxKind::BooleanLiteral:
                return true;
            case SyntaxKind::ParenthesizedExpression:
                return syntax.FirstChild != NO_NODE && isExplicit(syntax.FirstChild) == true;
            case SyntaxKind::BinaryExpression: {
                const std::string_view operation = spell(syntax.Token);
                return operation == "==" || operation == "!=" || operation == "<" || operation == "<=" ||
                       operation == ">" || operation == ">=" || operation == "&&" || operation == "||";
            }
            default:
                return false;
        }
    }

    void checkOperand(uint32 node) noexcept
    {
        if (node == NO_NODE || getNode(node).Kind == SyntaxKind::Empty ||
            isBoolType(mInputs.Model.NodeTypes[node]) == false || isExplicit(node) == true)
        {
            return;
        }
        const SyntaxKind kind = getNode(node).Kind;
        const bool isPostfix = kind == SyntaxKind::Name || kind == SyntaxKind::MemberExpression ||
                               kind == SyntaxKind::CallExpression || kind == SyntaxKind::IndexExpression ||
                               kind == SyntaxKind::ParenthesizedExpression;
        DiagnosticFix fix;
        if (isPostfix == true)
        {
            FixBuilder builder(mInputs.Request, mInputs.Preprocessed);
            builder.InsertAfter(getNode(node).LastToken, " == true");
            fix = builder.GetFix();
        }
        report(DiagnosticCode::BareBoolOperand,
               getNode(node).FirstToken,
               ComposeMessage("compare the bool explicitly, for example 'value == true'"),
               fix);
    }

    [[nodiscard]] uint32 skipAttributes(uint32 child) const noexcept
    {
        while (child != NO_NODE && getNode(child).Kind == SyntaxKind::Attribute)
        {
            child = getNode(child).NextSibling;
        }
        return child;
    }

    void checkConditions(uint32 node) noexcept
    {
        const SyntaxNode& syntax = getNode(node);
        const uint32 first = skipAttributes(syntax.FirstChild);
        switch (syntax.Kind)
        {
            case SyntaxKind::IfStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::ConditionalExpression:
                checkOperand(first);
                return;
            case SyntaxKind::DoStatement:
            case SyntaxKind::ForStatement:
                // The condition follows the body of a do loop and the initializer of a for loop.
                checkOperand(first == NO_NODE ? NO_NODE : getNode(first).NextSibling);
                return;
            case SyntaxKind::BinaryExpression: {
                const std::string_view operation = spell(syntax.Token);
                if (operation == "&&" || operation == "||")
                {
                    checkOperand(first);
                    checkOperand(first == NO_NODE ? NO_NODE : getNode(first).NextSibling);
                }
                return;
            }
            default:
                return;
        }
    }

    const AnalysisInputs& mInputs;
    Severity mLevel = Severity::Error;
    DiagnosticSink& mSink;
};
} // namespace

bool CheckStyle(const AnalysisInputs& inputs, StyleSetting setting, DiagnosticSink& inoutSink) noexcept
{
    if (setting == StyleSetting::Off)
    {
        return true;
    }
    StyleChecker checker(inputs, setting == StyleSetting::Strict ? Severity::Error : Severity::Warning, inoutSink);
    checker.Run();
    return inoutSink.HasOutOfMemory() == false;
}
} // namespace ludus::tooling::shader_language::internal
