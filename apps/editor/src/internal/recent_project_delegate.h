#pragma once

// The Start page's recent project rows: the project name leads in bold and its descriptor path follows on a quieter
// line, elided from the middle so the file name stays whole. A project whose file is gone is dimmed and carries a
// Missing badge; the first such row also draws the quiet heading of the Missing section the list groups them under.
// Projects that share a name (worktrees of one game) follow it with a dimmed folder, " · milestone-2", that tells
// them apart; see GetRecentProjectQualifiers.

#include <QStringList>
#include <QStyledItemDelegate>

namespace ludus::editor
{
/// Item data role holding the project name drawn on a recent project row.
inline constexpr int RECENT_PROJECT_NAME_ROLE = Qt::UserRole + 80;
/// Item data role that is true for a recent project whose descriptor file is missing.
inline constexpr int RECENT_PROJECT_IS_MISSING_ROLE = Qt::UserRole + 81;
/// Item data role that is true for the first missing row, which draws the Missing section heading above itself.
inline constexpr int RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE = Qt::UserRole + 82;
/// Item data role holding the dimmed folder drawn after a name that other recent projects share (empty for none).
inline constexpr int RECENT_PROJECT_QUALIFIER_ROLE = Qt::UserRole + 83;

/// Name-over-path rows for the recent projects list. GUI thread only.
class RecentProjectDelegate final : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit RecentProjectDelegate(QObject* parentOrNull = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

/// For each recent project (names and descriptorPaths in the same order), the folder that tells it apart from the
/// others with its name, compared without case: empty when the name is unique; else the one folder that differs
/// within its group, found by walking up from the project folder ("milestone-2" for .../milestone-2/Samples/
/// Dolomites); else, when no single folder does, the fewest trailing folders that differ ("repro/Sample").
[[nodiscard]] QStringList GetRecentProjectQualifiers(const QStringList& names, const QStringList& descriptorPaths);

/// The heading drawn above the first missing recent project.
[[nodiscard]] QString GetMissingRecentProjectsHeading();
} // namespace ludus::editor
