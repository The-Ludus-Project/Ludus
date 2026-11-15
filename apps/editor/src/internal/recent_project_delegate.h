#pragma once

// The Start page's recent project rows: the project name leads in bold and its descriptor path follows on a quieter
// line, elided from the middle so the file name stays whole. A project whose file is gone is dimmed and carries a
// Missing badge; the first such row also draws the quiet heading of the Missing section the list groups them under.

#include <QStyledItemDelegate>

namespace ludus::editor
{
/// Item data role holding the project name drawn on a recent project row.
inline constexpr int RECENT_PROJECT_NAME_ROLE = Qt::UserRole + 80;
/// Item data role that is true for a recent project whose descriptor file is missing.
inline constexpr int RECENT_PROJECT_IS_MISSING_ROLE = Qt::UserRole + 81;
/// Item data role that is true for the first missing row, which draws the Missing section heading above itself.
inline constexpr int RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE = Qt::UserRole + 82;

/// Name-over-path rows for the recent projects list. GUI thread only.
class RecentProjectDelegate final : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit RecentProjectDelegate(QObject* parentOrNull = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

/// The heading drawn above the first missing recent project.
[[nodiscard]] QString GetMissingRecentProjectsHeading();
} // namespace ludus::editor
