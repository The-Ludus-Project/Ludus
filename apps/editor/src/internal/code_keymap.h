#pragma once

#include <ludus/foundation/base/types.h>

#include <QHash>
#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// The two built-in keymap profiles (design "Settings, keymaps and
/// EditorConfig", decided 2026-11-03). Ludus is the house layout; VS Code mirrors
/// Visual Studio Code's defaults, freeing Ctrl+K for chords and opening the
/// command palette with Ctrl+Shift+P.
enum class CodeKeymapProfile : foundation::uint8
{
    Ludus,
    VsCode,
};

/// One command's portable key binding in one profile: the command id (the
/// QAction objectName) and its QKeySequence text. An empty Shortcut reserves the
/// id with no binding, which still participates in conflict detection so a later
/// milestone cannot silently collide with it.
struct CodeKeymapBinding final
{
    QString CommandId;
    QString Shortcut;
};

/// One detected collision: two command ids bound to the same key in the resolved
/// keymap. Both ids are reported so the first-use choice and the settings UI can
/// name them.
struct CodeKeymapConflict final
{
    QString Shortcut;
    QString FirstCommandId;
    QString SecondCommandId;
};

/// The editor keymap (CE07/CE17 shortcut resolution). It owns the per-profile
/// binding tables, the user's overrides and the active profile, resolves the
/// effective binding for each command, detects collisions, and applies the
/// resolved shortcuts onto the shell's QActions by object name. It is Qt-light
/// (QString/QHash only) and has no widget state of its own, so it is unit
/// testable without a window. GUI thread only for Apply.
class CodeKeymap final
{
public:
    /// Construct with the built-in bindings for both profiles and the stored user
    /// overrides and active profile (QSettings). A fresh install has no stored
    /// profile; HasStoredProfile is then false so the shell offers the first-use
    /// choice, and the active profile is the default (Ludus) until chosen.
    CodeKeymap();

    [[nodiscard]] CodeKeymapProfile GetActiveProfile() const noexcept
    {
        return mActiveProfile;
    }
    /// Whether a profile was ever chosen and stored; false on a fresh install, so
    /// the shell shows the first-use choice once.
    [[nodiscard]] bool HasStoredProfile() const noexcept
    {
        return mHasStoredProfile;
    }
    /// Set and persist the active profile (the first-use choice or a settings
    /// change). Marks the profile as stored.
    void SetActiveProfile(CodeKeymapProfile profile);

    /// Override one command's binding for the active profile and persist it, or
    /// clear the override with an empty shortcut so the profile default returns.
    void SetUserOverride(const QString& commandId, const QString& shortcut);
    void ClearUserOverride(const QString& commandId);

    /// The effective shortcut for a command in the active profile: the user
    /// override when present, else the profile binding, else the empty string.
    [[nodiscard]] QString ResolveShortcut(const QString& commandId) const;

    /// Every resolved (command id, shortcut) binding for the active profile,
    /// with overrides applied. Used to apply the keymap and to detect conflicts.
    [[nodiscard]] QList<CodeKeymapBinding> ResolveActiveBindings() const;

    /// Every pair of commands that resolve to the same non-empty shortcut in the
    /// active profile. Empty when the keymap is collision-free, which the
    /// shortcut-collision tests require for both built-in profiles.
    [[nodiscard]] QList<CodeKeymapConflict> DetectConflicts() const;

    /// Apply the resolved shortcuts to the shell's QActions found under `host` by
    /// object name (the command id). A command id with no QAction under `host` is
    /// skipped; a resolved empty shortcut clears the action's shortcut. Call after
    /// the actions are created and whenever the profile or an override changes.
    void ApplyToActions(QWidget* inoutHost) const;

    /// The built-in binding table for a profile, for tests and the settings UI.
    [[nodiscard]] static QList<CodeKeymapBinding> GetProfileBindings(CodeKeymapProfile profile);

private:
    [[nodiscard]] static QString settingsKeyForProfile();
    [[nodiscard]] static QString overrideSettingsGroup(CodeKeymapProfile profile);
    void loadOverrides();

    CodeKeymapProfile mActiveProfile = CodeKeymapProfile::Ludus;
    bool mHasStoredProfile = false;
    // The active profile's user overrides: command id to shortcut (empty clears).
    QHash<QString, QString> mOverrides;
};
} // namespace ludus::editor
