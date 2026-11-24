# Code editor

The editor opens every project text file as a **code tab** with a shared text
surface, atomic save, external-change handling, crash recovery and a code status
bar. Syntax highlighting and navigation are available from the same text
surface in Code tabs and the Scripts work area. Shader sources also get
[language services](#language-services) from a language server: hover,
diagnostics while you type, completion, rename and code actions.

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

From a graph, choose **Open Source** to open its selected node's shader
or its graph JSON in **Code**. The package file picker also opens the shader
manifest and other files in the same draft. **Shader API** is available for
shader programs, and **Build Graph** uses the package shown in Code.
Choose **Return to Graph** to restore the canvas and node you came from.

**Edit Declaration** and cross-file shader definitions use the same tabs.
Generated definitions open as read-only previews. **Navigate Back** returns to
the call site. For native external editing, use **Source → Open in Code
Editor**.

### Files that open read-only

A file opens read-only, with a banner that explains why, when it is not
well-formed UTF-8 or when it is past a size threshold. A very large file opens
read-only. Large editable files use visible-line coloring and
indentation-based folds. A file too large to open at all is refused with a message pointing at Open
Externally. Only UTF-8 is editable; other encodings are an explicit non-goal.

## Syntax and navigation

The editor recognizes Slang, LUSL, Luau, C, C++, JSON, CMake and Markdown by
file name. The language item in the code status bar overrides detection for
the active view.

- **Outline** opens a searchable list of declarations, JSON keys or Markdown
  headings. Type to filter and press Enter to visit the selected entry.
- The breadcrumb above the text opens the scopes containing the caret. When
  the first visible line is inside a scope that began above the viewport, the
  pinned scope header jumps to its start.
- **Expand Selection** and **Shrink Selection** are available from the source
  context menu. The defaults are Alt+Shift+Right and Alt+Shift+Left.
- The arrow in the line gutter collapses or expands a range. Click the
  placeholder to reveal the hidden lines. The source context menu also offers
  folding by level or kind, recursive folding, manual folds, and navigation
  between folds. The command palette lists the same actions while the editor
  has focus. Each split remembers its own folds. A restored tab reapplies
  folds whose opening line still matches.

Collapsed lines are part of the text, including when copied in a selection.
Going to a hidden line opens the folds that contain it. While the parser
processes an edit, the current line uses lexical coloring. The outline and
breadcrumbs become actionable when results for that revision are ready.

## Language services

Slang and LUSL sources in a render-graph package use a language server: the
pinned `slangd` for Slang and `luslc lsp` for LUSL. This covers package shaders
opened in **Code** and in the graph's Source pane. Ludus never runs a server
named by project settings. Luau and C/C++ servers arrive in later milestones.
The browser editor has no language server and reports these commands as
unavailable.

### Shader context

The **Shader context** row above a package shader chooses the shader program
whose compiler context the server analyzes. Opening a file starts nothing.
The context is prepared, and the server started, the first time you ask for
something: completion, a Go to command, Rename Symbol or Show Code Actions.
From then on, diagnostics, semantic colors, inlay hints and the light bulb
refresh by themselves about 100 ms after you stop typing.

The same row shows the server's state and any failure, with its repair. Editing,
saving and undo keep working whatever the server does:

- **Retry assistance** rebuilds the draft's compiler context and restarts a
  failed server.
- **Show Log** shows what the server wrote to its standard error since it last
  started.

A server that exits unexpectedly restarts after 1 s, then 2 s. A third exit
within a minute stops it until you choose Retry assistance.

A command the running server does not support stays in the menu, disabled,
with the reason in its tooltip. The pinned `slangd` and `luslc lsp` offer no
Rename Symbol, code actions or server folding ranges, and `slangd` offers no
Find References. Those commands work with any server that does offer them.

### Commands

In the table, Ctrl is Cmd on macOS. The source context menu lists every
command.

| Command | Default | What it does |
| --- | --- | --- |
| Completion | Ctrl+Space, or a trigger character such as `.` | Suggestions from the server, built-in snippets and words in open editors of the same language |
| Signature help | Ctrl+Shift+Space, or `(` and `,` | The parameters of the call at the caret |
| Show Hover | none (or rest the pointer) | The symbol's description and the diagnostics at that position |
| Go to Definition | F12 | Jumps to the definition |
| Go to Declaration, Go to Type Definition | none | Jump to the declaration or the type |
| Go to Implementation | Ctrl+F12 | Jumps to the implementation |
| Find References | Shift+F12 | Lists every use in the References window |
| Go to Symbol | Ctrl+Shift+O | Declarations in the current file |
| Go to Symbol in Workspace | Ctrl+T | Matching declarations in the package's sources |
| Rename Symbol | F2 | Renames the symbol at the caret in every file |
| Show Code Actions | Ctrl+. | Opens the fixes and refactorings for the caret or selection |
| Show Inlay Hints | none (a toggle) | Shows or hides parameter names and deduced types |

### Hover and diagnostics

Rest the pointer on a name for 300 ms, or choose **Show Hover**, to see what
the server knows about it. The hover also lists the build and analysis messages
at that position. A hover never starts the server: before a context is prepared
it shows only the build's messages.

Diagnostics from the server are **Analysis** results, kept apart from the
build's. They are drawn with a dotted underline beneath the build's wave
squiggles. The Error List shows them as rows with the origin
`Analysis (<server>)`; activating a row selects the range in its file. The list
does not come forward when analysis changes, so typing never loses focus. F8
and the gutter markers still step through build results.

### Colors and inlay hints

Semantic colors from the server are drawn over the syntax colors, so a
parameter, a constant and a field can look different even when the grammar
cannot tell them apart. Between replies, colors move with your edits instead of
flickering.

Inlay hints show parameter names and deduced types as gray text inside the
line. They are never part of the file: copying, saving and undo ignore them, and
the caret moves over the real characters only. **Show Inlay Hints** turns them
on or off, and the choice is remembered. While they are off, hold Ctrl+Alt to
show them briefly.

### Completion and snippets

The completion list combines four sources. They are ranked with the same fuzzy
matching as the command palette:

- the server's suggestions;
- built-in snippets: `if`, `for`, `struct`, `func` and a compute entry point, in
  the language's own syntax;
- words from open editors of the same language;
- after `.`, only the server's suggestions, since only the server knows a
  type's members.

Highlighting a suggestion shows its documentation in a panel beside the list.
Enter accepts it as one undoable edit. A snippet then selects its first tab
stop: Tab and Shift+Tab move between stops, and a stop that repeats is edited
in every place at once. Escape, or moving the caret out of the inserted text,
ends the snippet.

### References

**Find References** opens the **References** window. It is tabbed with the
Error List and Search, and also listed under **View** and **Window**. Rows are
grouped by file: the current source, other package files and read-only shader
context. After you edit the text, a row no longer jumps to a position that may
have moved. Instead it offers **Find Again**, which searches again from the
original symbol.

### Rename and code actions

**Rename Symbol** opens a box over the symbol. Enter renames it. Shift+Enter,
or a rename that reaches other files, first shows a preview of every file and
change, with Apply and Cancel.

When fixes exist on the caret's line, a light bulb appears in the gutter. Click
it, or press Ctrl+., to list them; preferred fixes are shown first, in bold.
Actions that would need the server to run its own command are listed disabled,
with the reason.

A rename or a code action is checked as a whole before anything changes. It is
refused, with nothing changed and its repair named, when it would:

- edit generated or read-only text;
- edit a file outside the package;
- apply to text that changed since the server answered;
- create, rename or delete files;
- contain overlapping edits.

An accepted change is a single undo step across every file it touches: one Undo
in any of those files reverts all of them. The files stay unsaved drafts until
you save.

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
- the **language-server state** item, which is still a placeholder. For a
  shader, the server's state, Retry assistance and Show Log are in the
  [Shader context](#shader-context) row.

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

The language services above cover shaders. These are not available yet:

- Luau scripts (M5) and C/C++ (M6) language servers;
- Ludus project symbols in completion, and the shader context picker in the
  status bar (M7);
- the debugger (M8).

The [architecture](../architecture/code-editor.md) and the
[task ledger](../../.kiro/specs/code-editor/tasks.md) track the full plan.
