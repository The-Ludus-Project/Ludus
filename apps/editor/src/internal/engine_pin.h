#pragma once

// Engine pin status and engine install validation.
//
// Private editor header (not installed). A version-2 project builds against a
// machine-local engine install recorded in ignored `.ludus/local.json`. The
// trusted tooling backend (scripts/python/ludus_tools/engine_pin.py) compares
// that install's manifest revision with the editor's checkout and reports the
// result through the `engine_pin` protocol event. This header holds the parsed
// status and the folder checks the Repair dialog runs before it accepts a path,
// so nobody needs to hand-edit `.ludus/local.json`.

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QStringList>

#include <optional>

namespace ludus::editor
{
/// How the project's selected engine install relates to this editor's engine.
enum class EnginePinState : foundation::uint8
{
    None,    ///< Not checked yet, or the project selects no local install.
    Current, ///< Built from the same revision as this editor's checkout.
    Stale,   ///< Built from another revision; games build against different engine code.
    Invalid, ///< The selection is not a usable install (for example a build folder).
    Unknown, ///< A revision is missing, so the install cannot be compared.
};

/// Parsed `engine_pin` report for the open project.
struct EnginePinStatus
{
    EnginePinState State = EnginePinState::None;
    QString Installation;       ///< Selected install folder as recorded by the project.
    QString PinnedRevision;     ///< Revision recorded in the install's manifest; may be empty.
    QString EditorRevision;     ///< Revision of this editor's tooling checkout; may be empty.
    QString Message;            ///< Human explanation including the available action.
    bool IsEditorOwned = false; ///< True when the install lives below this checkout's out/install.
};

/// Map a protocol state string to its enum value.
/// @return The state, or no value for an unrecognized string (a protocol error).
[[nodiscard]] std::optional<EnginePinState> ParseEnginePinState(const QString& text) noexcept;

/// True when the editor should offer "Use this editor's engine": the selected
/// install is stale or unusable. Current and unknown states need no action.
[[nodiscard]] bool ShouldOfferEditorEngine(const EnginePinStatus& status) noexcept;

/// Map a native profile such as `macos-clang-development` to its SDK build flavor.
/// @return `Debug`, `Development` or `Release`; empty for an unrecognized profile.
[[nodiscard]] QString GetFlavorForProfile(const QString& profile);

/// Result of checking a folder the user offered as an engine install.
struct EngineInstallCheck
{
    bool IsInstall = false; ///< True when the folder holds an installed engine of the expected flavor.
    QString Flavor;         ///< Flavor recorded in the install's manifest, when readable.
    QString Revision;       ///< Revision recorded in the install's manifest, when readable.
    QString Message;        ///< Why the folder was rejected, or a short identity summary.
};

/// Check that `folder` is an installed Ludus engine before repair records it.
///
/// Reads only `share/Ludus/LudusSdkManifest.json` (bounded) and looks for CMake
/// build-tree markers, so a build folder is rejected with a message that names
/// the mistake. The tooling backend repeats the full ABI/version validation.
/// @param folder Absolute folder path chosen by the user.
/// @param expectedFlavor Required build flavor, or empty to accept any flavor.
[[nodiscard]] EngineInstallCheck CheckEngineInstallFolder(const QString& folder, const QString& expectedFlavor);

/// List installs below `<toolingRoot>/out/install` whose manifest matches `expectedFlavor`.
/// @return Absolute install folders sorted by name; empty when none exist.
[[nodiscard]] QStringList DiscoverEngineInstalls(const QString& toolingRoot, const QString& expectedFlavor);
} // namespace ludus::editor
