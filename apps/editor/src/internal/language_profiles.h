#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
/// Answers one `workspace/configuration` item for a server. Returns the value for
/// `section`, or null when the editor supplies no setting for it. Called on the
/// GUI thread while a server request is pending; must not block or start work.
using LanguageServerConfigurationResponder = QJsonValue (*)(const QString& section);

/// The language-specific part of one generic LSP connection (code editor M4).
/// The client owns framing, process supervision, decoding and synchronization;
/// a profile names the language identifier it sends, the capabilities it
/// requires before the connection is ready, its `workspace/configuration`
/// answers and the vocabulary of its user-facing messages. A profile never
/// names an executable or adds arguments: resolution stays with the context
/// provider, so a project file cannot replace a server program.
///
/// Profiles are immutable static values; references to them stay valid for the
/// process lifetime and may be read from any thread.
struct LanguageServerProfile final
{
    /// The LSP `languageId` sent with `textDocument/didOpen` ("slang").
    QString LanguageId;
    /// The language or toolchain named in failures ("Slang").
    QString ProductName;
    /// The server executable named in failures ("slangd").
    QString ServerName;
    /// The feature named at the start of a sentence ("Shader assistance").
    QString AssistanceTitle;
    /// The same feature inside a sentence ("shader assistance").
    QString AssistanceNoun;
    /// The context that bounds the virtual document ("shader context").
    QString ContextNoun;
    /// The source the user repairs after a server error ("shader syntax").
    QString SyntaxNoun;
    /// The fix offered when the executable cannot start.
    QString StartRepair;
    /// Server capabilities the initialize response must advertise; otherwise the
    /// connection fails with the pinned-tools fix.
    QStringList RequiredCapabilities;
    /// Describes the required protocol in the unsupported-server failure
    /// ("UTF-16 completion/signature").
    QString RequiredProtocol;
    /// `workspace/configuration` answers; never null.
    LanguageServerConfigurationResponder ConfigurationResponder = nullptr;
};

/// The `slangd` profile from the selected Slang SDK. It also serves LUSL sources
/// until the LUSL profile is added, exactly as before the move to the generic
/// client.
[[nodiscard]] const LanguageServerProfile& GetSlangServerProfile();
} // namespace ludus::editor
