# Code editor

The editor opens every project text file as a **code tab** with a shared text
surface, atomic save, external-change handling, crash recovery and a code status
bar. Syntax highlighting and navigation are available from the same text
surface in code tabs, the render-graph Source pane and the Scripts work area.

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
read-only. Large editable files use visible-line coloring and
indentation-based folds. A file too large to open at all is refused with a message pointing at Open
Externally. Only UTF-8 is editable; other encodings are an explicit non-goal.

## Syntax and navigation

The editor recognizes Slang, Luau, C, C++, JSON, CMake and Markdown by file
name. The language item in the code status bar overrides detection for the
active view. LUSL uses the compiler's lexical profile and indentation folds.

- **Outline** opens a searchable list of declarations, JSON keys or Markdown
  headings. Type to filter and press Enter to visit the selected entry.
- The breadcrumb above the text opens the scopes containing the caret. When
  the first visible line is inside a scope that began above the viewport, the
  pinned scope header jumps to its start.
- **Expand Selection** and **Shrink Selection** are available from the source
  context menu. The defaults are Alt+Shift+Right and Alt+Shift+Left.
- The arrow in the line gutter collapses or expands a range. Click the
  placeholder to reveal the hidden lines. The source context menu also offers
  **Fold Selection**, **Fold All**, and **Unfold All**. Each split remembers
  its own folds. A restored tab reapplies folds whose opening line still
  matches.

Collapsed lines are part of the text, including when copied in a selection.
Going to a hidden line opens the folds that contain it. While the parser
processes an edit, the current line uses lexical coloring. The outline and
breadcrumbs become actionable when results for that revision are ready.

## Editing and saving

The code surface records its own undo history for a bound file. Undo and Redo,
Save (**File > Save File**, Ctrl+S or Cmd+S), and the editing commands shared
with the rest of the editor all work on a code tab.

Save uses an atomic replace: the new bytes are written to a temporary file and
swapped in, so an interrupted save never leaves a half-written file. A save that
fails keeps the buffer dirty and explains the cause and the next step in the
tab's bar; your text is never lost. The
file's byte-order mark is re-emitted when the original had one, and a final
newline is added when the file's `.editorconfig` asks for it.

`.editorconfig` is resolved for each file on a background thread and sets the
indentation and line endings for new lines (`indent_style`, `indent_size`,
`tab_width`, `end_of_line`, `charset`, `insert_final_newline`,
`trim_trailing_whitespace`). Existing line endings are preserved byte for byte;
only newly typed lines follow the file's dominant ending.

### External changes

The editor watches the files behind open tabs, and checks them again when you
switch tabs or return to the editor. Before declaring a change it compares the
file's size, modification time and a content hash, so the editor's own save
never counts as one. A **clean** buffer is reloaded from disk.

A **dirty** buffer is never overwritten. The tab's bar offers:

- **Compare** shows your unsaved text beside the file on disk, read-only.
- **Keep Mine** keeps your draft; the next Save writes it over the change.
- **Reload** discards your draft and loads the file.

Save refuses to write over an undecided change and shows the bar instead. If the
file was deleted, the buffer stays open and Save writes it back. The browser
editor has no file watcher, so it checks only on tab and window activation.

## Crash recovery

Unsaved code buffers are journaled for crash recovery. The journal lives in this
computer's local application-data directory, scoped to the project, and is
written on an idle pause after you stop typing, on a background thread, so it
never blocks the editor. Each unsaved buffer records the file path, the hash of
the on-disk baseline it was edited over, and the draft bytes; the journal never
writes into the project tree.

When you reopen the project after a crash or forced quit, each unsaved buffer is
reopened and its draft is restored on top of the file's current saved baseline,
so the recovered tab is dirty and ready to save. The journal is rewritten right
after the restore, so a second crash still recovers the same drafts.

If the file changed on disk after the draft was journaled (for example a pull
before you reopened), the draft is still restored and the bar offers **Compare**,
**Keep Mine** and **Reload**, so you choose between your work and the file. A
buffer whose file was deleted is not reopened.

After a clean close there is nothing to recover: saving a buffer removes its
journal entry, and **Don't Save** discards the drafts in memory and in the
journal, so they never come back on the next open.

The browser editor cannot write a machine-local journal; download or export your
work before closing or reloading the tab.

## Closing a dirty tab

Closing a tab with unsaved changes asks **Save**, **Don't Save** or **Cancel**,
with **Cancel** as the default so a stray keypress keeps your draft. Save writes
the buffer and closes the tab; Don't Save closes it and discards the draft;
Cancel leaves the tab open. When the same file is also open in the other editor
group, closing one of its views does not ask: the other view keeps the draft.

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
