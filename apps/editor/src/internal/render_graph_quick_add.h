#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_node_ports.h"
#include "internal/render_graph_template_registry.h"

#include <QFrame>
#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Which template kinds the Quick Add palette lists.
enum class RenderGraphQuickAddKind : foundation::uint8
{
    All,
    Snippets,
    Nodes,
};

/// Limits Quick Add to node templates that can take a wire from one output, as when a wire is dropped
/// on empty canvas. An empty Source means no filter.
struct RenderGraphQuickAddPortFilter final
{
    /// Output endpoint the wire comes from, such as "tonemap.color".
    QString Source;
    /// Type of that output, such as "ColorTexture".
    QString Type;
    /// Template key (GetRenderGraphTemplateKey) to the input that accepts Source, as checked by the
    /// caller with CheckRenderGraphLinkToNewNode. Templates without an entry are not listed.
    QHash<QString, QString> Inputs;
};

/// Stable identity of one catalog entry for recent and pinned lists, such as
/// "node:ToneMap" or "node:Upscale/fsr-2". Survives catalog reordering.
[[nodiscard]] QString GetRenderGraphTemplateKey(const RenderGraphTemplate& entry);

/// Rank templates against a search query, best first. Every word of the query must match the
/// label, identity, keywords, category name or description; label word prefixes rank highest.
/// An empty query keeps every template, grouped by category in catalog order.
[[nodiscard]] QVector<RenderGraphTemplate> SearchRenderGraphTemplates(const QString& query,
                                                                      const QVector<RenderGraphTemplate>& templates);

/// Search-first popup for adding snippets and nodes to the canvas. It shows at most
/// MAX_ROWS results; anything further is summarized as "+N more" so the list never scrolls.
/// With an empty query it opens on recent and pinned templates. With a port filter it lists only
/// nodes that accept that output, under a context line that can clear the filter. Selecting a
/// template emits InsertRequested; the palette never edits a document itself. GUI thread only.
/// See docs/development/render-graph-authoring.md#add-nodes-with-quick-add.
class RenderGraphQuickAdd final : public QFrame
{
    Q_OBJECT
public:
    /// Result rows shown at once, not counting section headings.
    static constexpr foundation::int32 MAX_ROWS = 8;
    /// Recent templates remembered, most recent first.
    static constexpr foundation::int32 MAX_RECENT = 3;

    /// Reads snippets, nodes and graph names from registry, which must outlive the palette.
    explicit RenderGraphQuickAdd(const RenderGraphTemplateRegistry& registry, QWidget* parentOrNull = nullptr);

    /// Reset the query, kind and category, set the port filter (empty for none), then show the
    /// palette near globalPosition with the search field focused. The palette stays inside the
    /// screen that contains the position. A filter opens on every compatible node, not Suggested.
    void Open(const QPoint& globalPosition, const RenderGraphQuickAddPortFilter& filter = {});
    /// Current port filter; its Source is empty when there is none.
    [[nodiscard]] const RenderGraphQuickAddPortFilter& GetPortFilter() const noexcept;
    /// Drop the port filter and list every template again, as the context line's clear button and
    /// Backspace in an empty search do. A later insert is no longer connected.
    void ClearPortFilter();

    /// Ports shown for node templates, keyed by GetRenderGraphTemplateKey(); entries without
    /// ports show none.
    void SetNodePorts(const QHash<QString, RenderGraphNodePorts>& ports);
    /// Why a requirement cannot preview in this editor, keyed by requirement identity. A
    /// missing or empty message means the requirement is available.
    void SetUnavailableRequirements(const QHash<QString, QString>& messages);
    /// Recent template keys, most recent first; only the first MAX_RECENT are kept.
    /// Unknown keys are kept but not shown.
    void SetRecent(const QStringList& keys);
    /// Pinned template keys in pin order; unknown keys are kept but not shown.
    void SetPinned(const QStringList& keys);
    [[nodiscard]] const QStringList& GetRecent() const noexcept;
    [[nodiscard]] const QStringList& GetPinned() const noexcept;

    /// Current filters, for tests and accessibility descriptions.
    [[nodiscard]] RenderGraphQuickAddKind GetKind() const noexcept;
    void SetKind(RenderGraphQuickAddKind kind);
    /// "suggested", "all" or a category identity.
    [[nodiscard]] const QString& GetCategory() const noexcept;
    void SetCategory(const QString& category);
    /// Keys of the listed result rows, in order; never more than MAX_ROWS.
    [[nodiscard]] QStringList GetListedKeys() const;
    /// Matches beyond MAX_ROWS in the current category.
    [[nodiscard]] foundation::int32 GetHiddenCount() const noexcept;
    /// Key of the highlighted row; empty when nothing is listed.
    [[nodiscard]] QString GetHighlightedKey() const;
    /// Emit InsertRequested for the highlighted row and close, as Enter does. False when empty.
    [[nodiscard]] bool InsertHighlighted();
    /// Add or remove a template key from the pinned list and emit HistoryChanged.
    void TogglePinned(const QString& key);

Q_SIGNALS:
    /// A node to insert where the palette opened, or a snippet to apply. The palette has closed.
    /// With a port filter, input is the node's input to connect from the filter's Source; it is
    /// empty otherwise. Snippets are never offered while filtered.
    void InsertRequested(const ludus::editor::RenderGraphTemplate& entry, const QString& input);
    /// The query matches complete graphs; open New Render Graph with this template selected.
    void NewGraphRequested(const QString& templateIdentity);
    /// Recent or pinned keys changed and may be saved.
    void HistoryChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refresh();
    void renderDetail();
    void moveHighlight(foundation::int32 delta);
    void cycleKind(bool isForward);
    [[nodiscard]] QVector<RenderGraphTemplate> getMatches() const;
    [[nodiscard]] QString getUnavailableMessage(const RenderGraphTemplate& entry) const;
    [[nodiscard]] QString describePorts(const RenderGraphTemplate& entry) const;
    [[nodiscard]] bool hasPortFilter() const noexcept;
    [[nodiscard]] QString getDefaultCategory() const;

    QVector<RenderGraphTemplate> mTemplates;
    QVector<RenderGraphTemplate> mGraphs;
    QHash<QString, RenderGraphNodePorts> mNodePorts;
    QHash<QString, QString> mUnavailableRequirements;
    RenderGraphQuickAddPortFilter mPortFilter;
    QStringList mRecent;
    QStringList mPinned;
    RenderGraphQuickAddKind mKind = RenderGraphQuickAddKind::All;
    QString mCategory;
    QVector<RenderGraphTemplate> mListed;
    foundation::int32 mHighlight = 0;
    foundation::int32 mHiddenCount = 0;
    QString mGraphMatch;

    QLineEdit* mSearch;
    QWidget* mContext;
    QLabel* mContextLabel;
    QToolButton* mContextClear;
    QToolButton* mKindAll;
    QToolButton* mKindSnippets;
    QToolButton* mKindNodes;
    QTreeWidget* mCategories;
    QTreeWidget* mResults;
    QLabel* mMore;
    QPushButton* mGraphHint;
    QLabel* mTitle;
    QLabel* mDescription;
    QLabel* mPorts;
    QLabel* mWarning;
    QPushButton* mInsert;
    QPushButton* mPin;
};
} // namespace ludus::editor
