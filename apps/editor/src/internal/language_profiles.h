#pragma once

#include <ludus/foundation/base/types.h>

#include <QJsonValue>
#include <QString>

namespace ludus::editor
{
/// Answers one `workspace/configuration` item for a server. Returns the value for
/// `section`, or null when the editor supplies no setting for it. Called on the
/// GUI thread while a server request is pending; must not block or start work.
using LanguageServerConfigurationResponder = QJsonValue (*)(const QString& section);

/// The language-specific part of one generic LSP connection (code editor M4).
/// The client owns framing, process supervision, decoding and synchronization;
/// a profile names the language identifier it sends, its
/// `workspace/configuration` answers and the vocabulary of its user-facing
/// messages. Which requests a server serves comes from its advertised
/// capabilities, not from the profile. A profile never names an executable or
/// adds arguments: resolution stays with the context provider, so a project
/// file cannot replace a server program.
///
/// Profiles are immutable static values; references to them stay valid for the
/// process lifetime and may be read from any thread.
/// How long a server may take to initialize or answer one request before the
/// client stops it, unless its profile says otherwise.
inline constexpr foundation::int32 LANGUAGE_SERVER_REQUEST_TIMEOUT_MILLISECONDS = 10000;

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
    /// `workspace/configuration` answers; never null.
    LanguageServerConfigurationResponder ConfigurationResponder = nullptr;
    /// The initialize and per-request deadline, in milliseconds.
    foundation::int32 RequestTimeoutMilliseconds = LANGUAGE_SERVER_REQUEST_TIMEOUT_MILLISECONDS;
};

/// The `slangd` profile from the selected Slang SDK.
[[nodiscard]] const LanguageServerProfile& GetSlangServerProfile();

/// The `luslc lsp` profile from the project SDK (LUSL sources).
[[nodiscard]] const LanguageServerProfile& GetLuslServerProfile();
} // namespace ludus::editor
