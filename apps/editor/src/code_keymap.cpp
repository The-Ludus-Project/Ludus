#include "internal/code_keymap.h"

#include <ludus/foundation/base/core.h>

#include <QAction>
#include <QKeySequence>
#include <QSettings>
#include <QWidget>

// The editor keymap and its two built-in profiles (shortcut resolution decided
// 2026-11-03; see the code-editor decision log). The binding tables encode that
// resolution: Record Profiling is Ctrl+Alt+F9 in both profiles and every focus
// context, F9 toggles a breakpoint in a code editor, the Ludus profile keeps
// Ctrl+K for the command palette, and the VS Code profile opens the palette with
// Ctrl+Shift+P and frees Ctrl+K for chords. The folding bindings are reserved in
// both profiles so the shortcut-collision tests cover them before M3 adds the
// fold commands.
namespace ludus::editor
{
namespace
{
using foundation::int32;

constexpr char PROFILE_SETTINGS_KEY[] = "codeEditor/keymapProfile";
constexpr char LUDUS_PROFILE_VALUE[] = "ludus";
constexpr char VSCODE_PROFILE_VALUE[] = "vscode";

// The shared bindings both profiles agree on. Each is a command id and its
// portable QKeySequence text. These are the editor/shell commands the keymap
// governs and checks for collisions; commands not listed here keep their
// descriptor default and are out of the keymap's conflict scope for now.
[[nodiscard]] QList<CodeKeymapBinding> SharedBindings()
{
    return {
        // Record Profiling moves off F9 to Ctrl+Alt+F9 in every profile so F9 is
        // free to toggle a breakpoint in a code editor.
        CodeKeymapBinding{ .CommandId = QStringLiteral("profiling.record"), .Shortcut = QStringLiteral("Ctrl+Alt+F9") },
        // F9 toggles a breakpoint when focus is in a code editor (its gutter
        // action is wired in M8); elsewhere F9 is unassigned.
        CodeKeymapBinding{ .CommandId = QStringLiteral("editor.toggleBreakpoint"), .Shortcut = QStringLiteral("F9") },
        CodeKeymapBinding{ .CommandId = QStringLiteral("tools.runInContext"), .Shortcut = QStringLiteral("F3") },
    };
}
} // namespace

CodeKeymap::CodeKeymap()
{
    QSettings settings;
    const QString stored = settings.value(QString::fromUtf8(PROFILE_SETTINGS_KEY)).toString();
    if (stored == QString::fromUtf8(VSCODE_PROFILE_VALUE))
    {
        mActiveProfile = CodeKeymapProfile::VsCode;
        mHasStoredProfile = true;
    }
    else if (stored == QString::fromUtf8(LUDUS_PROFILE_VALUE))
    {
        mActiveProfile = CodeKeymapProfile::Ludus;
        mHasStoredProfile = true;
    }
    else
    {
        // A fresh install: default to Ludus until the first-use choice is made.
        mActiveProfile = CodeKeymapProfile::Ludus;
        mHasStoredProfile = false;
    }
    loadOverrides();
}

QString CodeKeymap::settingsKeyForProfile()
{
    return QString::fromUtf8(PROFILE_SETTINGS_KEY);
}

QString CodeKeymap::overrideSettingsGroup(CodeKeymapProfile profile)
{
    return QStringLiteral("codeEditor/keymapOverrides/%1")
        .arg(profile == CodeKeymapProfile::VsCode ? QString::fromUtf8(VSCODE_PROFILE_VALUE)
                                                  : QString::fromUtf8(LUDUS_PROFILE_VALUE));
}

void CodeKeymap::loadOverrides()
{
    mOverrides.clear();
    QSettings settings;
    settings.beginGroup(overrideSettingsGroup(mActiveProfile));
    for (const QString& key : settings.childKeys())
    {
        mOverrides.insert(key, settings.value(key).toString());
    }
    settings.endGroup();
}

void CodeKeymap::SetActiveProfile(CodeKeymapProfile profile)
{
    mActiveProfile = profile;
    mHasStoredProfile = true;
    QSettings settings;
    settings.setValue(settingsKeyForProfile(),
                      profile == CodeKeymapProfile::VsCode ? QString::fromUtf8(VSCODE_PROFILE_VALUE)
                                                           : QString::fromUtf8(LUDUS_PROFILE_VALUE));
    loadOverrides();
}

void CodeKeymap::SetUserOverride(const QString& commandId, const QString& shortcut)
{
    if (shortcut.isEmpty() == true)
    {
        ClearUserOverride(commandId);
        return;
    }
    mOverrides.insert(commandId, shortcut);
    QSettings settings;
    settings.beginGroup(overrideSettingsGroup(mActiveProfile));
    settings.setValue(commandId, shortcut);
    settings.endGroup();
}

void CodeKeymap::ClearUserOverride(const QString& commandId)
{
    mOverrides.remove(commandId);
    QSettings settings;
    settings.beginGroup(overrideSettingsGroup(mActiveProfile));
    settings.remove(commandId);
    settings.endGroup();
}

QList<CodeKeymapBinding> CodeKeymap::GetProfileBindings(CodeKeymapProfile profile)
{
    QList<CodeKeymapBinding> bindings = SharedBindings();
    if (profile == CodeKeymapProfile::VsCode)
    {
        // VS Code opens the palette with Ctrl+Shift+P and leaves Ctrl+K free, so
        // the folding commands use VS Code's own Ctrl+K chords.
        bindings.append(CodeKeymapBinding{ .CommandId = QStringLiteral("tools.commandPalette"),
                                           .Shortcut = QStringLiteral("Ctrl+Shift+P") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.foldAll"), .Shortcut = QStringLiteral("Ctrl+K Ctrl+0") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.unfoldAll"), .Shortcut = QStringLiteral("Ctrl+K Ctrl+J") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.toggleFold"), .Shortcut = QStringLiteral("Ctrl+Shift+[") });
    }
    else
    {
        // The Ludus profile keeps Ctrl+K for the command palette, so Ctrl+K is not
        // free to prefix a chord; the folding commands use single Ctrl+Shift keys
        // instead, which do not collide with the palette or each other.
        bindings.append(CodeKeymapBinding{ .CommandId = QStringLiteral("tools.commandPalette"),
                                           .Shortcut = QStringLiteral("Ctrl+K") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.foldAll"), .Shortcut = QStringLiteral("Ctrl+Shift+-") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.unfoldAll"), .Shortcut = QStringLiteral("Ctrl+Shift+=") });
        bindings.append(
            CodeKeymapBinding{ .CommandId = QStringLiteral("editor.toggleFold"), .Shortcut = QStringLiteral("Ctrl+Shift+[") });
    }
    return bindings;
}

QString CodeKeymap::ResolveShortcut(const QString& commandId) const
{
    const QHash<QString, QString>::const_iterator override = mOverrides.constFind(commandId);
    if (override != mOverrides.constEnd())
    {
        return override.value();
    }
    for (const CodeKeymapBinding& binding : GetProfileBindings(mActiveProfile))
    {
        if (binding.CommandId == commandId)
        {
            return binding.Shortcut;
        }
    }
    return {};
}

QList<CodeKeymapBinding> CodeKeymap::ResolveActiveBindings() const
{
    QList<CodeKeymapBinding> resolved = GetProfileBindings(mActiveProfile);
    for (CodeKeymapBinding& binding : resolved)
    {
        const QHash<QString, QString>::const_iterator override = mOverrides.constFind(binding.CommandId);
        if (override != mOverrides.constEnd())
        {
            binding.Shortcut = override.value();
        }
    }
    return resolved;
}

QList<CodeKeymapConflict> CodeKeymap::DetectConflicts() const
{
    QList<CodeKeymapConflict> conflicts;
    const QList<CodeKeymapBinding> bindings = ResolveActiveBindings();
    for (int32 first = 0; first < bindings.size(); ++first)
    {
        if (bindings.at(first).Shortcut.isEmpty() == true)
        {
            continue;
        }
        // Compare canonical key sequences so "Ctrl+K" and "ctrl+k" collide.
        const QKeySequence firstKey(bindings.at(first).Shortcut);
        for (int32 second = first + 1; second < bindings.size(); ++second)
        {
            if (bindings.at(second).Shortcut.isEmpty() == true)
            {
                continue;
            }
            const QKeySequence secondKey(bindings.at(second).Shortcut);
            // Two bindings collide when they are equal, or when one is a single
            // key that is the opening key of the other's chord: a complete
            // Ctrl+K binding would swallow the first stroke of a Ctrl+K chord, so
            // they cannot both live in one profile.
            bool isConflict = firstKey == secondKey;
            if (isConflict == false && firstKey.count() == 1 && secondKey.count() > 1)
            {
                isConflict = firstKey[0] == secondKey[0];
            }
            if (isConflict == false && secondKey.count() == 1 && firstKey.count() > 1)
            {
                isConflict = secondKey[0] == firstKey[0];
            }
            if (isConflict == true)
            {
                conflicts.append(CodeKeymapConflict{ .Shortcut = bindings.at(first).Shortcut,
                                                     .FirstCommandId = bindings.at(first).CommandId,
                                                     .SecondCommandId = bindings.at(second).CommandId });
            }
        }
    }
    return conflicts;
}

void CodeKeymap::ApplyToActions(QWidget* inoutHost) const
{
    LUDUS_ASSERT(inoutHost != nullptr);
    for (const CodeKeymapBinding& binding : ResolveActiveBindings())
    {
        QAction* action = inoutHost->findChild<QAction*>(binding.CommandId);
        if (action == nullptr)
        {
            continue;
        }
        action->setShortcut(QKeySequence(binding.Shortcut));
    }
}
} // namespace ludus::editor
