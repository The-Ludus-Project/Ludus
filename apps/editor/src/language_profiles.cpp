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
        .RequiredCapabilities = {QStringLiteral("completionProvider"), QStringLiteral("signatureHelpProvider")},
        .RequiredProtocol = QStringLiteral("UTF-16 completion/signature"),
        .ConfigurationResponder = &ConfigureSlangSection,
    };
    return PROFILE;
}
} // namespace ludus::editor
