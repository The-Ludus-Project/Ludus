#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_history.h"

#include <QByteArray>
#include <QDialog>
#include <QSize>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QCheckBox;
class QCloseEvent;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphDocument;
/// Copied endpoint and exact caller path; contains no canvas items or GPU ownership.
struct RenderGraphResourceRequest final
{
    QString File;
    QString Configuration;
    QString Endpoint;
    bool IsOutput = true;
    QVector<RenderGraphHistoryLocation> Path;
};
/// Compare caller file/configuration/instance identities; saved pan/zoom/selection do not affect scope.
[[nodiscard]] bool AreRenderGraphResourcePathsEqual(const QVector<RenderGraphHistoryLocation>& left,
                                                    const QVector<RenderGraphHistoryLocation>& right);
/// Read-only draft resolution. ResourceName is empty when the root instance cannot be established.
struct RenderGraphResourceDetails final
{
    QString Type;
    QString Producer;
    QStringList Consumers;
    QString ResourceName;
    QString Description;
    QString Problem;
};
/// Resolve an existing port through bounded caller bindings and writable targets. Never guesses
/// a library instance by suffix; malformed, ambiguous or cyclic connections remain unavailable.
[[nodiscard]] RenderGraphResourceDetails ResolveRenderGraphResourceInspection(const RenderGraphDocument& document,
                                                                              const RenderGraphResourceRequest& request,
                                                                              const QSize& referenceSize);
/// One copied catalogue entry, described by the existing texture-viewer metadata owner.
struct RenderGraphResourcePreviewTarget final
{
    QString Name;
    QString Description;
    bool CanOpen = false;
};
/// Metadata from the existing preview, with no GPU allocations, images or readback commands.
/// InputIdentity/Configuration describe the admitted build that produced this active generation;
/// empty identity means unverified. Active and presented serials must agree before opening Textures.
struct RenderGraphResourcePreviewSnapshot final
{
    QVector<RenderGraphResourcePreviewTarget> Targets;
    QByteArray InputIdentity;
    QString Configuration;
    QString RootFile;
    QString Problem;
    foundation::uint64 ActiveGeneration = 0;
    foundation::uint64 PresentedGeneration = 0;
    bool IsAttached = false;
    bool HasNames = false;
    bool HasPresentedCatalogue = false; ///< Includes the preview's device session and drawable-extent check.
};
/// Modeless canvas inspector. Owns copied requests/metadata, never document history or GPU state.
/// Explicit Inspect replaces its endpoint; unpinned hover follows other resources. Pin freezes
/// endpoint, caller path and resource identity. GUI thread only; document must outlive this widget.
class RenderGraphResourceInspector final : public QDialog
{
    Q_OBJECT
public:
    RenderGraphResourceInspector(const RenderGraphDocument& document, QWidget& inoutParent);
    /// Show an explicit request without selecting nodes or changing dependency tracing.
    void Inspect(const RenderGraphResourceRequest& request);
    /// Follow hover only while visible and unpinned; repeated requests are ignored.
    void Follow(const RenderGraphResourceRequest& request);
    /// Refresh copied generation information supplied by the existing native preview bridge.
    void SetPreviewSnapshot(const RenderGraphResourcePreviewSnapshot& snapshot);
    /// Re-evaluate draft details and freshness after document edits, history or a drawable resize.
    void Refresh();
    /// Set the drawable reference used by existing draft resource inference; no GPU work.
    void SetReferenceSize(const QSize& size);
    /// Clear package-local requests and hide after a project/package is replaced.
    void Reset();
    /// Pin/unpin the current endpoint and resource identity; pin survives graph navigation.
    void SetPinned(bool isPinned);
    /// Whether passive canvas hover is suspended for the current resource.
    [[nodiscard]] bool IsPinned() const;
    /// Current presentation for accessibility and regression checks.
    [[nodiscard]] QString GetDetailsText() const;
    /// Freshness/unavailability explanation, independent of the generation image.
    [[nodiscard]] QString GetStatusText() const;
    /// True only for a named image in the attached, presented generation.
    [[nodiscard]] bool CanOpenInTextures() const;
Q_SIGNALS:
    /// Supply a current preview snapshot before refreshing this visible request.
    void PreviewRequested();
    /// Workspace must revalidate the generation and exact name before using its existing viewer.
    void OpenInTexturesRequested(const QString& name, foundation::uint64 generation);

protected:
    void closeEvent(QCloseEvent* event) override;
    void done(foundation::int32 result) override;

private:
    const RenderGraphDocument& mDocument;
    RenderGraphResourceRequest mRequest;
    RenderGraphResourcePreviewSnapshot mPreview;
    QSize mReferenceSize;
    QString mPinnedResource;
    QString mOpenName;
    QByteArray mDraftIdentity;
    QString mIdentityConfiguration;
    RenderGraphResourceDetails mResource;
    foundation::uint64 mResourceRevision = 0;
    foundation::uint64 mIdentityRevision = 0;
    bool mShouldResolve = true;
    QLabel* mDetails;
    QLabel* mStatus;
    QCheckBox* mPin;
    QPushButton* mOpen;
};
} // namespace ludus::editor
