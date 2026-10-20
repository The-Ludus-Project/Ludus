#include "internal/editor_files.h"

#include "internal/scene_archive.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QWeakPointer>
#include <QWidget>

#include <utility>

#if defined(Q_OS_WASM)
#    include <QtCore/private/qstdweb_p.h>
#    include <ludus/audio/audio_types.h>
#endif

namespace ludus::editor
{
namespace
{
bool IsGeneratedProjectPath(const QString& relative)
{
    for (const auto& name : {QStringLiteral("out"),
                             QStringLiteral("build"),
                             QStringLiteral(".git"),
                             QStringLiteral(".ludus"),
                             QStringLiteral("node_modules")})
    {
        if (relative == name || relative.startsWith(name + QLatin1Char('/')))
        {
            return true;
        }
    }
    return false;
}

/// A file Ludus::SceneCook can import as a best-effort scene entry; the importer
/// itself is chosen from the file's content.
[[maybe_unused]] bool IsSceneEntryName(const QString& name)
{
    for (const QString& extension :
         {QStringLiteral(".pbrt"), QStringLiteral(".gltf"), QStringLiteral(".glb"), QStringLiteral(".obj")})
    {
        if (name.endsWith(extension, Qt::CaseInsensitive) == true)
        {
            return true;
        }
    }
    return false;
}
} // namespace
#if defined(Q_OS_WASM)
namespace
{
constexpr foundation::int64 MAX_DOCUMENT_BYTES = 1024LL * 1024;
constexpr foundation::int64 MAX_IMPORT_BYTES = 32 * MAX_DOCUMENT_BYTES;
foundation::int64 ImportedBytes = 0;
foundation::uint32 ImportSerial = 0;

struct BrowserImport
{
    QPointer<QWidget> Owner;
    QString Name;
    QString Directory;
    QString Descriptor;
    QStringList Paths;
    QList<foundation::uint32> Indices;
    emscripten::val Files = emscripten::val::undefined();
    foundation::uint32 Index = 0;
    bool Folder = false;
    bool IsAudioSource = false;
    bool Admitted = false;
    foundation::int64 ChargedBytes = 0;
    emscripten::val Input = emscripten::val::undefined();
    QSharedPointer<qstdweb::EventCallback> Change;
    QSharedPointer<qstdweb::EventCallback> Cancel;
    QSharedPointer<qstdweb::FileReader> Reader;
    foundation::uint64 Size = 0;
    std::function<void(const QString&)> Completed;
};
QSharedPointer<BrowserImport> PendingImport;
void Report(QWidget* owner, const QString& message)
{
    auto* dialog =
        new QMessageBox(QMessageBox::Warning, QStringLiteral("Browser files"), message, QMessageBox::Ok, owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}
} // namespace
extern "C" void LudusBrowserDownloadDocument(const char* name, const foundation::uint8* bytes, foundation::int32 size);
extern "C" void LudusBrowserMarkEdited();
void MarkBrowserEdited()
{
    LudusBrowserMarkEdited();
}
#endif

void OpenEditorDocument(QWidget* owner,
                        const EditorDocumentDialog& dialog,
                        const std::function<void(const QString&)>& loaded)
{
#if defined(Q_OS_WASM)
    if (PendingImport)
    {
        return;
    }
    auto state = QSharedPointer<BrowserImport>::create();
    PendingImport = state;
    state->Owner = owner;
    state->Folder = dialog.ProjectFolder;
    state->IsAudioSource = dialog.IsAudioSource;
    state->Completed = loaded;
    const QWeakPointer<BrowserImport> weak(state);
    // Thanks to Qt Group, qstdweb::EventCallback/FileReader (Qt 6.10.3): these
    // callbacks participate in Qt's suspend/resume dispatcher. A raw Wasm call
    // from a JavaScript promise cannot safely re-enter modal editor workflows.
    // Thanks also to MDN contributors, "<input type=file>" and "cancel event":
    // check size before reading, and release selection on cancellation.
    // https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/input/file
    // https://developer.mozilla.org/en-US/docs/Web/API/HTMLInputElement/cancel_event
    // MDN contributors, "HTMLInputElement: webkitdirectory property": retain
    // File.webkitRelativePath while copying a selected directory hierarchy.
    // https://developer.mozilla.org/en-US/docs/Web/API/HTMLInputElement/webkitdirectory
    // Only this private adapter depends on the exact pinned Qt implementation.
    // https://code.qt.io/cgit/qt/qtbase.git/tree/src/corelib/platform/wasm/qstdweb.cpp?h=v6.10.3
    state->Input = emscripten::val::global("document").call<emscripten::val>("createElement", emscripten::val("input"));
    state->Input.set("type", emscripten::val("file"));
    if (state->Folder)
    {
        state->Input.set("webkitdirectory", true);
        state->Input.set("multiple", true);
    }
    else
    {
        state->Input.set("accept",
                         emscripten::val(state->IsAudioSource == true ? ".wav,.flac" : ".json,application/json"));
    }
    state->Input.set("hidden", true);
    auto finish = [weak]() {
        const auto state = weak.toStrongRef();
        if (state)
        {
            state->Input.call<void>("remove");
            if (state->Admitted == false)
            {
                ImportedBytes -= state->ChargedBytes;
                state->ChargedBytes = 0;
                if (state->Directory.isEmpty() == false)
                {
                    (void)QDir(state->Directory).removeRecursively();
                }
            }
            if (PendingImport == state)
            {
                PendingImport.clear();
            }
        }
    };
    state->Cancel = QSharedPointer<qstdweb::EventCallback>::create(state->Input,
                                                                   "cancel",
                                                                   [finish](const emscripten::val&) { finish(); });
    state->Change =
        QSharedPointer<qstdweb::EventCallback>::create(state->Input, "change", [weak, finish](const emscripten::val&) {
            const auto state = weak.toStrongRef();
            if (state == nullptr || state->Owner.isNull())
            {
                finish();
                return;
            }
            const auto files = state->Input["files"];
            const auto count = files["length"].as<foundation::uint32>();
            if (count == 0 || count > 100000 || (state->Folder == false && count != 1))
            {
                Report(state->Owner,
                       QStringLiteral("Choose a source project folder with at most 4096 imported files."));
                finish();
                return;
            }
            foundation::uint64 total = 0;
            for (foundation::uint32 index = 0; index < count; ++index)
            {
                const qstdweb::File file(files[index]);
                const auto path = state->Folder
                                      ? QString::fromStdString(files[index]["webkitRelativePath"].as<std::string>())
                                      : QFileInfo(QString::fromStdString(file.name())).fileName();
                const bool isAudioName = path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive) == true ||
                                         path.endsWith(QStringLiteral(".flac"), Qt::CaseInsensitive) == true;
                if (state->IsAudioSource == true && isAudioName == false)
                {
                    Report(state->Owner, QStringLiteral("Choose a WAV or FLAC source."));
                    finish();
                    return;
                }
                if (state->Folder && IsGeneratedProjectPath(path.section(QLatin1Char('/'), 1)))
                {
                    continue;
                }
                total += file.size();
                const foundation::uint64 maximumFileBytes = state->IsAudioSource == true
                                                                ? audio::BROWSER_ENCODED_CAP_BYTES
                                                                : static_cast<foundation::uint64>(MAX_DOCUMENT_BYTES);
                if (file.size() > static_cast<foundation::uint64>(MAX_IMPORT_BYTES) ||
                    total > static_cast<foundation::uint64>(MAX_IMPORT_BYTES - ImportedBytes) ||
                    (state->Folder == false && (file.size() == 0 || file.size() > maximumFileBytes)))
                {
                    Report(state->Owner,
                           QStringLiteral("Import refused: invalid paths or the 32 MiB session limit exceeded."));
                    finish();
                    return;
                }
                state->Paths.append(path);
                state->Indices.append(index);
            }
            if (ValidateEditorImportPaths(state->Paths, state->Folder, state->Descriptor) == false)
            {
                Report(state->Owner,
                       QStringLiteral(
                           "Choose a folder containing ludus.project.json with valid, unique relative file paths."));
                finish();
                return;
            }
            state->ChargedBytes = static_cast<foundation::int64>(total);
            ImportedBytes += state->ChargedBytes;
            state->Files = files;
            state->Directory = QStringLiteral("/browser-imports/") + QString::number(++ImportSerial);
            state->Size = qstdweb::File(files[state->Indices[0]]).size();
            state->Name = state->Paths[0];
            state->Reader = QSharedPointer<qstdweb::FileReader>::create();
            auto failed = [weak, finish](const emscripten::val&) {
                const auto state = weak.toStrongRef();
                if (state && state->Owner.isNull() == false)
                {
                    Report(state->Owner, QStringLiteral("Import failed; the current document was retained."));
                }
                finish();
            };
            state->Reader->onError(failed);
            state->Reader->onAbort(failed);
            state->Reader->onLoad([weak, finish](const emscripten::val&) {
                const auto state = weak.toStrongRef();
                if (state == nullptr || state->Owner.isNull())
                {
                    finish();
                    return;
                }
                const auto result = state->Reader->result();
                if (result.byteLength() != state->Size)
                {
                    Report(state->Owner, QStringLiteral("Could not read the complete selected document."));
                    finish();
                    return;
                }
                const auto bytes = qstdweb::Uint8Array(result).copyToQByteArray();
                const auto path = QDir(state->Directory).filePath(state->Name);
                QSaveFile output(path);
                output.setDirectWriteFallback(false);
                if (QDir().mkpath(QFileInfo(path).absolutePath()) == false ||
                    output.open(QIODevice::WriteOnly) == false || output.write(bytes) != bytes.size() ||
                    output.commit() == false)
                {
                    Report(state->Owner, QStringLiteral("Import failed; the current document was retained."));
                    finish();
                    return;
                }
                ++state->Index;
                if (state->Index < static_cast<foundation::uint32>(state->Paths.size()))
                {
                    state->Name = state->Paths[state->Index];
                    const qstdweb::File next(state->Files[state->Indices[state->Index]]);
                    state->Size = next.size();
                    state->Reader->readAsArrayBuffer(next.slice(0, state->Size));
                    return;
                }
                state->Admitted = true;
                const auto descriptor = state->Folder ? QDir(state->Directory).filePath(state->Descriptor) : path;
                finish();
                state->Completed(descriptor);
            });
            state->Reader->readAsArrayBuffer(qstdweb::File(files[state->Indices[0]]).slice(0, state->Size));
        });
    emscripten::val::global("document")["body"].call<void>("appendChild", state->Input);
    state->Input.call<void>("click");
#else
    const auto selection = dialog.ProjectFolder
                               ? QFileDialog::getExistingDirectory(owner, dialog.Title, dialog.Directory)
                               : QFileDialog::getOpenFileName(owner, dialog.Title, dialog.Directory, dialog.Filter);
    const QString path = dialog.ProjectFolder && selection.isEmpty() == false
                             ? QDir(selection).filePath(QStringLiteral("ludus.project.json"))
                             : selection;
    if (path.isEmpty() == false)
    {
        loaded(path);
    }
#endif
}

#if defined(Q_OS_WASM)
namespace
{
[[nodiscard]] bool ValidateSceneImportPaths(const QStringList& paths, bool folder, QString& outEntry) noexcept
{
    if (paths.isEmpty() == true || paths.size() > 4096)
    {
        return false;
    }
    QSet<QString> unique;
    QString root;
    QString entry;
    // A .pbrt entry wins; glTF/OBJ files are entries only for scenes without one,
    // because pbrt scenes may ship .obj companions.
    QString otherEntry;
    const auto consider = [&entry, &otherEntry](const QString& name, const QString& path) {
        if (name.endsWith(QStringLiteral(".pbrt"), Qt::CaseInsensitive) == true)
        {
            entry = entry.isEmpty() == true ? path : entry;
        }
        else if (IsSceneEntryName(name) == true && otherEntry.isEmpty() == true)
        {
            otherEntry = path;
        }
    };
    for (const QString& path : paths)
    {
        const QStringList parts = path.split(QLatin1Char('/'));
        if (path.toUtf8().size() > 4096 || QDir::isAbsolutePath(path) == true ||
            path.contains(QLatin1Char('\\')) == true || path.contains(QChar(0)) == true ||
            unique.contains(path) == true)
        {
            return false;
        }
        for (const QString& part : parts)
        {
            if (part.isEmpty() == true || part == QStringLiteral(".") || part == QStringLiteral(".."))
            {
                return false;
            }
        }
        if (folder == true)
        {
            if (parts.size() < 2 || (root.isEmpty() == false && parts.first() != root))
            {
                return false;
            }
            root = parts.first();
            consider(parts.back(), path);
        }
        else if (parts.size() != 1)
        {
            return false;
        }
        else if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) == true)
        {
            if (paths.size() != 1)
            {
                return false; // An archive carries its own companions.
            }
            entry = path;
        }
        else
        {
            consider(path, path);
        }
        unique.insert(path);
    }
    entry = entry.isEmpty() == true ? otherEntry : entry;
    if (entry.isEmpty() == true)
    {
        return false;
    }
    outEntry = entry;
    return true;
}
} // namespace

