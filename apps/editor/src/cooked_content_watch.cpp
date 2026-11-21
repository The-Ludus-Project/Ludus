#include "internal/cooked_content_watch.h"

#include <QByteArray>
#include <QSet>

#include <new>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace filesystem = foundation::filesystem;
constexpr usize MAX_STEPS_PER_POLL = 128;

[[nodiscard]] std::string_view GetView(const QByteArray& bytes) noexcept
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}
} // namespace

CookedContentWatch::CookedContentWatch(const filesystem::WatchConfig& config) noexcept : mConfig(config) {}

bool CookedContentWatch::Reset(const QString& root)
{
    mTexturePaths.clear();
    mWatcher.Reset();
    if (root.isEmpty() == true)
    {
        return false;
    }
    foundation::core::UniquePtr<filesystem::Watcher> watcher;
    watcher.Reset(new (std::nothrow) filesystem::Watcher);
    if (watcher.Get() == nullptr || watcher->Init(GetView(root.toUtf8()), mConfig).Succeeded() == false)
    {
        return false;
    }
    filesystem::WatchHandle catalog;
    if (watcher->Add(COOKED_CATALOG_PATH, 0, catalog).Succeeded() == false)
    {
        return false;
    }
    mWatcher.Swap(watcher);
    return true;
}

QString CookedContentWatch::SetTexturePaths(const QStringList& paths)
{
    if (mWatcher.Get() == nullptr || NeedsRescan() == true)
    {
        return {};
    }
    const QSet<QString> desired(paths.begin(), paths.end());
    for (QHash<QString, filesystem::WatchHandle>::iterator it = mTexturePaths.begin(); it != mTexturePaths.end();)
    {
        if (desired.contains(it.key()) == true)
        {
            ++it;
        }
        else
        {
            static_cast<void>(mWatcher->Remove(it.value()));
            it = mTexturePaths.erase(it);
        }
    }
    QString failedPath;
    for (const QString& path : paths)
    {
        if (mTexturePaths.contains(path) == true)
        {
            continue;
        }
        filesystem::WatchHandle handle;
        if (mWatcher->Add(GetView(path.toUtf8()), 0, handle).Succeeded() == true)
        {
            mTexturePaths.insert(path, handle);
        }
        else if (failedPath.isEmpty() == true)
        {
            failedPath = path;
        }
    }
    return failedPath;
}

CookedContentWatchStep CookedContentWatch::Poll(uint64 nowNanoseconds)
{
    if (mWatcher.Get() == nullptr)
    {
        return {};
    }
    const usize steps = qMin(mConfig.MaxPaths, MAX_STEPS_PER_POLL);
    filesystem::WatchHint hint;
    if (mWatcher->NeedsRescan() == true)
    {
        if (mWatcher->IsRescanning() == false && mWatcher->BeginRescan().Succeeded() == false)
        {
            return {};
        }
        CookedContentWatchStep result;
        // One attempt per tick. A failed observation must go through the end of this pass before retrying.
        for (usize step = 0; step <= steps; ++step)
        {
            const filesystem::WatchPollResult observed = mWatcher->NextRescan(hint);
            if (observed.Available == false)
            {
                result.ShouldRefresh = observed.Outcome.Succeeded() == true && mWatcher->NeedsRescan() == false;
                break;
            }
            if (hint.Observation.Succeeded() == false && hint.Observation.Code != filesystem::Status::NotFound &&
                result.FailedPath.isEmpty() == true)
            {
                result.FailedPath = QString::fromUtf8(hint.Path, static_cast<qsizetype>(hint.PathBytes));
            }
        }
        return result;
    }

    static_cast<void>(mWatcher->Advance(nowNanoseconds, steps));
    if (mWatcher->NeedsRescan() == true)
    {
        return {};
    }
    bool shouldRefresh = false;
    while (mWatcher->Poll(hint).Available == true)
    {
        shouldRefresh = true;
    }
    return { .ShouldRefresh = shouldRefresh, .FailedPath = {} };
}

bool CookedContentWatch::NeedsRescan() const noexcept
{
    return mWatcher.Get() != nullptr && mWatcher->NeedsRescan() == true;
}
} // namespace ludus::editor
