// Browser editor: open a folder from disk in place. The JavaScript side
// (apps/editor/browser/host_folder.js) picks and lists the folder and mounts it
// into the session file system; this adapter admits the listing, reports
// failures in the editor and hands the descriptor or scene entry to the caller.
// See docs/architecture/editor-browser-strategy.md (host project folders).

#include "internal/browser_folder.h"
#include "internal/editor_files.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMessageBox>
#include <QPointer>
#include <QSharedPointer>
#include <QWidget>

#include <string>

#include <QtCore/private/qstdweb_p.h>
#include <emscripten/val.h>
#include <unistd.h>

extern "C" void LudusHostPickFolder(emscripten::EM_VAL target, ludus::foundation::int32 intent, const char* options);
extern "C" void LudusHostReopenFolder(emscripten::EM_VAL target, const char* options);
extern "C" ludus::foundation::int32 LudusHostMountSelection(ludus::foundation::int32 selection,
                                                            const char* mountPoint,
                                                            ludus::foundation::float64 budgetBytes,
                                                            const char* options);
extern "C" void LudusHostDiscardSelection(ludus::foundation::int32 selection);
extern "C" void LudusHostSetMonitor(emscripten::EM_VAL target);
extern "C" ludus::foundation::int32 LudusHostIsWritable(const char* path);

namespace ludus::editor
{
namespace
{
/// Session file-system directory under which picked folders are mounted, one numbered mount each.
constexpr QStringView FOLDER_MOUNT_ROOT = u"/browser-folders/";

foundation::uint32 FolderSerial = 0;

struct FolderRequest
{
    QPointer<QWidget> Owner;
    BrowserFolderIntent Intent = BrowserFolderIntent::Project;
    std::function<void(const QString&)> Completed;
    emscripten::val Target = emscripten::val::undefined();
    QSharedPointer<qstdweb::EventCallback> Picked;
    QSharedPointer<qstdweb::EventCallback> Failed;
    QSharedPointer<qstdweb::EventCallback> Cancelled;
};
QSharedPointer<FolderRequest> PendingFolder;

/// Receives read and write-back failures of mounted folders for the whole session.
struct FolderMonitor
{
    QPointer<QWidget> Owner;
    emscripten::val Target = emscripten::val::undefined();
    QSharedPointer<qstdweb::EventCallback> ReadFailed;
    QSharedPointer<qstdweb::EventCallback> WriteFailed;
    QPointer<QMessageBox> WriteWarning;
};
FolderMonitor* MonitorOrNull = nullptr;

QMessageBox* ShowMessage(QWidget* owner, QMessageBox::Icon icon, const QString& message)
{
    QMessageBox* dialog = new QMessageBox(icon, QStringLiteral("Browser folders"), message, QMessageBox::Ok, owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
    return dialog;
}

QByteArray EncodeOptions()
{
    QJsonArray skip;
    for (const QString& name : GetGeneratedProjectFolderNames())
    {
        skip.append(name);
    }
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("skip"), skip},
                             {QStringLiteral("maximumEntries"), static_cast<qint64>(MAX_BROWSER_FOLDER_FILES)},
                         })
        .toJson(QJsonDocument::Compact);
}

QString ReadString(const emscripten::val& object, const char* key)
{
    const emscripten::val value = object[key];
    return value.isString() == true ? QString::fromStdString(value.as<std::string>()) : QString();
}

BrowserFolderFailure ReadFailure(const emscripten::val& detail, const QString& kind)
{
    BrowserFolderFailure failure;
    failure.Kind = kind.isEmpty() == true ? ReadString(detail, "failure") : kind;
    failure.Name = ReadString(detail, "name");
    failure.Path = ReadString(detail, "path");
    failure.Error = ReadString(detail, "error");
    const emscripten::val count = detail["count"];
    failure.Count = count.isNumber() == true ? static_cast<foundation::uint64>(count.as<foundation::float64>()) : 0;
    return failure;
}

