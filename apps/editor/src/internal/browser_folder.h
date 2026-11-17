#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>
#include <QStringList>

// Admission and failure text for folders the browser editor opens from disk.
// The browser adapter (browser_host_folder.cpp) and apps/editor/browser/host_folder.js
// mount a picked folder without copying it; this file holds the parts that need no
// browser, so native tests check them. See docs/architecture/editor-browser-strategy.md.

#if !defined(LUDUS_EDITOR_WASM_MAXIMUM_MEMORY_BYTES)
#    error "apps/editor/CMakeLists.txt defines LUDUS_EDITOR_WASM_MAXIMUM_MEMORY_BYTES for the editor sources"
#endif

namespace ludus::editor
{
/// Maximum WebAssembly memory of the browser editor (QT_WASM_MAXIMUM_MEMORY in apps/editor/CMakeLists.txt).
inline constexpr foundation::uint64 BROWSER_EDITOR_MAXIMUM_MEMORY_BYTES = LUDUS_EDITOR_WASM_MAXIMUM_MEMORY_BYTES;

/// Bytes of file data the browser editor holds at once, and the admission limit derived from its memory.
///
/// One quarter of the maximum WebAssembly memory: a file at this size can be resident in the session file
/// system, read into WebAssembly memory and decoded there while the editor keeps working. It bounds the largest
/// single file of an opened folder, the resident copies of folder files (older clean copies are dropped and read
/// again on their next open), the files copied into the session by document, audio and archive imports, and a
/// Download Project archive. A folder's total size on disk is not limited.
inline constexpr foundation::uint64 BROWSER_FILE_BUDGET_BYTES = BROWSER_EDITOR_MAXIMUM_MEMORY_BYTES / 4;

/// Most files one project folder or project archive may contain outside its generated folders.
inline constexpr qsizetype MAX_BROWSER_FOLDER_FILES = 4096;

/// What a picked folder is opened for.
enum class BrowserFolderIntent : foundation::uint8
{
    /// A project folder containing ludus.project.json at its top level; read-write where the browser allows.
    Project,
    /// A scene folder whose .pbrt, .gltf, .glb, .obj or .luscene.json entry is imported; always read-only.
    Scene,
};

/// One regular file of a picked folder, as the browser listed it.
struct BrowserFolderEntry
{
    /// Path relative to the picked folder, with `/` separators.
    QString Path;
    /// Size in bytes when the folder was listed.
    foundation::uint64 Size = 0;
};

/// Limits applied by AdmitBrowserFolder; the defaults are the browser editor's.
struct BrowserFolderLimits
{
    /// Largest single file.
    foundation::uint64 MaximumFileBytes = BROWSER_FILE_BUDGET_BYTES;
    /// Most files outside generated folders.
    qsizetype MaximumFiles = MAX_BROWSER_FOLDER_FILES;
};

/// A failure reported while picking, listing or using a browser folder.
struct BrowserFolderFailure
{
    /// One of `activation`, `permission`, `no-remembered`, `too-many`, `unreadable`, `read` or `write`.
    QString Kind;
    /// Name of the picked folder, when known.
    QString Name;
    /// Path relative to the folder (or within the session file system for `read`/`write`), when known.
    QString Path;
    /// The browser's error name and message, when it gave one.
    QString Error;
    /// Files listed when the browser stopped (`too-many`).
    foundation::uint64 Count = 0;
};

/// Top-level folders of a project that hold build outputs, caches or version control and are never opened,
/// exported or written back from the browser: out, build, .git, .ludus and node_modules.
[[nodiscard]] QStringList GetGeneratedProjectFolderNames();

/// Whether a path relative to a project root lies in one of GetGeneratedProjectFolderNames().
[[nodiscard]] bool IsGeneratedProjectPath(const QString& relative);

/// Format a byte count with binary units for messages, for example `16.0 MiB`.
[[nodiscard]] QString FormatBrowserByteSize(foundation::uint64 bytes);

/// The outcome of AdmitBrowserFolder.
struct BrowserFolderAdmission
{
    /// The descriptor (Project) or scene entry (Scene) as `name/relative/path`; empty when refused.
    QString Entry;
    /// The cause with the measured counts or sizes and the next step; empty when admitted.
    QString Problem;

    /// Whether the folder can be mounted.
    [[nodiscard]] bool IsAdmitted() const noexcept
    {
        return Problem.isEmpty() == true;
    }
};

/// Check a listed folder before anything is mounted or read.
/// @param name Name of the picked folder; it becomes the project folder name in the session.
/// @param entries Regular files outside generated folders, relative to the folder.
/// @param intent What the folder is opened for.
/// @param limits File-count and per-file limits.
/// @return The entry to open, or the problem to report.
[[nodiscard]] BrowserFolderAdmission AdmitBrowserFolder(const QString& name,
                                                        const QList<BrowserFolderEntry>& entries,
                                                        BrowserFolderIntent intent,
                                                        const BrowserFolderLimits& limits);

/// Validate scene import names; on success outEntry is the .pbrt entry if any, else the first other scene entry.
/// `isFolder` requires every path below one shared top-level folder; otherwise paths are flat file names, or a
/// single .zip. Preserves outEntry on failure.
[[nodiscard]] bool ValidateSceneImportPaths(const QStringList& paths, bool isFolder, QString& outEntry) noexcept;

/// The message for a picking, listing, read or write failure: what happened, measured counts and the next step.
[[nodiscard]] QString DescribeBrowserFolderFailure(const BrowserFolderFailure& failure,
                                                   BrowserFolderIntent intent,
                                                   qsizetype maximumFiles = MAX_BROWSER_FOLDER_FILES);
} // namespace ludus::editor
