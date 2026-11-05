// The editor keymap profiles and shortcut-collision checks (CE07/CE17 shortcut
// resolution, decided 2026-11-03). Both built-in profiles must be collision-free,
// the decided bindings must resolve as specified, and user overrides must take
// precedence and be detectable as conflicts. Offscreen; no window is built. The
// keymap reads QSettings, so each test clears the keymap keys first for
// determinism.

#include "internal/code_keymap.h"

#include <catch2/catch_test_macros.hpp>

#include <QKeySequence>
#include <QList>
#include <QSettings>
#include <QString>

#include <string>

using namespace ludus::editor;

namespace
{
// Remove the keymap's stored profile and overrides so a constructed CodeKeymap
// starts from a known, fresh state regardless of other tests or the real user.
void ClearKeymapSettings()
{
    QSettings settings;
    settings.remove(QStringLiteral("codeEditor/keymapProfile"));
    settings.remove(QStringLiteral("codeEditor/keymapOverrides"));
    settings.sync();
}

[[nodiscard]] QString ShortcutIn(const QList<CodeKeymapBinding>& bindings, const QString& commandId)
{
    for (const CodeKeymapBinding& binding : bindings)
    {
        if (binding.CommandId == commandId)
        {
            return binding.Shortcut;
        }
    }
    return QStringLiteral("<unbound>");
}

// Names each collision so a failing check says which bindings to fix.
[[nodiscard]] std::string DescribeConflicts(const QList<CodeKeymapConflict>& conflicts)
{
    QString text;
    for (const CodeKeymapConflict& conflict : conflicts)
    {
        text +=
            QStringLiteral("%1: %2 vs %3; ").arg(conflict.Shortcut, conflict.FirstCommandId, conflict.SecondCommandId);
    }
    return text.toStdString();
}
} // namespace

TEST_CASE("Both keymap profiles are free of shortcut collisions", "[editor][keymap]")
{
    ClearKeymapSettings();
    SECTION("Ludus profile")
    {
        CodeKeymap keymap;
        keymap.SetActiveProfile(CodeKeymapProfile::Ludus);
        const QList<CodeKeymapConflict> conflicts = keymap.DetectConflicts();
        INFO(DescribeConflicts(conflicts));
        CHECK(conflicts.isEmpty() == true);
    }
    SECTION("VS Code profile")
    {
        CodeKeymap keymap;
        keymap.SetActiveProfile(CodeKeymapProfile::VsCode);
        const QList<CodeKeymapConflict> conflicts = keymap.DetectConflicts();
        INFO(DescribeConflicts(conflicts));
        CHECK(conflicts.isEmpty() == true);
    }
    ClearKeymapSettings();
}

TEST_CASE("Every built-in shortcut is text Qt parses into keys", "[editor][keymap]")
{
    // QKeySequence separates chord strokes with ", "; "Ctrl+K Ctrl+0" parses to an
    // empty sequence, which never triggers and hides collisions from the check above.
    for (const CodeKeymapProfile profile : {CodeKeymapProfile::Ludus, CodeKeymapProfile::VsCode})
    {
        for (const CodeKeymapBinding& binding : CodeKeymap::GetProfileBindings(profile))
        {
            if (binding.Shortcut.isEmpty() == true)
            {
                continue;
            }
            INFO(binding.CommandId.toStdString() + " = " + binding.Shortcut.toStdString());
            const QKeySequence keys(binding.Shortcut);
            CHECK(keys.isEmpty() == false);
            CHECK(keys.toString(QKeySequence::PortableText) == binding.Shortcut);
        }
    }
}

TEST_CASE("The decided bindings resolve per the 2026-11-03 resolution", "[editor][keymap]")
{
    ClearKeymapSettings();
    // Record Profiling is Ctrl+Alt+F9 and the breakpoint keeps F9 in both profiles.
    const QList<CodeKeymapBinding> ludus = CodeKeymap::GetProfileBindings(CodeKeymapProfile::Ludus);
    const QList<CodeKeymapBinding> vscode = CodeKeymap::GetProfileBindings(CodeKeymapProfile::VsCode);
    CHECK(QKeySequence(ShortcutIn(ludus, QStringLiteral("profiling.record"))) ==
          QKeySequence(QStringLiteral("Ctrl+Alt+F9")));
    CHECK(QKeySequence(ShortcutIn(vscode, QStringLiteral("profiling.record"))) ==
          QKeySequence(QStringLiteral("Ctrl+Alt+F9")));
    CHECK(QKeySequence(ShortcutIn(ludus, QStringLiteral("editor.toggleBreakpoint"))) ==
          QKeySequence(QStringLiteral("F9")));
    // The command palette: Ctrl+K in Ludus, Ctrl+Shift+P in VS Code.
    CHECK(QKeySequence(ShortcutIn(ludus, QStringLiteral("tools.commandPalette"))) ==
          QKeySequence(QStringLiteral("Ctrl+K")));
    CHECK(QKeySequence(ShortcutIn(vscode, QStringLiteral("tools.commandPalette"))) ==
          QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    // The folding bindings are reserved in both profiles.
    CHECK(ShortcutIn(ludus, QStringLiteral("editor.foldAll")).isEmpty() == false);
    CHECK(ShortcutIn(vscode, QStringLiteral("editor.toggleFold")).isEmpty() == false);
    ClearKeymapSettings();
}

TEST_CASE("A fresh install has no stored profile and defaults to Ludus", "[editor][keymap]")
{
    ClearKeymapSettings();
    CodeKeymap keymap;
    CHECK(keymap.HasStoredProfile() == false);
    CHECK(keymap.GetActiveProfile() == CodeKeymapProfile::Ludus);
    keymap.SetActiveProfile(CodeKeymapProfile::VsCode);
    CHECK(keymap.HasStoredProfile() == true);
    // A second keymap reads the stored choice.
    CodeKeymap reopened;
    CHECK(reopened.HasStoredProfile() == true);
    CHECK(reopened.GetActiveProfile() == CodeKeymapProfile::VsCode);
    ClearKeymapSettings();
}

TEST_CASE("A user override takes precedence and is detectable as a conflict", "[editor][keymap]")
{
    ClearKeymapSettings();
    CodeKeymap keymap;
    keymap.SetActiveProfile(CodeKeymapProfile::Ludus);
    // Override Run Command in Context onto the palette key; the override wins and
    // now collides with the palette, which conflict detection must report.
    keymap.SetUserOverride(QStringLiteral("tools.runInContext"), QStringLiteral("Ctrl+K"));
    CHECK(QKeySequence(keymap.ResolveShortcut(QStringLiteral("tools.runInContext"))) ==
          QKeySequence(QStringLiteral("Ctrl+K")));
    const QList<CodeKeymapConflict> conflicts = keymap.DetectConflicts();
    CHECK(conflicts.isEmpty() == false);
    // Clearing the override restores the profile default (F3) and removes the clash.
    keymap.ClearUserOverride(QStringLiteral("tools.runInContext"));
    CHECK(QKeySequence(keymap.ResolveShortcut(QStringLiteral("tools.runInContext"))) ==
          QKeySequence(QStringLiteral("F3")));
    CHECK(keymap.DetectConflicts().isEmpty() == true);
    ClearKeymapSettings();
}
