#include "internal/script_code_document.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
// Append one byte through the seam, as the controller would for a keystroke.
[[nodiscard]] CodeEditStatus appendByte(ScriptCodeDocument& inoutHost, char value)
{
    const CodeDocumentSnapshot snapshot = inoutHost.GetSnapshot();
    const usize size = snapshot.Text.GetSize();
    CodeTextEdit edit;
    edit.StartBytes = size;
    edit.RemovedBytes = 0;
    edit.Inserted = {&value, 1};
    return inoutHost.ApplyEdit(snapshot.Identity, edit, {size, size}, {size + 1, size + 1}, false);
}
} // namespace

TEST_CASE("Script host tracks the unsaved draft against its baseline", "[code-editor][scripts][script-host]")
{
    ScriptCodeDocument host;
    host.SetBaseline(QByteArray("return 1\n"));
    CHECK(host.IsDirty() == false);
    CHECK(host.GetDraft() == QByteArray("return 1\n"));

    REQUIRE(appendByte(host, 'x') == CodeEditStatus::Ok);
    CHECK(host.IsDirty() == true);
    CHECK(host.CanUndo() == true);

    // Undo returns to the clean baseline, like the S5 source Undo button.
    REQUIRE(host.Undo() == true);
    CHECK(host.IsDirty() == false);
    CHECK(host.CanRedo() == true);
    REQUIRE(host.Redo() == true);
    CHECK(host.IsDirty() == true);
}

TEST_CASE("Script host re-baselines on save and reload", "[code-editor][scripts][script-host]")
{
    ScriptCodeDocument host;
    host.SetBaseline(QByteArray("alpha\n"));
    REQUIRE(appendByte(host, '!') == CodeEditStatus::Ok);
    CHECK(host.IsDirty() == true);

    // Saving makes the current draft the new clean baseline.
    host.SetBaseline(host.GetDraft());
    CHECK(host.IsDirty() == false);
    CHECK(host.CanUndo() == false);

    // Reloading replaces the draft and baseline together.
    host.SetBaseline(QByteArray("reloaded\n"));
    CHECK(host.GetDraft() == QByteArray("reloaded\n"));
    CHECK(host.IsDirty() == false);
}

TEST_CASE("Script host rejects a stale identity without mutating", "[code-editor][scripts][script-host]")
{
    ScriptCodeDocument host;
    host.SetBaseline(QByteArray("abc\n"));
    CodeDocumentIdentity stale = host.GetSnapshot().Identity;
    REQUIRE(appendByte(host, 'z') == CodeEditStatus::Ok);
    // The pre-edit identity is now stale; a second edit against it is a conflict.
    CodeTextEdit edit;
    const char value = 'q';
    edit.StartBytes = 0;
    edit.RemovedBytes = 0;
    edit.Inserted = {&value, 1};
    CHECK(host.ApplyEdit(stale, edit, {0, 0}, {1, 1}, false) == CodeEditStatus::Conflict);
}
