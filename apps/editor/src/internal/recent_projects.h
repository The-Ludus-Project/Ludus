#pragma once

// Local editor preferences, independent of project descriptors. Only successful
// opens are recorded; a persistence failure keeps the in-memory history usable.

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

namespace ludus::editor
{

struct RecentProject
{
    QString DescriptorPath;
    QString Name;
};

class RecentProjectStore
{
public:
    // An explicit path isolates tests; the default lives in the user config dir.
    explicit RecentProjectStore(const QString& path = {});
    [[nodiscard]] bool Load() noexcept;
    [[nodiscard]] bool Remember(RecentProject project) noexcept;
    [[nodiscard]] bool Clear() noexcept;
    /// Forget one project's entry, leaving the project itself on disk. Removing an absent path succeeds.
    [[nodiscard]] bool Remove(const QString& descriptorPath) noexcept;
    [[nodiscard]] const QList<RecentProject>& Entries() const noexcept
    {
        return Entries_;
    }

    static constexpr foundation::int32 MaxEntries = 10;

private:
    [[nodiscard]] bool Save() const noexcept;

    QString Path_;
    QList<RecentProject> Entries_;
};

} // namespace ludus::editor
