#include "internal/language_profiles.h"

#include <QJsonArray>

namespace ludus::editor
{
namespace
{
QJsonValue ConfigureSlangSection(const QString& section)
{
    if (section == QStringLiteral("slang.predefinedMacros") || section == QStringLiteral("slang.additionalSearchPaths"))
    {
        return QJsonArray{};
    }
    if (section == QStringLiteral("slang.searchInAllWorkspaceDirectories"))
    {
        return false;
    }
    // Inlay hints are drawn or hidden by the view's setting; the server always computes them.
    if (section == QStringLiteral("slang.inlayHints.deducedTypes") ||
        section == QStringLiteral("slang.inlayHints.parameterNames"))
    {
        return true;
    }
    return QJsonValue::Null;
}

QJsonValue ConfigureNoSection(const QString& section)
{
    static_cast<void>(section);
    return QJsonValue::Null;
}
} // namespace

const LanguageServerProfile& GetSlangServerProfile()
{
    static const LanguageServerProfile PROFILE
    {
        .LanguageId = QStringLiteral("slang"),
        .ProductName = QStringLiteral("Slang"),
        .ServerName = QStringLiteral("slangd"),
        .AssistanceTitle = QStringLiteral("Shader assistance"),
        .AssistanceNoun = QStringLiteral("shader assistance"),
        .ContextNoun = QStringLiteral("shader context"),
        .SyntaxNoun = QStringLiteral("shader syntax"),
        .StartRepair = QStringLiteral("Repair project shader tools or check the compiler override."),
        .ConfigurationResponder = &ConfigureSlangSection,
        .RequestTimeoutMilliseconds = LANGUAGE_SERVER_REQUEST_TIMEOUT_MILLISECONDS,
    };
    return PROFILE;
}

const LanguageServerProfile& GetLuslServerProfile()
{
    static const LanguageServerProfile PROFILE
    {
        .LanguageId = QStringLiteral("lusl"),
        .ProductName = QStringLiteral("LUSL"),
        .ServerName = QStringLiteral("luslc"),
        .AssistanceTitle = QStringLiteral("LUSL assistance"),
        .AssistanceNoun = QStringLiteral("LUSL assistance"),
        .ContextNoun = QStringLiteral("shader context"),
        .SyntaxNoun = QStringLiteral("LUSL syntax"),
        .StartRepair = QStringLiteral("Repair project tools or set LUDUS_LUSLC, then Retry assistance."),
        .ConfigurationResponder = &ConfigureNoSection,
        .RequestTimeoutMilliseconds = LANGUAGE_SERVER_REQUEST_TIMEOUT_MILLISECONDS,
    };
    return PROFILE;
}
} // namespace ludus::editor