void EnsureMonitor(QWidget* owner)
{
    QWidget* window = owner != nullptr ? owner->window() : nullptr;
    if (MonitorOrNull != nullptr)
    {
        MonitorOrNull->Owner = window;
        return;
    }
    MonitorOrNull = new FolderMonitor;
    MonitorOrNull->Owner = window;
    MonitorOrNull->Target = emscripten::val::global("EventTarget").new_();
    const auto report = [](const QString& kind) {
        return [kind](const emscripten::val& event) {
            if (MonitorOrNull != nullptr && MonitorOrNull->Owner.isNull() == false)
            {
                const QString message =
                    DescribeBrowserFolderFailure(ReadFailure(event["detail"], kind), BrowserFolderIntent::Project);
                const bool isWrite = kind == QStringLiteral("write");
                if (isWrite == true && MonitorOrNull->WriteWarning.isNull() == false &&
                    MonitorOrNull->WriteWarning->text() == message)
                {
                    return;
                }
                QMessageBox* dialog = ShowMessage(MonitorOrNull->Owner, QMessageBox::Warning, message);
                if (isWrite == true)
                {
                    MonitorOrNull->WriteWarning = dialog;
                }
            }
        };
    };
    MonitorOrNull->ReadFailed = QSharedPointer<qstdweb::EventCallback>::create(MonitorOrNull->Target,
                                                                               "ludushostreadfailed",
                                                                               report(QStringLiteral("read")));
    MonitorOrNull->WriteFailed = QSharedPointer<qstdweb::EventCallback>::create(MonitorOrNull->Target,
                                                                                "ludushostwritefailed",
                                                                                report(QStringLiteral("write")));
    LudusHostSetMonitor(MonitorOrNull->Target.as_handle());
}

void FinishFolderRequest(const QSharedPointer<FolderRequest>& request)
{
    if (PendingFolder == request)
    {
        PendingFolder.clear();
    }
}

void AcceptPickedFolder(const QSharedPointer<FolderRequest>& request, const emscripten::val& detail)
{
    const foundation::int32 selection = detail["selection"].as<foundation::int32>();
    const QString name = ReadString(detail, "name");
    const bool isWritable = detail["writable"].as<bool>();
    const emscripten::val list = detail["entries"];
    const foundation::uint32 count = list["length"].as<foundation::uint32>();
    QList<BrowserFolderEntry> entries;
    entries.reserve(static_cast<qsizetype>(count));
    for (foundation::uint32 index = 0; index < count; ++index)
    {
        const emscripten::val item = list[index];
        entries.append(
        {
            .Path = ReadString(item, "path"),
            .Size = static_cast<foundation::uint64>(item["size"].as<foundation::float64>()),
        });
    }
    const BrowserFolderAdmission admission = AdmitBrowserFolder(name, entries, request->Intent, {});
    if (admission.IsAdmitted() == false)
    {
        LudusHostDiscardSelection(selection);
        ShowMessage(request->Owner, QMessageBox::Warning, admission.Problem);
        return;
    }
    const QString mountPoint = FOLDER_MOUNT_ROOT.toString() + QString::number(++FolderSerial);
    EnsureMonitor(request->Owner);
    const QByteArray mountPath = mountPoint.toUtf8();
    const QByteArray options = EncodeOptions();
    const foundation::int32 error = LudusHostMountSelection(selection,
                                                            mountPath.constData(),
                                                            static_cast<foundation::float64>(BROWSER_FILE_BUDGET_BYTES),
                                                            options.constData());
    if (error != 0)
    {
        ShowMessage(request->Owner,
                    QMessageBox::Warning,
                    QStringLiteral("Could not prepare %1 in the browser's session file system (error %2). Reload the "
                                   "page and open the folder again.")
                        .arg(name)
                        .arg(error));
        return;
    }
    if (request->Intent == BrowserFolderIntent::Project && isWritable == false)
    {
        ShowMessage(request->Owner,
                    QMessageBox::Information,
                    QStringLiteral("%1 is open read-only: this browser cannot write to folders on disk. Edits stay in "
                                   "this tab; use File → Download Project to save them, or open the folder in Chrome "
                                   "or Edge to save in place.")
                        .arg(name));
    }
    request->Completed(mountPoint + QLatin1Char('/') + admission.Entry);
}

