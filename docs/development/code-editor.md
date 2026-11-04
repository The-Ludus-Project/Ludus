# Code editor

The editor opens every project text file as a **code tab** with a shared text
surface, atomic save, external-change handling, crash recovery and a code status
bar. This guide covers the behavior delivered in milestone M1; syntax
highlighting, multiple cursors, the find overlay and language servers arrive in
later milestones and are noted where they apply.

This page is a user guide for the code editor inside the native
[editor workspace](editor-workspace.md). The authoritative contract is the
specification package `.kiro/specs/code-editor/{requirements,design,tasks}.md`
and the [code editor architecture](../architecture/code-editor.md); milestone
decisions are recorded in the
[decision log](../architecture/code-editor-decision-log.md).

## Opening files

Open a file from Project Explorer, Quick Open, the Error List or any navigation
command. A single click opens a **preview tab**: it is reused for the next
single-click open in the same group, so browsing files does not fill the tab
bar. Editing the file, saving it, or double-clicking its tab makes the tab
**permanent**, and a permanent tab is never replaced by a preview.

A file can be open in two editor groups side by side. Both views share one
buffer and one undo history, so an edit in one group appears in the other at
once; each view keeps its own caret, selection, scroll position and wrap.

Files owned by a render-graph package open in that package's **Source** pane
rather than a standalone tab. **Open Externally** remains available for files the
editor opens read-only or cannot open.

### Files that open read-only

A file opens read-only, with a banner that explains why, when it is not
well-formed UTF-8 or when it is past a size threshold. A very large file opens
read-only; a large file opens editable but with syntax and language features
off. A file too large to open at all is refused with a message pointing at Open
Externally. Only UTF-8 is editable; other encodings are an explicit non-goal.

## Editing and saving

The code surface records its own undo history for a bound file. Undo and Redo,
Save, and the editing commands shared with the rest of the editor all work on a
code tab.

Save uses an atomic replace: the new bytes are written to a temporary file and
swapped in, so an interrupted save never leaves a half-written file. A save that
fails keeps the buffer dirty and reports the cause; your text is never lost. The
file's byte-order mark is re-emitted when the original had one, and a final
newline is added when the file's `.editorconfig` asks for it.

`.editorconfig` is resolved for each file on a background thread and sets the
indentation and line endings for new lines (`indent_style`, `indent_size`,
`tab_width`, `end_of_line`, `charset`, `insert_final_newline`,
`trim_trailing_whitespace`). Existing line endings are preserved byte for byte;
only newly typed lines follow the file's dominant ending.

### External changes

If a file changes on disk while it is open, the editor compares its size,
modification time and a content hash before declaring a conflict, so the
editor's own save never counts as one. A **clean** buffer is reloaded from disk.
A **dirty** buffer is never overwritten: the conflict offers **Compare**, **Keep
Mine** and **Reload**. Keep Mine re-anchors your draft against the new on-disk
content so it stays dirty and can be saved over the external change.

## Crash recovery

Unsaved code buffers are journaled for crash recovery. The journal lives in this
computer's local application-data directory, scoped to the project, and is
written on an idle pause after you stop typing, on a background thread, so it
never blocks the editor. Each unsaved buffer records the file path, the hash of
the on-disk baseline it was edited over, and the draft bytes; the journal never
writes into the project tree.

When you reopen the project after a crash or forced quit, each unsaved buffer is
reopened and its draft is restored on top of the file's current saved baseline,
so the recovered tab is dirty and ready to save. A buffer whose file was deleted,
or whose file changed on disk since the crash, is skipped rather than restored
over different content. After a clean close there is nothing to recover: saving a
buffer clears its journal entry, and discarding unsaved buffers clears them too.

The browser editor cannot write a machine-local journal; download or export your
work before closing or reloading the tab.

## Closing a dirty tab

Closing a tab with unsaved changes asks **Save**, **Don't Save** or **Cancel**,
with **Cancel** as the default so a stray keypress keeps your draft. Save writes
the buffer and closes the tab; Don't Save closes it and discards the draft;
Cancel leaves the tab open.

Closing the project or quitting the editor lists every unsaved code buffer and
asks **Save All**, **Don't Save** or **Cancel**, again defaulting to Cancel. A
save that fails keeps those buffers open and reports which ones.

## Status bar

When a code view has focus the status bar shows, for the active view:

- the **cursor position** as line and column;
- the **selection** size when text is selected;
- the **indentation** (spaces or tab size);
- the **encoding** (UTF-8);
- the **line endings** (LF or CRLF);
- the **language** mode;
- the **language-server state**, a placeholder until the language client arrives
  in a later milestone.

Items you can change in this milestone are clickable: the position opens **Go to
Line**, the language opens a picker that sets the view's mode, and the line
endings open an LF/CRLF choice. The encoding and indentation are shown for
information.

## Scripts

The Scripts work area edits its project script and sequence drafts on the same
code surface, keeping its draft, Save, reload and source-stop behavior. Saving a
script re-baselines its draft and keeps the undo history, so Undo after Save
returns to the unsaved text; opening or reloading another script replaces the
draft and clears history, so Undo never reaches a different script's text.

## What arrives later

Syntax highlighting and folding (M3), multiple cursors and the find/replace
overlay (M2), and hover, completion, diagnostics and the other language-server
features (M4 onward) are not part of this milestone. The
[architecture](../architecture/code-editor.md) and the
[task ledger](../../.kiro/specs/code-editor/tasks.md) track the full plan.
