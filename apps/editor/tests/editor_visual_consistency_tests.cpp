// Visual consistency: spacing comes from the design-token scale and every toolbar command draws from one icon
// set. The spacing check reads the Editor's sources, so a new hard-coded margin fails here rather than in review.

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>

using ludus::foundation::int32;

namespace
{
/// Layout calls whose arguments are distances in pixels.
const QRegularExpression& GetSpacingCall()
{
    static const QRegularExpression SPACING_CALL(QStringLiteral(
        R"(\b(setContentsMargins|setSpacing|addSpacing|setHorizontalSpacing|setVerticalSpacing)\s*\(([^;{}]*)\))"));
    return SPACING_CALL;
}

/// A constant named like a layout distance: it must be built from design tokens, not typed in. Control padding
/// belongs to the style's own metrics and is not matched.
const QRegularExpression& GetSpacingConstant()
{
    static const QRegularExpression SPACING_CONSTANT(
        QStringLiteral(R"(constexpr\s+(?:int|int32|foundation::int32)\s+(\w*(?:SPACING|MARGIN)\w*)\s*=\s*([^;]+);)"));
    return SPACING_CONSTANT;
}

/// Distances that are not steps of the spacing scale: the code editor's gutter and callout metrics follow its glyph
/// size, and the command palette's top offset places a floating surface rather than spacing anything.
bool IsExemptConstant(const QString& relativePath, const QString& name)
{
    return relativePath == QStringLiteral("code_editor.cpp") ||
           (relativePath == QStringLiteral("command_palette.cpp") && name == QStringLiteral("PALETTE_TOP_MARGIN"));
}

/// Source text without comments and string literals, so a number in either is not a finding.
QString StripCommentsAndStrings(const QString& text)
{
    QString stripped = text;
    stripped.remove(QRegularExpression(QStringLiteral(R"(//[^\n]*)")));
    stripped.remove(QRegularExpression(QStringLiteral(R"(/\*.*?\*/)"), QRegularExpression::DotMatchesEverythingOption));
    stripped.replace(QRegularExpression(QStringLiteral(R"("(?:\\.|[^"\\])*")")), QStringLiteral("\"\""));
    return stripped;
}

/// Whether an argument is a pixel number typed in place: any digit not part of an identifier or a token name.
bool IsTypedNumber(const QString& argument)
{
    const QString trimmed = argument.trimmed();
    if (trimmed == QStringLiteral("0") || trimmed.isEmpty() == true)
    {
        return false;
    }
    static const QRegularExpression TYPED_NUMBER(QStringLiteral(R"((?<![\w:])\d+(?![\w:]))"));
    return TYPED_NUMBER.match(trimmed).hasMatch();
}

QStringList FindSpacingViolations()
{
    QStringList violations;
    const QString root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/apps/editor/src");
    QDirIterator files(root,
                       {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                       QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext() == true)
    {
        const QString path = files.next();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text) == false)
        {
            continue;
        }
        const QString text = StripCommentsAndStrings(QString::fromUtf8(file.readAll()));
        const QString name = QDir(root).relativeFilePath(path);
        for (QRegularExpressionMatchIterator calls = GetSpacingCall().globalMatch(text); calls.hasNext() == true;)
        {
            const QRegularExpressionMatch call = calls.next();
            for (const QString& argument : call.captured(2).split(QLatin1Char(',')))
            {
                if (IsTypedNumber(argument) == true)
                {
                    violations << QStringLiteral("%1: %2(%3)").arg(name, call.captured(1), call.captured(2).trimmed());
                }
            }
        }
        for (QRegularExpressionMatchIterator constants = GetSpacingConstant().globalMatch(text);
             constants.hasNext() == true;)
        {
            const QRegularExpressionMatch constant = constants.next();
            if (IsExemptConstant(name, constant.captured(1)) == false && IsTypedNumber(constant.captured(2)) == true)
            {
                violations
                    << QStringLiteral("%1: %2 = %3").arg(name, constant.captured(1), constant.captured(2).trimmed());
            }
        }
    }
    violations.sort();
    return violations;
}
} // namespace

TEST_CASE("The spacing lint recognises typed pixel numbers", "[editor][style][spacing]")
{
    CHECK(IsTypedNumber(QStringLiteral("12")) == true);
    CHECK(IsTypedNumber(QStringLiteral(" 2 ")) == true);
    CHECK(IsTypedNumber(QStringLiteral("0")) == false);
    CHECK(IsTypedNumber(QStringLiteral("design::SPACING_SMALL")) == false);
    CHECK(IsTypedNumber(QStringLiteral("SOURCE_PAGE_MARGIN")) == false);
    CHECK(IsTypedNumber(QStringLiteral("design::SPACING_SMALL - design::SPACING_EXTRA_SMALL / 2")) == true);
    CHECK(IsTypedNumber(QStringLiteral("design::SPACING_MEDIUM")) == false);
    CHECK(IsTypedNumber(QStringLiteral("spacingPixels")) == false);
    const QString source =
        StripCommentsAndStrings(QStringLiteral("a->setSpacing(8); // setSpacing(12)\nb(\"setSpacing(4)\");"));
    CHECK(GetSpacingCall().match(source).captured(2) == QStringLiteral("8"));
    CHECK(GetSpacingCall().globalMatch(source).hasNext() == true);
}

TEST_CASE("Editor layout spacing comes from the design-token scale", "[editor][style][spacing]")
{
    const QStringList violations = FindSpacingViolations();
    INFO(violations.join(QLatin1Char('\n')).toStdString());
    CHECK(violations.isEmpty() == true);
}