/// Prepare the request's event callbacks; `start` asks the JavaScript side to pick or reopen.
void StartFolderRequest(QWidget* owner,
                        BrowserFolderIntent intent,
                        const std::function<void(const QString&)>& loaded,
                        const std::function<void(emscripten::EM_VAL, const char*)>& start)
{
    if (PendingFolder)
    {
        return;
    }
    QSharedPointer<FolderRequest> request = QSharedPointer<FolderRequest>::create();
    PendingFolder = request;
    request->Owner = owner;
    request->Intent = intent;
    request->Completed = loaded;
    request->Target = emscripten::val::global("EventTarget").new_();
    const QWeakPointer<FolderRequest> weak(request);
    // Thanks to Qt Group, qstdweb::EventCallback (Qt 6.10.3): events dispatched from
    // JavaScript promises reach the editor through Qt's suspend/resume dispatcher,
    // never as a nested Wasm call (the same adapter as the document imports).
    request->Picked = QSharedPointer<qstdweb::EventCallback>::create(
        request->Target,
        "ludushostpicked",
        [weak](const emscripten::val& event) {
            const QSharedPointer<FolderRequest> request = weak.toStrongRef();
            if (request.isNull() == true)
            {
                return;
            }
            FinishFolderRequest(request);
            if (request->Owner.isNull() == true)
            {
                LudusHostDiscardSelection(event["detail"]["selection"].as<foundation::int32>());
                return;
            }
            AcceptPickedFolder(request, event["detail"]);
        });
    request->Failed = QSharedPointer<qstdweb::EventCallback>::create(
        request->Target,
        "ludushostfailed",
        [weak](const emscripten::val& event) {
            const QSharedPointer<FolderRequest> request = weak.toStrongRef();
            if (request.isNull() == true)
            {
                return;
            }
            FinishFolderRequest(request);
            if (request->Owner.isNull() == false)
            {
                ShowMessage(request->Owner,
                            QMessageBox::Warning,
                            DescribeBrowserFolderFailure(ReadFailure(event["detail"], QString()), request->Intent));
            }
        });
    request->Cancelled =
        QSharedPointer<qstdweb::EventCallback>::create(request->Target,
                                                       "ludushostcancel",
                                                       [weak](const emscripten::val&) {
                                                           const QSharedPointer<FolderRequest> request =
                                                               weak.toStrongRef();
                                                           if (request.isNull() == false)
                                                           {
                                                               FinishFolderRequest(request);
                                                           }
                                                       });
    const QByteArray options = EncodeOptions();
    start(request->Target.as_handle(), options.constData());
}
} // namespace

void OpenBrowserFolder(QWidget* owner, BrowserFolderIntent intent, const std::function<void(const QString&)>& loaded)
{
    StartFolderRequest(owner, intent, loaded, [intent](emscripten::EM_VAL target, const char* options) {
        LudusHostPickFolder(target, intent == BrowserFolderIntent::Project ? 0 : 1, options);
    });
}

bool IsBrowserFolderWritable(const QString& path)
{
    const QByteArray bytes = path.toUtf8();
    return path.isEmpty() == false && LudusHostIsWritable(bytes.constData()) != 0;
}

bool FlushBrowserFolderWrites(const QString& path)
{
    if (IsBrowserFolderWritable(path) == false)
    {
        return false;
    }
    QFile file(path);
    // Emscripten's fd_sync waits for the mounted folder's existing write queue.
    return file.open(QIODevice::ReadOnly) == true && fsync(file.handle()) == 0;
}

void ReopenBrowserProjectFolder(QWidget* owner, const std::function<void(const QString&)>& loaded)
{
    StartFolderRequest(owner, BrowserFolderIntent::Project, loaded, [](emscripten::EM_VAL target, const char* options) {
        LudusHostReopenFolder(target, options);
    });
}
} // namespace ludus::editor
