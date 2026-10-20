#include "internal/content_browser.h"
#include "internal/content_import.h"

#include <QByteArray>

#if defined(Q_OS_WASM)
#    define LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS
#endif
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
#    include <QThread>
#endif

#include <atomic>
#include <new>

namespace ludus::editor
{
namespace
{
using namespace foundation;
struct JobState final
{
    ContentImportGate Gate;
    std::atomic<bool> Done{false};
    ContentResult Result;
};
void Run(JobState& state)
{
    auto& result = state.Result;
    const auto& request = result.Request;
    if (request.Id.isEmpty() == false)
    {
        const auto imported = ImportAudioSource({ .Root = request.Root, .File = request.File, .Id = request.Id },
                                                state.Gate,
                                                request.CheckCatalog ? &request.CatalogDigest : nullptr);
        result.Status = imported.Status;
        result.Message = imported.Message;
        result.CandidatePath = imported.CandidatePath;
    }
    else if (state.Gate.Cancelled())
    {
        result.Status = content::Status::Cancelled;
        result.Message = QStringLiteral("Catalog refresh cancelled.");
    }
    else
    {
        content::Bytes bytes;
        content::Catalog catalog;
        content::Diagnostic diagnostic;
        const auto root = request.Root.toUtf8();
        result.Status = content::ReadFile({root.constData(), static_cast<usize>(root.size())},
                                          "catalog.json",
                                          content::MAX_DOCUMENT_BYTES,
                                          bytes);
        if (result.Status == content::Status::Ok)
        {
            result.Status = catalog.Read(bytes.String(), diagnostic);
        }
        if (state.Gate.Cancelled())
        {
            result.Status = content::Status::Cancelled;
        }
        if (result.Status == content::Status::Ok)
        {
            result.CatalogDigest = content::Hash(bytes.Data());
            result.Rows.reserve(static_cast<qsizetype>(catalog.Entries().size()));
            for (const auto& entry : catalog.Entries())
            {
                result.Rows.append({QString::fromUtf8(entry.Id.Data, static_cast<qsizetype>(entry.Id.Length)),
                                    QString::fromUtf8(entry.Path.Data, static_cast<qsizetype>(entry.Path.Length)),
                                    entry.Type});
            }
            result.Message =
                QStringLiteral("%1 resources · select an ID to inspect or reimport.").arg(result.Rows.size());
        }
        else
        {
            result.Message =
                result.Status == content::Status::NotFound
                    ? QStringLiteral("No catalog yet. Import WAV or FLAC to create one.")
                : result.Status == content::Status::Cancelled
                    ? QStringLiteral("Catalog refresh cancelled; last valid list retained.")
                    : QStringLiteral("Catalog refresh failed; last valid list retained. Check content/catalog.json.");
        }
    }
    state.Done.store(true, std::memory_order_release);
}
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
// Thanks to Qt Group, QThread, "Managing Threads": keep worker execution
// separate from QObject affinity. This adapter owns the copied state in the
// thread subclass rather than the previous create()/shared-deleter closure.
// https://doc.qt.io/qt-6/qthread.html
class ContentWorker final : public QThread
{
public:
    JobState State;

private:
    void run() override
    {
        Run(State);
    }
};
#endif
} // namespace

struct ContentJobs::Impl final
{
    JobState* State = nullptr;
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
    ContentWorker* Thread = nullptr;
#endif
};
ContentJobs::ContentJobs(QObject* parent) : QObject(parent), Impl_(new (std::nothrow) Impl)
{
    PollTimer_.setInterval(20);
    connect(&PollTimer_, &QTimer::timeout, this, &ContentJobs::Poll);
}
ContentJobs::~ContentJobs()
{
    Shutdown();
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
    if (Impl_ != nullptr && Impl_->Thread != nullptr)
    {
        if (Impl_->Thread->isFinished() == false)
        {
            connect(Impl_->Thread, &QThread::finished, Impl_->Thread, &QObject::deleteLater);
        }
        if (Impl_->Thread->isFinished())
        {
            delete Impl_->Thread;
        }
    }
#else
    if (Impl_ != nullptr)
    {
        delete Impl_->State;
    }
#endif
    delete Impl_;
}
bool ContentJobs::Start(const ContentRequest& request)
{
    if (Impl_ == nullptr || Closing_ || Busy())
    {
        return false;
    }
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
    auto* thread = new (std::nothrow) ContentWorker;
    if (thread == nullptr)
    {
        return false;
    }
    // The worker owns its copied request and result until it finishes. The UI
    // borrows that state while polling; early UI destruction cancels and retires
    // the worker through finished/deleteLater without joining or shared deleters.
    // No worker callback captures this QObject or touches a model.
    auto* state = &thread->State;
    state->Result.Request = request;
    Impl_->State = state;
    Impl_->Thread = thread;
    thread->start();
    if (thread->isRunning() == false && state->Done.load(std::memory_order_acquire) == false)
    {
        delete thread;
        Impl_->Thread = nullptr;
        Impl_->State = nullptr;
        return false;
    }
#else
    auto* state = new (std::nothrow) JobState;
    if (state == nullptr)
    {
        return false;
    }
    state->Result.Request = request;
    Impl_->State = state;
    // Yield once so progress can paint and cancellation/project teardown can
    // revoke admission. Run is synchronous and bounded by the existing catalog
    // and encoded-source limits; it never re-enters the UI during publication.
    QTimer::singleShot(0, this, [this, state]() {
        if (Closing_ == true)
        {
            (void)state->Gate.Cancel();
        }
        Run(*state);
    });
#endif
    PollTimer_.start();
    Q_EMIT BusyChanged();
    return true;
}
bool ContentJobs::Cancel()
{
    return Impl_ != nullptr && Impl_->State != nullptr && Impl_->State->Done.load(std::memory_order_acquire) == false &&
           Impl_->State->Gate.Cancel();
}
void ContentJobs::Shutdown()
{
    Closing_ = true;
    (void)Cancel();
}
bool ContentJobs::Busy() const noexcept
{
    return Impl_ != nullptr && Impl_->State != nullptr;
}
bool ContentJobs::Importing() const noexcept
{
    return Busy() && Impl_->State->Result.Request.Id.isEmpty() == false;
}
bool ContentJobs::Finished() const noexcept
{
    return Busy() == false;
}
void ContentJobs::Poll()
{
    if (Busy() == false || Impl_->State->Done.load(std::memory_order_acquire) == false)
    {
        return;
    }
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
    if (Impl_->Thread != nullptr && Impl_->Thread->isFinished() == false)
    {
        return;
    }
#endif
    const auto result = Impl_->State->Result;
#if !defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
    delete Impl_->Thread;
    Impl_->Thread = nullptr;
#else
    delete Impl_->State;
#endif
    Impl_->State = nullptr;
    PollTimer_.stop();
    Q_EMIT Completed(result);
    Q_EMIT BusyChanged();
}
} // namespace ludus::editor