void OpenBrowserSceneImport(QWidget* owner, bool isFolder, const std::function<void(const QString&)>& loaded)
{
    if (PendingImport)
    {
        return;
    }
    QSharedPointer<BrowserImport> state = QSharedPointer<BrowserImport>::create();
    PendingImport = state;
    state->Owner = owner;
    state->Folder = isFolder;
    state->Completed = loaded;
    const QWeakPointer<BrowserImport> weak(state);
    state->Input = emscripten::val::global("document").call<emscripten::val>("createElement", emscripten::val("input"));
    state->Input.set("type", emscripten::val("file"));
    state->Input.set("multiple", true);
    if (isFolder == true)
    {
        // A scene folder keeps the relative textures/ and models/ paths that the entry references.
        state->Input.set("webkitdirectory", true);
    }
    else
    {
        // One .zip archive, or a scene entry plus its companion files picked together (flat).
        state->Input.set("accept",
                         emscripten::val(".pbrt,.gltf,.glb,.bin,.obj,.mtl,.zip,application/zip,image/*,.ply,.spd,"
                                         ".exr,.hdr"));
    }
    state->Input.set("hidden", true);
    auto finish = [weak]() {
        const QSharedPointer<BrowserImport> state = weak.toStrongRef();
        if (state)
        {
            state->Input.call<void>("remove");
            if (state->Admitted == false)
            {
                ImportedBytes -= state->ChargedBytes;
                state->ChargedBytes = 0;
                if (state->Directory.isEmpty() == false)
                {
                    (void)QDir(state->Directory).removeRecursively();
                }
            }
            if (PendingImport == state)
            {
                PendingImport.clear();
            }
        }
    };
    state->Cancel = QSharedPointer<qstdweb::EventCallback>::create(state->Input,
                                                                   "cancel",
                                                                   [finish](const emscripten::val&) { finish(); });
    state->Change =
        QSharedPointer<qstdweb::EventCallback>::create(state->Input, "change", [weak, finish](const emscripten::val&) {
            const QSharedPointer<BrowserImport> state = weak.toStrongRef();
            if (state.isNull() == true || state->Owner.isNull() == true)
            {
                finish();
                return;
            }
            const emscripten::val files = state->Input["files"];
            const foundation::uint32 count = files["length"].as<foundation::uint32>();
            if (count == 0 || count > 100000)
            {
                Report(state->Owner,
                       QStringLiteral("Choose a scene folder, one .pbrt/.gltf/.glb/.obj file, or a .zip archive."));
                finish();
                return;
            }
            foundation::uint64 total = 0;
            for (foundation::uint32 index = 0; index < count; ++index)
            {
                const qstdweb::File file(files[index]);
                const QString path = state->Folder
                                         ? QString::fromStdString(files[index]["webkitRelativePath"].as<std::string>())
                                         : QFileInfo(QString::fromStdString(file.name())).fileName();
                if (path.isEmpty() == true)
                {
                    continue;
                }
                total += file.size();
                if (file.size() > static_cast<foundation::uint64>(MAX_IMPORT_BYTES) ||
                    total > static_cast<foundation::uint64>(MAX_IMPORT_BYTES - ImportedBytes))
                {
                    Report(state->Owner,
                           QStringLiteral("Import refused: the 32 MiB session import limit was exceeded."));
                    finish();
                    return;
                }
                state->Paths.append(path);
                state->Indices.append(index);
            }
            if (ValidateSceneImportPaths(state->Paths, state->Folder, state->Descriptor) == false)
            {
                // Fall back to a single-file picker if the directory selection had no scene entry.
                Report(state->Owner,
                       QStringLiteral("Choose a folder containing a .pbrt, .gltf, .glb or .obj entry (and its "
                                      "textures/models), or a single entry / .zip file."));
                finish();
                return;
            }
            state->ChargedBytes = static_cast<foundation::int64>(total);
            ImportedBytes += state->ChargedBytes;
            state->Files = files;
            state->Directory = QStringLiteral("/browser-imports/") + QString::number(++ImportSerial);
            state->Size = qstdweb::File(files[state->Indices[0]]).size();
            state->Name = state->Paths[0];
            state->Reader = QSharedPointer<qstdweb::FileReader>::create();
            auto failed = [weak, finish](const emscripten::val&) {
                const QSharedPointer<BrowserImport> state = weak.toStrongRef();
                if (state && state->Owner.isNull() == false)
                {
                    Report(state->Owner, QStringLiteral("Import failed; the current document was retained."));
                }
                finish();
            };
            state->Reader->onError(failed);
            state->Reader->onAbort(failed);
            state->Reader->onLoad([weak, finish](const emscripten::val&) {
                const QSharedPointer<BrowserImport> state = weak.toStrongRef();
                if (state.isNull() == true || state->Owner.isNull() == true)
                {
                    finish();
                    return;
                }
                const qstdweb::ArrayBuffer result = state->Reader->result();
                if (result.byteLength() != state->Size)
                {
                    Report(state->Owner, QStringLiteral("Could not read the complete selected document."));
                    finish();
                    return;
                }
                const QByteArray bytes = qstdweb::Uint8Array(result).copyToQByteArray();
                if (state->Name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) == true && state->Folder == false)
                {
                    // Expand the archive into MEMFS beside the other session imports.
                    // Charge the declared expanded size, not the compressed file, against the
                    // session budget before decompressing (the archive bytes are released after).
                    QString error;
                    const foundation::int64 available = MAX_IMPORT_BYTES - (ImportedBytes - state->ChargedBytes);
                    foundation::uint64 expanded = 0;
                    if (ExpandSceneArchive(bytes,
                                           state->Directory,
                                           static_cast<foundation::uint64>(available),
                                           expanded,
                                           error) == false)
                    {
                        Report(state->Owner, error);
                        finish();
                        return;
                    }
                    ImportedBytes += static_cast<foundation::int64>(expanded) - state->ChargedBytes;
                    state->ChargedBytes = static_cast<foundation::int64>(expanded);
                    QString entry;
                    // Prefer a .pbrt entry; otherwise the first glTF/OBJ entry.
                    QDirIterator pbrtFiles(state->Directory,
                                           QStringList{QStringLiteral("*.pbrt")},
                                           QDir::Files,
                                           QDirIterator::Subdirectories);
                    QDirIterator otherFiles(
                        state->Directory,
                        QStringList{QStringLiteral("*.gltf"), QStringLiteral("*.glb"), QStringLiteral("*.obj")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
                    QDirIterator& files = pbrtFiles.hasNext() == true ? pbrtFiles : otherFiles;
                    if (files.hasNext() == false)
                    {
                        Report(state->Owner,
                               QStringLiteral("The .zip archive contains no .pbrt, .gltf, .glb or .obj entry."));
                        finish();
                        return;
                    }
                    entry = files.next();
                    state->Admitted = true;
                    finish();
                    state->Completed(entry);
                    return;
                }
                const QString path = QDir(state->Directory).filePath(state->Name);
                QSaveFile output(path);
                output.setDirectWriteFallback(false);
                if (QDir().mkpath(QFileInfo(path).absolutePath()) == false ||
                    output.open(QIODevice::WriteOnly) == false || output.write(bytes) != bytes.size() ||
                    output.commit() == false)
                {
                    Report(state->Owner, QStringLiteral("Import failed; the current document was retained."));
                    finish();
                    return;
                }
                ++state->Index;
                if (state->Index < static_cast<foundation::uint32>(state->Paths.size()))
                {
                    state->Name = state->Paths[state->Index];
                    const qstdweb::File next(state->Files[state->Indices[state->Index]]);
                    state->Size = next.size();
                    state->Reader->readAsArrayBuffer(next.slice(0, state->Size));
                    return;
                }
                state->Admitted = true;
                const QString descriptor = QDir(state->Directory).filePath(state->Descriptor);
                finish();
                state->Completed(descriptor);
            });
            state->Reader->readAsArrayBuffer(qstdweb::File(files[state->Indices[0]]).slice(0, state->Size));
        });
    emscripten::val::global("document")["body"].call<void>("appendChild", state->Input);
    state->Input.call<void>("click");
}
#endif

bool ValidateEditorImportPaths(const QStringList& paths, bool folder, QString& descriptor)
{
    if (paths.isEmpty() || paths.size() > 4096 || (folder == false && paths.size() != 1))
    {
        return false;
    }
    QSet<QString> unique;
    QString project;
    QString root;
    for (const auto& path : paths)
    {
        const auto parts = path.split(QLatin1Char('/'));
        if (path.toUtf8().size() > 4096 || QDir::isAbsolutePath(path) || path.contains(QLatin1Char('\\')) ||
            path.contains(QChar(0)) || unique.contains(path) || (folder == false && parts.size() != 1))
        {
            return false;
        }
        for (const auto& part : parts)
        {
            if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
            {
                return false;
            }
        }
        if (folder)
        {
            if (parts.size() < 2 || (root.isEmpty() == false && parts.first() != root))
            {
                return false;
            }
            root = parts.first();
            if (parts.size() == 2 && parts.back() == QStringLiteral("ludus.project.json"))
            {
                project = path;
            }
        }
        unique.insert(path);
    }
    if (folder && project.isEmpty())
    {
        return false;
    }
    descriptor = folder ? project : paths.first();
    return true;
}

bool EncodeEditorProjectArchive(const QString& directory, QByteArray& archive)
{
    constexpr foundation::int64 maxBytes = 32LL * 1024 * 1024;
    if (QDir(directory).exists() == false)
    {
        return false;
    }
    QByteArray result;
    QDirIterator files(directory, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    foundation::uint32 count = 0;
    while (files.hasNext())
    {
        const auto path = files.next();
        const auto relative = QDir(directory).relativeFilePath(path);
        if (IsGeneratedProjectPath(relative))
        {
            continue;
        }
        if (QFileInfo(path).isSymLink())
        {
            return false;
        }
        if (QFileInfo(path).isDir())
        {
            continue;
        }
        QFile input(path);
        if (++count > 4096 || QFileInfo(path).isSymLink() || input.open(QIODevice::ReadOnly) == false ||
            input.size() > maxBytes || result.size() + input.size() + 2048 > maxBytes)
        {
            return false;
        }
        auto name = relative.toUtf8();
        QByteArray prefix;
        if (name.size() > 100)
        {
            const auto slash = name.lastIndexOf('/', 155);
            if (slash <= 0 || name.size() - slash - 1 > 100)
            {
                return false;
            }
            prefix = name.first(slash);
            name = name.sliced(slash + 1);
        }
        QByteArray header(512, '\0');
        header.replace(0, name.size(), name);
        header.replace(100, 7, QByteArrayLiteral("0000644"));
        header.replace(108, 7, QByteArrayLiteral("0000000"));
        header.replace(116, 7, QByteArrayLiteral("0000000"));
        header.replace(124, 11, QByteArray::number(input.size(), 8).rightJustified(11, '0'));
        header.replace(136, 11, QByteArrayLiteral("00000000000"));
        header.replace(148, 8, QByteArray(8, ' '));
        header[156] = '0';
        header.replace(257, 5, QByteArrayLiteral("ustar"));
        header.replace(263, 2, QByteArrayLiteral("00"));
        header.replace(345, prefix.size(), prefix);
        foundation::uint32 checksum = 0;
        for (const auto byte : header)
        {
            checksum += static_cast<foundation::uint8>(byte);
        }
        header.replace(148, 6, QByteArray::number(checksum, 8).rightJustified(6, '0'));
        header[154] = '\0';
        header[155] = ' ';
        const auto data = input.readAll();
        if (data.size() != input.size())
        {
            return false;
        }
        result.append(header);
        result.append(data);
        result.append(QByteArray((512 - data.size() % 512) % 512, '\0'));
    }
    result.append(QByteArray(1024, '\0'));
    archive = result;
    return true;
}

bool CopyEditorSample(const QString& id, QString& descriptor, const QString& destinationRoot)
{
    if (id != QStringLiteral("cornell-box") && id != QStringLiteral("live-edit-game") &&
        id != QStringLiteral("scripted-game") && id != QStringLiteral("editor-sdk-project"))
    {
        return false;
    }
    const auto source = QStringLiteral(":/samples/") + id;
    const auto base = destinationRoot.isEmpty()
                          ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                .filePath(QStringLiteral("sample-projects"))
                          : destinationRoot;
    QString destination;
    for (foundation::uint32 serial = 1; serial <= 10000; ++serial)
    {
        const auto candidate = QDir(base).filePath(id + QLatin1Char('-') + QString::number(serial));
        if (QFileInfo::exists(candidate) == false)
        {
            destination = candidate;
            break;
        }
    }
    if (destination.isEmpty() || QDir().mkpath(destination) == false)
    {
        return false;
    }
    QDirIterator files(source, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext())
    {
        const auto input = files.next();
        const auto output = QDir(destination).filePath(QDir(source).relativeFilePath(input));
        if (QDir().mkpath(QFileInfo(output).absolutePath()) == false || QFile::copy(input, output) == false ||
            QFile::setPermissions(output, QFileDevice::ReadOwner | QFileDevice::WriteOwner) == false)
        {
            (void)QDir(destination).removeRecursively();
            return false;
        }
    }
    const auto result = QDir(destination).filePath(QStringLiteral("ludus.project.json"));
    if (QFileInfo::exists(result) == false)
    {
        (void)QDir(destination).removeRecursively();
        return false;
    }
#if defined(Q_OS_MACOS)
    QFile original(result);
    if (original.open(QIODevice::ReadOnly) == false)
    {
        (void)QDir(destination).removeRecursively();
        return false;
    }
    auto document = QJsonDocument::fromJson(original.readAll()).object();
    original.close();
    if (document.value(QStringLiteral("version")).toInt() == 2)
    {
        document.insert(QStringLiteral("preset"), QStringLiteral("macos-clang-development"));
        QSaveFile output(result);
        const auto bytes = QJsonDocument(document).toJson();
        if (output.open(QIODevice::WriteOnly) == false || output.write(bytes) != bytes.size() ||
            output.commit() == false)
        {
            (void)QDir(destination).removeRecursively();
            return false;
        }
    }
#endif
    descriptor = result;
    return true;
}

#if defined(Q_OS_WASM)
bool DownloadEditorProject(QWidget* owner, const QString& descriptor)
{
    QByteArray bytes;
    const auto directory = QFileInfo(descriptor).absolutePath();
    if (QFileInfo::exists(descriptor) == false || EncodeEditorProjectArchive(directory, bytes) == false)
    {
        Report(
            owner,
            QStringLiteral(
                "Could not export the project: maximum 4096 regular files and 32 MiB; unsupported paths or symlinks."));
        return false;
    }
    const auto name = (QFileInfo(directory).fileName() + QStringLiteral(".tar")).toUtf8();
    LudusBrowserDownloadDocument(name.constData(),
                                 reinterpret_cast<const foundation::uint8*>(bytes.constData()),
                                 static_cast<foundation::int32>(bytes.size()));
    return true;
}

bool DownloadEditorDocument(QWidget* owner, const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false || file.size() <= 0 || file.size() > MAX_DOCUMENT_BYTES)
    {
        Report(owner, QStringLiteral("Could not read the document for download."));
        return false;
    }
    const auto bytes = file.readAll();
    if (bytes.size() != file.size())
    {
        Report(owner, QStringLiteral("Could not read the complete document for download."));
        return false;
    }
    const auto name = QFileInfo(path).fileName().toUtf8();
    LudusBrowserDownloadDocument(name.constData(),
                                 reinterpret_cast<const foundation::uint8*>(bytes.constData()),
                                 static_cast<foundation::int32>(bytes.size()));
    return true;
}
#endif
} // namespace ludus::editor
