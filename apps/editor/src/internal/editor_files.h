#pragma once

#include "internal/browser_folder.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
struct EditorDocumentDialog
{
    QString Title;
    QString Directory;
    QString Filter;
    bool ProjectFolder = false;
    // Audio imports share the session budget but admit WAV/FLAC up to the audio
    // source limit rather than the smaller configuration-document limit.
    bool IsAudioSource = false;
};

// Native dialogs retain the existing filesystem behavior. In the browser, project
// folders open in place through OpenBrowserFolder; single documents are copied
// into the session filesystem within BROWSER_FILE_BUDGET_BYTES. Completion is
// lifetime-guarded.
void OpenEditorDocument(QWidget* owner,
                        const EditorDocumentDialog& dialog,
                        const std::function<void(const QString&)>& loaded);
// Validate import names before reading bytes; preserve the descriptor output on failure.
[[nodiscard]] bool ValidateEditorImportPaths(const QStringList& paths, bool folder, QString& descriptor);
// Encode regular project files outside generated folders as a ustar archive of at most
// `maximumBytes`; preserve output on failure.
[[nodiscard]] bool EncodeEditorProjectArchive(const QString& directory,
                                              QByteArray& outArchive,
                                              foundation::uint64 maximumBytes = BROWSER_FILE_BUDGET_BYTES);
// Copy a bundled sample to a new writable project; preserve the output on failure.
[[nodiscard]] bool CopyEditorSample(const QString& id, QString& descriptor, const QString& destinationRoot = {});
/// Return the CMake target `ludus create` derives from a project name: ASCII letters, digits and `_.+-` stay,
/// every other code point becomes `_`, leading and trailing `._+-` are trimmed, an empty result becomes `app`,
/// and the result is cut to 128 characters. Mirrors ludus_tools.create._derive_target.
[[nodiscard]] QString DeriveProjectTarget(const QString& name);
/// Create a project in the absent folder `destination` from the bundled minimal template, which the build
/// renders from ludus_tools.create.session_template_files, substituting `name` and its derived target.
/// Used where the Python tooling cannot run (the browser editor). On failure nothing remains at
/// `destination`, `outDescriptor` is preserved and `outProblem` names the cause and the next step.
[[nodiscard]] bool CreateEditorProjectFromTemplate(const QString& name,
                                                   const QString& destination,
                                                   QString& outDescriptor,
                                                   QString& outProblem);
#if defined(Q_OS_WASM)
/// Pick a scene and report its entry path (.pbrt, .gltf, .glb, .obj or .luscene.json; lifetime-guarded).
/// isFolder opens a whole scene folder in place, read-only (OpenBrowserFolder); otherwise one .zip, or an
/// entry file with its companion files, is copied into the session within BROWSER_FILE_BUDGET_BYTES.
/// Refused selections write nothing.
void OpenBrowserSceneImport(QWidget* owner, bool isFolder, const std::function<void(const QString&)>& loaded);
/// Open a folder from disk in place, without copying it into the session (browser_host_folder.cpp).
/// Where the browser has showDirectoryPicker (Chrome, Edge), a Project folder is read-write: files are read on
/// first open and saves are written back to the folder, and the folder is remembered for
/// ReopenBrowserProjectFolder. Other browsers list it with a directory <input>: files are still read on first
/// open, but edits stay in the session (Download Project saves them). Scene folders are always read-only.
/// `loaded` receives the session path of ludus.project.json (Project) or of the scene entry (Scene). A refused
/// folder mounts nothing and is reported with its counts or sizes and the next step.
void OpenBrowserFolder(QWidget* owner, BrowserFolderIntent intent, const std::function<void(const QString&)>& loaded);
/// Reopen the project folder last opened read-write in this browser, after the browser confirms permission
/// (a prompt that needs the click that triggered this call). Reports when none is remembered.
void ReopenBrowserProjectFolder(QWidget* owner, const std::function<void(const QString&)>& loaded);
/// Whether `path` lies in a folder opened from disk whose saves are written back to it.
[[nodiscard]] bool IsBrowserFolderWritable(const QString& path);
// Starts a browser download, which cannot confirm the user's destination/save.
[[nodiscard]] bool DownloadEditorDocument(QWidget* owner, const QString& path);
[[nodiscard]] bool DownloadEditorProject(QWidget* owner, const QString& descriptor);
void MarkBrowserEdited();
#endif
} // namespace ludus::editor
