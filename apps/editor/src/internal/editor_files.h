#pragma once

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

// Native dialogs retain the existing filesystem behavior. Browser imports copy
// bounded documents or complete project folders into the session filesystem.
// Completion is lifetime-guarded.
void OpenEditorDocument(QWidget* owner,
                        const EditorDocumentDialog& dialog,
                        const std::function<void(const QString&)>& loaded);
// Validate import names before reading bytes; preserve the descriptor output on failure.
[[nodiscard]] bool ValidateEditorImportPaths(const QStringList& paths, bool folder, QString& descriptor);
// Encode regular project files as a bounded ustar archive; preserve output on failure.
[[nodiscard]] bool EncodeEditorProjectArchive(const QString& directory, QByteArray& archive);
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
/// Copy a selected scene into MEMFS and report its entry path (.pbrt, .gltf, .glb, .obj or .luscene.json;
/// lifetime-guarded). isFolder picks a whole scene folder; otherwise one .zip, or an entry
/// file with its companion files.
/// The browser editor's 32 MiB session import budget applies; refused selections write nothing.
void OpenBrowserSceneImport(QWidget* owner, bool isFolder, const std::function<void(const QString&)>& loaded);
// Starts a browser download, which cannot confirm the user's destination/save.
[[nodiscard]] bool DownloadEditorDocument(QWidget* owner, const QString& path);
[[nodiscard]] bool DownloadEditorProject(QWidget* owner, const QString& descriptor);
void MarkBrowserEdited();
#endif
} // namespace ludus::editor
