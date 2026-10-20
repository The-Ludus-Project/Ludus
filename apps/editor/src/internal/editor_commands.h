#pragma once

// Editor shell command catalogue (private editor header, not installed).
//
// Each descriptor names one stable command ID, its menu label, a short toolbar
// label, an optional portable shortcut and a tooltip. MainWindow creates exactly
// one QAction per descriptor and adds that same action to menus and toolbars, so
// capability gates, shortcuts and checked state agree everywhere. Descriptors are
// presentation metadata only; the document or controller operation behind a
// command still validates at dispatch. See
// docs/architecture/editor-gui-systems.md#commands-and-property-presentation.

#include <ludus/foundation/base/types.h>

#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QAction;
class QObject;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Presentation metadata for one shell command.
struct EditorCommand final
{
    /// Stable ID; also the QAction objectName that tests and automation find.
    const char* Id;
    /// Menu label, with an `&` mnemonic.
    const char* Text;
    /// Short toolbar label; empty uses Text without its mnemonic.
    const char* ToolbarText;
    /// Portable QKeySequence text (for example "Ctrl+F7"); empty for none.
    const char* Shortcut;
    /// Tooltip and status tip; the shortcut is appended when present.
    const char* Help;
    /// True for a toggle such as Live Preview.
    bool IsCheckable;
};

