#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include <QHash>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
inline constexpr std::string_view COOKED_CATALOG_PATH = "assets/catalog.json";

/// Poll up to a fixed number of slots at a time, including slots vacated by recooks.
inline constexpr foundation::filesystem::WatchConfig COOKED_CONTENT_WATCH_CONFIG
{
    .MaxPaths = 1024,
    .HintCapacity = 32,
    .DebounceNanoseconds = 50'000'000,
};

struct CookedContentWatchStep final
{
    bool ShouldRefresh = false;
    QString FailedPath;
};

/// Native Content browser observation. Only the catalog worker validates and selects revisions.
class CookedContentWatch final
{
public:
    explicit CookedContentWatch(const foundation::filesystem::WatchConfig& config) noexcept;
    [[nodiscard]] bool Reset(const QString& root);
    /// Sync registrations after accepting a catalog. Returns the first path that could not be watched.
    [[nodiscard]] QString SetTexturePaths(const QStringList& paths);
    [[nodiscard]] CookedContentWatchStep Poll(foundation::uint64 nowNanoseconds);
    [[nodiscard]] bool NeedsRescan() const noexcept;

private:
    foundation::core::UniquePtr<foundation::filesystem::Watcher> mWatcher;
    foundation::filesystem::WatchConfig mConfig;
    QHash<QString, foundation::filesystem::WatchHandle> mTexturePaths;
};
} // namespace ludus::editor
