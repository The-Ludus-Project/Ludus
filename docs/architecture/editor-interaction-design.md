# Ludus editor interaction design

Status: target updated 2026-10-06. The current S1 shell and Qt/Wasm preview exist;
the compact layout and full portable document transactions below are proposed.
The [initial S2 interactions](../development/editor-workspace.md#initial-s2-document-interactions)
implement bounded project history, focus preservation and scoped Save/Undo.
[GUI systems](editor-gui-systems.md) owns the engineering contracts; this document
owns their user interaction. Existing functionality remains documented in the
[workspace guide](../development/editor-workspace.md).

## Default workspace

```text
Native: OS decoration + platform menu conventions
Browser: app container, without simulated OS title bar

Project / document / save state       Save   useful context actions   Search
+--------------------------------------------------+-------------------+
| Active document tabs                             | Inspector         |
|                                                  | when applicable   |
| Project settings / Configuration / Audio / World  |                   |
|                                                  | Document or       |
| Current authoring work has first claim on space   | session scope     |
+--------------------------------------------------+-------------------+
| Optional Output / Jobs: opened for useful details, otherwise collapsed|
+----------------------------------------------------------------------+
| Draft / operation / capability status                                |
```

The drawing describes roles, not fixed sizes or a promise of a particular viewport layout.
Menus retain common commands, View tool windows and Window → Reset Window Layout. The context strip presents
frequent actions for the current task; native-only actions do not consume a
permanently disabled browser row. Advanced commands remain discoverable through
stable menus and command search. Context menus and shortcuts provide faster
access without being the only way to find an action.

Native windows retain OS decoration and macOS menu conventions. Browser windows
use the website/application container without a second imitation title bar.
Qt fonts, logical pixels and layout managers govern control sizing. A narrow
container switches secondary panels to tabs/drawers, preserving the complete
form through scrolling instead of clipping it. Show an Inspector only for an
applicable selection; collapse empty diagnostics initially. A job/error indicator
can reveal details without taking keyboard focus away from an edit.

Desktop panels may move, float and hide. View restores each named panel; Reset
Layout restores access without discarding drafts, stopping Play or changing
project data. Local preferences are versioned and bounded; corrupt/incompatible
layout recovers to defaults. Exact app/Qt compatibility is checked before opaque
Qt layout restoration. Do not load layout blobs from project files. A preferences
write failure leaves document saves independent. Browser multiwindow behavior
requires its own acceptance; floating native docks are not assumed there.

## Existing project workflow

Creation asks for Name and a parent Location with Browse, previews the new folder,
and uses Create Project. The native shared backend selects/prepares the associated
engine and verifies staged configure/build/tests. An explicit Advanced override
never falls back silently on failure. Opening checks setup without building or
repairing it. Browser import/download is a separate capability; it does not
create a working native project or run the compiler.

Native Close Project uses the existing settings/audio Save/Discard/Cancel and
quiescence policy; owned work stops before close. Configuration has an independent
offline draft that survives a project change. Quit considers its unsaved state.
The current F1 shortcut list and shared action objects remain useful. Current
Ctrl+S now follows the active Project Settings, Audio or Configuration work area.
The full command catalogue and portable transaction model below remain pending;
the workspace guide owns the implemented behavior and limits.

## Proposed command and edit behavior

- One command catalogue supplies menu, strip, shortcut, search and automation
  intent. Validate current capability/target at dispatch. Unavailable commands
  explain a useful reason; hiding a strip item does not invent support.
- Focus routes text editing first, then committed document operations, then
  workspace commands. Platform Save/Open/Undo conventions use Ctrl or Command
  as appropriate. Viewport manipulation shortcuts never capture typing or IME.
- A field may contain an incomplete/invalid buffer. Show validation near it and
  preserve it while jobs or session snapshots arrive. Save validates pending
  buffers in scope; failure keeps them available. No heartbeat rebuilds a focused
  form or changes selection/scroll. Close counts pending edits as unsaved work.
- Drag/scrub begins a transient edit, previews it, and commits one Undo item on
  acceptance. Escape or lost capture cancels it. Multi-selection displays mixed
  values; focus alone changes nothing. Failed batch preparation changes nothing.
- Undo restores committed document content; text Undo first edits the focused
  temporary buffer. Undo back to saved content can be clean at a newer revision.
  Saving during another edit acknowledges only the captured snapshot.
- Labels name data, tooltips explain consequences, and status reports state.
  Document draft, live session and imported artifact scopes remain visible.
  Applying live values and copying them to a draft are distinct operations.
- Keep common actions visibly available; reduce cursor travel with contextual
  tools and stable ordering. Allow user customization without dynamically moving
  commands based on usage. Advanced options may be disclosed without hiding the
  next step of the main task.
- Ordinary validation and progress remain in the workspace. Use Save/Discard/
  Cancel for loss of work and explicit confirmations for irreversible actions;
  frequent reversible edits rely on Undo. Cancellation says requested until the
  operation acknowledges or otherwise finishes.
- Form labels, panels and custom canvases expose names/roles and keyboard paths.
  Focus indication, errors, mixed state and dirty state use more than color.
  Shared semantic controls support light/dark, density, scale and contrast;
  a decorative theme cannot replace text and accessibility behavior.

## Representative tasks and failure behavior

| Task | Expected interaction | Failure behavior |
| --- | --- | --- |
| Open native recent project | Select/open, or double-click | Existing path/setup diagnostics; no automatic repair/download |
| Edit project/document | Active task, edit, Save | Invalid buffer or save conflict preserves the draft; Build uses documented saved state |
| Browser round trip | Import, edit, download, reopen | Malformed/oversized admission retains current content; volatile storage/export limits stay explicit |
| Tune Play | Start, pause, inspect, apply to session | Stale revisions rejected; failed game process cannot overwrite source |
| Author audio | Select/create, edit, save, preview | Existing discard policy; native preview stops before game launch |
| Recover workspace | Window tool-window toggle or Reset Window Layout | Corrupt/incompatible preferences use useful defaults |
| Future asset import | Source, progress/result, optional cancellation | Last valid artifact retained; stale job cannot publish; actionable retry |
| Future scene transform | Select, preview, accept, Undo | Cancel/invalid target safely drops preview; resized-scene picking cannot select obsolete data |
| Future macOS authoring | Same operations with native menu/Command/text behavior | Host-specific capabilities and errors; engine Metal support alone does not qualify the editor |

## Evaluation

Use sketches to assess hierarchy and discoverability. Use runnable prototypes
for efficiency, large models, text/IME, screen readers, renderer composition and
failure recovery. Observe role-appropriate users doing the same realistic tasks
without coaching; record completion, wrong turns, recovery and repeated-task
cost. Preference is useful feedback but not a performance benchmark.

Current S1 tests cover action identity, panel recovery and preferences. They do
not prove the new interactions. Verify keyboard-only, real text input, high DPI,
light/dark, narrow browser container and actual native window-manager tasks.
Offscreen images do not establish screen-reader behavior, Wayland/macOS/Windows
input or rendered frames. Add focused-buffer, transaction, stale job and
savepoint acceptance with the corresponding implementation slice.

The [October 6 review](editor-gui-reference-review.md) records the actual
Lightbown/Wihlidal/Nystrom excerpts and their effect. The historical
[October 4 task script](editor-design-review.md) remains useful for current native
workflows; the GUI systems delivery gates qualify the new target.