// Render-graph commands act on the open render-graph document.
inline constexpr EditorCommand RENDER_GRAPH_NEW_COMMAND
{
    .Id = "renderGraph.new",
    .Text = "New Render &Graph...",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Create an editable render graph and its shaders in this project's graphs folder.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_OPEN_COMMAND
{
    .Id = "renderGraph.open",
    .Text = "Open Render G&raph...",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Open a root .lurender.json in this project as the render-graph document.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_VALIDATE_COMMAND
{
    .Id = "renderGraph.validate",
    .Text = "&Validate Render Graph",
    .ToolbarText = "Validate",
    .Shortcut = "Ctrl+F7",
    .Help = "Validate the render-graph draft and show its serial plan; saved files are unchanged.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_BUILD_COMMAND
{
    .Id = "renderGraph.build",
    .Text = "Build Render &Graph",
    .ToolbarText = "Build Graph",
    .Shortcut = "F7",
    .Help = "Build the render-graph draft's shaders into the project's out/render-graph tree.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_BUILD_PREVIEW_COMMAND
{
    .Id = "renderGraph.buildPreview",
    .Text = "Build Render Graph and &Preview",
    .ToolbarText = "Build && Preview",
    .Shortcut = "Shift+F7",
    .Help = "Rebuild and cook the render-graph draft, then preview it on the World viewport device.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_LIVE_PREVIEW_COMMAND
{
    .Id = "renderGraph.livePreview",
    .Text = "&Live Render Graph Preview",
    .ToolbarText = "Live Preview",
    .Shortcut = "Ctrl+Shift+F7",
    .Help = "Update the preview after graph or shader saves; keep the last working preview on failure.",
    .IsCheckable = true,
};
inline constexpr EditorCommand RENDER_GRAPH_COOK_COMMAND
{
    .Id = "renderGraph.cook",
    .Text = "&Cook Render Graph Runtime",
    .ToolbarText = "Cook",
    .Shortcut = "",
    .Help = "Cook the last published render-graph build into a runtime generation.",
    .IsCheckable = false,
};
inline constexpr EditorCommand RENDER_GRAPH_CANCEL_COMMAND
{
    .Id = "renderGraph.cancel",
    .Text = "Cance&l Render Graph Job",
    .ToolbarText = "Cancel",
    .Shortcut = "Ctrl+Break",
    .Help = "Stop the running render-graph validation, build, cook or graph creation and turn Live Preview off.",
    .IsCheckable = false,
};

inline constexpr EditorCommand PROFILING_RECORD_COMMAND
{
    .Id = "profiling.record",
    .Text = "Record &Profiling",
    .ToolbarText = "Record Profiling",
    .Shortcut = "F9",
    .Help = "Record or stop editor CPU scopes, viewport GPU durations, or both in a Perfetto trace.",
    .IsCheckable = true,
};
inline constexpr EditorCommand VIEW_PROFILING_COMMAND
{
    .Id = "view.profiling",
    .Text = "&Profiling",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Choose CPU, GPU or Both and manage Perfetto recordings.",
    .IsCheckable = false,
};

// Tool-window commands show and focus one tool window, like Visual Studio's View menu.
inline constexpr EditorCommand VIEW_PROJECT_EXPLORER_COMMAND
{
    .Id = "view.projectExplorer",
    .Text = "&Project Explorer",
    .ToolbarText = "",
    .Shortcut = "Ctrl+Alt+L",
    .Help = "Show the project's worlds, render graphs, shaders and content.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_PROPERTIES_COMMAND
{
    .Id = "view.properties",
    .Text = "P&roperties",
    .ToolbarText = "",
    .Shortcut = "F4",
    .Help = "Show the properties of the active document's selection.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_ERROR_LIST_COMMAND
{
    .Id = "view.errorList",
    .Text = "&Error List",
    .ToolbarText = "",
    .Shortcut = "Ctrl+Alt+E",
    .Help = "Show render-graph, shader and build diagnostics.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_OUTPUT_COMMAND
{
    .Id = "view.output",
    .Text = "&Output",
    .ToolbarText = "",
    .Shortcut = "Ctrl+Alt+O",
    .Help = "Show build, tool and runtime output.",
    .IsCheckable = false,
};

inline constexpr EditorCommand VIEW_CONTENT_COMMAND
{
    .Id = "view.content",
    .Text = "&Content",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Show the project's content catalog and imports.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_LIVE_INSPECTOR_COMMAND
{
    .Id = "view.liveInspector",
    .Text = "&Live Inspector",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Show the running game's live properties.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_RENDER_PREVIEW_COMMAND
{
    .Id = "view.renderPreview",
    .Text = "Render Pre&view",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Show the render-graph preview of the World viewport.",
    .IsCheckable = false,
};
inline constexpr EditorCommand VIEW_TIMING_COMMAND
{
    .Id = "view.timing",
    .Text = "&Timing",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Show the running game's frame timing.",
    .IsCheckable = false,
};

// Document commands open one document view in the central document well.
inline constexpr EditorCommand PROJECT_SETTINGS_COMMAND
{
    .Id = "project.settings",
    .Text = "Project &Settings",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Open the project's name, build profile and launch settings.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_CLOSE_ALL_DOCUMENTS_COMMAND
{
    .Id = "window.closeAllDocuments",
    .Text = "Close &All Documents",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Close every document tab. Drafts stay open; reopen them from Project Explorer or the Window menu.",
    .IsCheckable = false,
};

// Tools commands open editor-wide preferences.
inline constexpr EditorCommand TOOLS_OPTIONS_COMMAND
{
    .Id = "tools.options",
    .Text = "&Options...",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Edit editor preferences, including render-graph shader tools and the native shader worker.",
    .IsCheckable = false,
};

// Window commands arrange documents and tool windows.
inline constexpr EditorCommand WINDOW_NEW_VERTICAL_GROUP_COMMAND
{
    .Id = "window.newVerticalTabGroup",
    .Text = "New &Vertical Tab Group",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Move the active document into a new tab group beside the current one.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_NEW_HORIZONTAL_GROUP_COMMAND
{
    .Id = "window.newHorizontalTabGroup",
    .Text = "New &Horizontal Tab Group",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Move the active document into a new tab group below the current one.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_MOVE_NEXT_GROUP_COMMAND
{
    .Id = "window.moveToNextTabGroup",
    .Text = "Move to &Next Tab Group",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Move the active document into the next tab group.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_MOVE_PREVIOUS_GROUP_COMMAND
{
    .Id = "window.moveToPreviousTabGroup",
    .Text = "Move to P&revious Tab Group",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Move the active document into the previous tab group.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_MERGE_GROUPS_COMMAND
{
    .Id = "window.mergeTabGroups",
    .Text = "&Merge All Tab Groups",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Move every open document back into one tab group.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_CLOSE_DOCUMENT_COMMAND
{
    .Id = "window.closeDocument",
    .Text = "&Close Document",
    .ToolbarText = "",
    .Shortcut = "Ctrl+W",
    .Help = "Close the active document's tab. Its draft stays open; reopen it from Project Explorer or the "
            "Window menu.",
    .IsCheckable = false,
};
inline constexpr EditorCommand WINDOW_RESET_LAYOUT_COMMAND
{
    .Id = "resetWorkspaceLayout",
    .Text = "Reset Window &Layout",
    .ToolbarText = "",
    .Shortcut = "",
    .Help = "Restore the default document and tool-window layout. Documents and drafts are unchanged.",
    .IsCheckable = false,
};

/// Create the single QAction for a catalogue command, owned by owner. The caller connects
/// triggered/toggled and adds the same action to every menu and toolbar that shows it.
[[nodiscard]] QAction* CreateEditorCommand(const EditorCommand& command, QObject* owner);
} // namespace ludus::editor
