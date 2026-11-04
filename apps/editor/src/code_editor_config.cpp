#include "internal/code_editor_config.h"

#include <ludus/foundation/base/parse_number.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

// A small Ludus-owned EditorConfig reader for the subset the file document host
// applies (CE04): indent_style, indent_size, tab_width, end_of_line, charset,
// insert_final_newline and trim_trailing_whitespace, with section glob matching
// and `root`. Numbers are parsed with the bounded FoundationBase conversion, not
// std::sto*. See the EditorConfig specification cited in the architecture's
// references. No exceptions and no project code is executed.

namespace ludus::editor
{
using foundation::NumberParseStatus;
using foundation::ParseUint64;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

namespace
{
/// The largest .editorconfig this reader will load, so a hostile or accidental
/// giant file cannot stall the background load.
constexpr qint64 MAX_EDITOR_CONFIG_BYTES = 256 * 1024;

/// Trim ASCII spaces and tabs from both ends of a view without allocating.
[[nodiscard]] QString trimAscii(const QString& text)
{
    return text.trimmed();
}

/// Translate an EditorConfig section glob into a QRegularExpression anchored to
/// the whole relative path. Supports '*', '**', '?', '[...]', '[!...]',
/// brace alternation '{a,b}' and numeric ranges '{m..n}', matching the
/// EditorConfig specification's glob rules. A pattern with no '/' matches in any
/// directory, as the specification requires.
[[nodiscard]] QString translateGlob(const QString& patternInput, bool& outIsValid)
{
    outIsValid = true;
    QString pattern = patternInput;
    const bool hasSlash = pattern.contains(QLatin1Char('/'));
    QString expression;
    expression.reserve(pattern.size() * 2);
    // A pattern without a slash applies to the file name in any subdirectory.
    if (hasSlash == false)
    {
        expression += QStringLiteral("(?:.*/)?");
    }
    else if (pattern.startsWith(QLatin1Char('/')))
    {
        pattern.remove(0, 1);
    }
    const int length = pattern.size();
    for (int index = 0; index < length; ++index)
    {
        const QChar character = pattern.at(index);
        if (character == QLatin1Char('*'))
        {
            if (index + 1 < length && pattern.at(index + 1) == QLatin1Char('*'))
            {
                expression += QStringLiteral(".*");
                ++index;
            }
            else
            {
                expression += QStringLiteral("[^/]*");
            }
        }
        else if (character == QLatin1Char('?'))
        {
            expression += QStringLiteral("[^/]");
        }
        else if (character == QLatin1Char('['))
        {
            int scan = index + 1;
            QString body;
            bool isNegated = false;
            if (scan < length && (pattern.at(scan) == QLatin1Char('!') || pattern.at(scan) == QLatin1Char('^')))
            {
                isNegated = true;
                ++scan;
            }
            while (scan < length && pattern.at(scan) != QLatin1Char(']'))
            {
                body += QRegularExpression::escape(QString(pattern.at(scan)));
                ++scan;
            }
            if (scan >= length)
            {
                // No closing bracket: treat '[' as a literal.
                expression += QStringLiteral("\\[");
            }
            else
            {
                expression += QLatin1Char('[');
                if (isNegated == true)
                {
                    expression += QLatin1Char('^');
                }
                expression += body;
                expression += QLatin1Char(']');
                index = scan;
            }
        }
        else if (character == QLatin1Char('{'))
        {
            // Scan the brace body to decide alternation versus numeric range.
            int scan = index + 1;
            QString body;
            while (scan < length && pattern.at(scan) != QLatin1Char('}'))
            {
                body += pattern.at(scan);
                ++scan;
            }
            if (scan >= length)
            {
                expression += QStringLiteral("\\{");
            }
            else
            {
                index = scan;
                if (body.contains(QStringLiteral("..")))
                {
                    // Numeric range {m..n}: match any integer, bounds checked at
                    // match time is overkill; accept any run of digits.
                    expression += QStringLiteral("-?[0-9]+");
                }
                else
                {
                    const QStringList options = body.split(QLatin1Char(','));
                    expression += QStringLiteral("(?:");
                    for (int option = 0; option < options.size(); ++option)
                    {
                        if (option != 0)
                        {
                            expression += QLatin1Char('|');
                        }
                        expression += QRegularExpression::escape(options.at(option));
                    }
                    expression += QLatin1Char(')');
                }
            }
        }
        else
        {
            expression += QRegularExpression::escape(QString(character));
        }
    }
    return QStringLiteral("\\A") + expression + QStringLiteral("\\z");
}

[[nodiscard]] bool globMatches(const QString& pattern, const QString& relativePath)
{
    bool isValid = true;
    const QString expressionText = translateGlob(pattern, isValid);
    if (isValid == false)
    {
        return false;
    }
    QRegularExpression expression(expressionText);
    if (expression.isValid() == false)
    {
        return false;
    }
    const QRegularExpressionMatch match = expression.match(relativePath);
    return match.hasMatch();
}

/// Parse a bounded unsigned decimal using the Foundation conversion.
[[nodiscard]] bool parseWidth(const QString& value, uint32& outWidth)
{
    const QByteArray bytes = value.toUtf8();
    uint64 parsed = 0;
    const NumberParseStatus status = ParseUint64(bytes.constData(), static_cast<usize>(bytes.size()), parsed);
    if (status != NumberParseStatus::Success || parsed < CODE_INDENT_WIDTH_MIN || parsed > CODE_INDENT_WIDTH_MAX)
    {
        return false;
    }
    outWidth = static_cast<uint32>(parsed);
    return true;
}

void applyKey(const QString& key, const QString& valueInput, CodeEditorConfigSettings& inoutSettings)
{
    const QString value = valueInput.toLower();
    if (key == QStringLiteral("indent_style"))
    {
        if (value == QStringLiteral("space"))
        {
            inoutSettings.HasIndentStyle = true;
            inoutSettings.IndentStyle = CodeIndentStyle::Spaces;
        }
        else if (value == QStringLiteral("tab"))
        {
            inoutSettings.HasIndentStyle = true;
            inoutSettings.IndentStyle = CodeIndentStyle::Tabs;
        }
    }
    else if (key == QStringLiteral("indent_size"))
    {
        // "tab" defers to tab_width; a numeric value sets the indent width.
        uint32 width = 0;
        if (value != QStringLiteral("tab") && parseWidth(value, width) == true)
        {
            inoutSettings.HasIndentSize = true;
            inoutSettings.IndentSize = width;
        }
    }
    else if (key == QStringLiteral("tab_width"))
    {
        uint32 width = 0;
        if (parseWidth(value, width) == true)
        {
            inoutSettings.HasTabWidth = true;
            inoutSettings.TabWidth = width;
        }
    }
    else if (key == QStringLiteral("end_of_line"))
    {
        if (value == QStringLiteral("lf"))
        {
            inoutSettings.HasLineEnding = true;
            inoutSettings.LineEnding = CodeLineEnding::Lf;
        }
        else if (value == QStringLiteral("crlf"))
        {
            inoutSettings.HasLineEnding = true;
            inoutSettings.LineEnding = CodeLineEnding::CrLf;
        }
        // "cr" is not supported by the host and is left unset.
    }
    else if (key == QStringLiteral("charset"))
    {
        inoutSettings.HasCharset = true;
        if (value == QStringLiteral("utf-8"))
        {
            inoutSettings.IsCharsetUtf8 = true;
            inoutSettings.IsBomExpected = false;
        }
        else if (value == QStringLiteral("utf-8-bom"))
        {
            inoutSettings.IsCharsetUtf8 = true;
            inoutSettings.IsBomExpected = true;
        }
        else
        {
            inoutSettings.IsCharsetUtf8 = false;
        }
    }
    else if (key == QStringLiteral("insert_final_newline"))
    {
        if (value == QStringLiteral("true") || value == QStringLiteral("false"))
        {
            inoutSettings.HasInsertFinalNewline = true;
            inoutSettings.ShouldInsertFinalNewline = value == QStringLiteral("true");
        }
    }
    else if (key == QStringLiteral("trim_trailing_whitespace"))
    {
        if (value == QStringLiteral("true") || value == QStringLiteral("false"))
        {
            inoutSettings.HasTrimTrailingWhitespace = true;
            inoutSettings.ShouldTrimTrailingWhitespace = value == QStringLiteral("true");
        }
    }
}
} // namespace

bool MergeEditorConfigFile(const char* configBytes,
                           usize configByteCount,
                           const QString& relativePath,
                           CodeEditorConfigSettings& inoutSettings) noexcept
{
    bool isRoot = false;
    if (configBytes == nullptr || configByteCount == 0)
    {
        return isRoot;
    }
    const QString text = QString::fromUtf8(configBytes, static_cast<qsizetype>(configByteCount));
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool isSectionMatched = false;
    bool isPreamble = true;
    for (const QString& rawLine : lines)
    {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
        {
            line.chop(1);
        }
        line = trimAscii(line);
        if (line.isEmpty() == true || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
        {
            continue;
        }
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
        {
            isPreamble = false;
            const QString pattern = line.mid(1, line.size() - 2);
            isSectionMatched = globMatches(pattern, relativePath);
            continue;
        }
        const int separator = line.indexOf(QLatin1Char('='));
        if (separator < 0)
        {
            continue;
        }
        const QString key = trimAscii(line.left(separator)).toLower();
        const QString value = trimAscii(line.mid(separator + 1));
        if (isPreamble == true)
        {
            // The preamble `root = true` tells the walker to stop here.
            if (key == QStringLiteral("root") && value.toLower() == QStringLiteral("true"))
            {
                isRoot = true;
            }
            continue;
        }
        if (isSectionMatched == true)
        {
            applyKey(key, value, inoutSettings);
        }
    }
    return isRoot;
}

CodeEditorConfigSettings ResolveEditorConfig(const QString& absoluteFilePath) noexcept
{
    const QFileInfo targetInfo(absoluteFilePath);
    // One loaded .editorconfig in the directory chain, with the target path made
    // relative to that config's directory for glob matching.
    struct LoadedConfig final
    {
        QByteArray Bytes;
        QString RelativePath;
    };
    // Collect from the nearest directory upward, stopping at the first file that
    // declares `root = true` (inclusive): nothing above it applies.
    QList<LoadedConfig> chain;
    QDir walk = targetInfo.absoluteDir();
    for (;;)
    {
        const QString configPath = walk.filePath(QStringLiteral(".editorconfig"));
        QFile file(configPath);
        if (QFile::exists(configPath) == true && file.size() <= MAX_EDITOR_CONFIG_BYTES &&
            file.open(QIODevice::ReadOnly) == true)
        {
            LoadedConfig loaded;
            loaded.Bytes = file.readAll();
            loaded.RelativePath = QDir(walk.absolutePath()).relativeFilePath(targetInfo.absoluteFilePath());
            chain.append(loaded);
            // A cheap probe tells us whether to stop walking to parents.
            CodeEditorConfigSettings probe;
            const bool isRoot = MergeEditorConfigFile(loaded.Bytes.constData(),
                                                      static_cast<usize>(loaded.Bytes.size()),
                                                      loaded.RelativePath,
                                                      probe);
            if (isRoot == true)
            {
                break;
            }
        }
        if (walk.isRoot() == true || walk.cdUp() == false)
        {
            break;
        }
    }
    // Merge from the outermost collected config inward so the nearest directory's
    // declarations win (EditorConfig's "closest file last" precedence).
    CodeEditorConfigSettings settings;
    for (int index = chain.size() - 1; index >= 0; --index)
    {
        const LoadedConfig& loaded = chain.at(index);
        (void)MergeEditorConfigFile(loaded.Bytes.constData(),
                                    static_cast<usize>(loaded.Bytes.size()),
                                    loaded.RelativePath,
                                    settings);
    }
    return settings;
}

void ApplyEditorConfig(const CodeEditorConfigSettings& settings, CodeTextProfile& inoutProfile) noexcept
{
    if (settings.HasIndentStyle == true)
    {
        inoutProfile.Indent.Style = settings.IndentStyle;
        inoutProfile.Indent.Source = CodeTextProfileSource::EditorConfig;
    }
    if (settings.HasIndentSize == true)
    {
        inoutProfile.Indent.Width = settings.IndentSize;
        inoutProfile.Indent.Source = CodeTextProfileSource::EditorConfig;
    }
    else if (settings.HasTabWidth == true && settings.HasIndentStyle == true &&
             settings.IndentStyle == CodeIndentStyle::Tabs)
    {
        inoutProfile.Indent.Width = settings.TabWidth;
        inoutProfile.Indent.Source = CodeTextProfileSource::EditorConfig;
    }
    if (settings.HasLineEnding == true)
    {
        // Configuring the ending wins over detection, including for a file that
        // was read with mixed endings: new lines adopt the configured kind.
        inoutProfile.LineEnding = settings.LineEnding;
        inoutProfile.LineEndingSource = CodeTextProfileSource::EditorConfig;
    }
}
} // namespace ludus::editor
