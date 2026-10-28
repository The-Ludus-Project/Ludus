#include "internal/render_graph_resource_inspection.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_live_build.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 INSPECTOR_WIDTH = 440;
constexpr int32 INSPECTOR_HEIGHT = 460;
bool AreRequestsEqual(const RenderGraphResourceRequest& left, const RenderGraphResourceRequest& right)
{
    if (left.File != right.File || left.Configuration != right.Configuration || left.Endpoint != right.Endpoint ||
        left.IsOutput != right.IsOutput || left.Path.size() != right.Path.size())
    {
        return false;
    }
    return AreRenderGraphResourcePathsEqual(left.Path, right.Path);
}
} // namespace
RenderGraphResourceInspector::RenderGraphResourceInspector(const RenderGraphDocument& document, QWidget& inoutParent)
    : QDialog(&inoutParent, Qt::Tool), mDocument(document), mDetails(new QLabel(this)), mStatus(new QLabel(this)),
      mPin(new QCheckBox(QStringLiteral("Pin resource"), this)),
      mOpen(new QPushButton(QStringLiteral("Open in Textures"), this))
{
    setObjectName(QStringLiteral("renderGraphResourceInspector"));
    setWindowTitle(QStringLiteral("Inspect Resource"));
    resize(INSPECTOR_WIDTH, INSPECTOR_HEIGHT);
    mDetails->setObjectName(QStringLiteral("renderGraphResourceDetails"));
    mStatus->setObjectName(QStringLiteral("renderGraphResourceStatus"));
    mPin->setObjectName(QStringLiteral("renderGraphResourcePin"));
    mOpen->setObjectName(QStringLiteral("renderGraphOpenInTextures"));
    for (QLabel* label : {mDetails, mStatus})
    {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    mDetails->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(mDetails);
    QPushButton* close = new QPushButton(QStringLiteral("Close"), this);
    QHBoxLayout* actions = new QHBoxLayout;
    actions->addWidget(mPin);
    actions->addStretch();
    actions->addWidget(close);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(scroll, 1);
    layout->addWidget(mStatus);
    layout->addWidget(mOpen);
    layout->addLayout(actions);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(mPin, &QCheckBox::toggled, this, [this](bool isPinned) {
        mPinnedResource = isPinned == true
                              ? ResolveRenderGraphResourceInspection(mDocument, mRequest, mReferenceSize).ResourceName
                              : QString();
        Refresh();
    });
    connect(mOpen, &QPushButton::clicked, this, [this]() {
        Q_EMIT PreviewRequested();
        Refresh();
        if (mOpen->isEnabled() == true)
        {
            Q_EMIT OpenInTexturesRequested(mOpenName, mPreview.PresentedGeneration);
        }
    });
    Refresh();
}
void RenderGraphResourceInspector::Inspect(const RenderGraphResourceRequest& request)
{
    mRequest = request;
    mShouldResolve = true;
    mPinnedResource = IsPinned() == true
                          ? ResolveRenderGraphResourceInspection(mDocument, request, mReferenceSize).ResourceName
                          : QString();
    show();
    Q_EMIT PreviewRequested();
    Refresh();
    raise();
}
void RenderGraphResourceInspector::Follow(const RenderGraphResourceRequest& request)
{
    if (isVisible() == true && IsPinned() == false && AreRequestsEqual(mRequest, request) == false)
    {
        mRequest = request;
        mShouldResolve = true;
        Refresh();
    }
}
void RenderGraphResourceInspector::SetPreviewSnapshot(const RenderGraphResourcePreviewSnapshot& snapshot)
{
    mPreview = snapshot;
    Refresh();
}
void RenderGraphResourceInspector::SetReferenceSize(const QSize& size)
{
    if (size != mReferenceSize)
    {
        mReferenceSize = size;
        mShouldResolve = true;
        Refresh();
    }
}
void RenderGraphResourceInspector::Reset()
{
    hide();
    mRequest = {};
    mShouldResolve = true;
    mIdentityRevision = 0;
    mIdentityConfiguration.clear();
    mPreview = {};
    SetPinned(false);
    Refresh();
}
void RenderGraphResourceInspector::SetPinned(bool isPinned)
{
    mPin->setChecked(isPinned);
}
bool RenderGraphResourceInspector::IsPinned() const
{
    return mPin->isChecked();
}
QString RenderGraphResourceInspector::GetDetailsText() const
{
    return mDetails->text();
}
QString RenderGraphResourceInspector::GetStatusText() const
{
    return mStatus->text();
}
bool RenderGraphResourceInspector::CanOpenInTextures() const
{
    return mOpen->isEnabled();
}
void RenderGraphResourceInspector::closeEvent(QCloseEvent* event)
{
    SetPinned(false);
    QDialog::closeEvent(event);
}
void RenderGraphResourceInspector::done(int32 result)
{
    SetPinned(false);
    QDialog::done(result);
}
void RenderGraphResourceInspector::Refresh()
{
    mOpen->setEnabled(false);
    mOpenName.clear();
    if (mShouldResolve == true || mResourceRevision != mDocument.GetRevision())
    {
        mResource = ResolveRenderGraphResourceInspection(mDocument, mRequest, mReferenceSize);
        mResourceRevision = mDocument.GetRevision();
        mShouldResolve = false;
    }
    const RenderGraphResourceDetails& resource = mResource;
    QStringList text{
        QStringLiteral("Draft · %1#%2\n%3 (%4)")
            .arg(mRequest.File,
                 mRequest.Configuration,
                 mRequest.Endpoint,
                 mRequest.IsOutput == true ? QStringLiteral("output") : QStringLiteral("input")),
        QStringLiteral("Type: %1").arg(resource.Type.isEmpty() == true ? QStringLiteral("unresolved") : resource.Type),
        QStringLiteral("Producer: %1")
            .arg(resource.Producer.isEmpty() == true ? QStringLiteral("unconnected") : resource.Producer),
        QStringLiteral("Consumers (displayed configuration): %1")
            .arg(resource.Consumers.isEmpty() == true ? QStringLiteral("none")
                                                      : resource.Consumers.join(QStringLiteral(", "))),
        resource.Description,
        QStringLiteral("Expanded resource: %1")
            .arg(resource.ResourceName.isEmpty() == true ? QStringLiteral("unresolved") : resource.ResourceName)};
    QString status;
    const RenderGraphResourcePreviewTarget* targetOrNull = nullptr;
    for (const RenderGraphResourcePreviewTarget& target : mPreview.Targets)
    {
        if (target.Name == resource.ResourceName)
        {
            targetOrNull = &target;
            break;
        }
    }
    if (targetOrNull != nullptr)
    {
        text.append(QStringLiteral("Generation %1 · %2\n%3\nThis is the generation's final resource contents, not an "
                                   "intermediate wire version.")
                        .arg(mPreview.ActiveGeneration)
                        .arg(targetOrNull->Name, targetOrNull->Description));
    }
    if (IsPinned() == true && mPinnedResource.isEmpty() == false && mPinnedResource != resource.ResourceName)
    {
        status =
            QStringLiteral("Unavailable · pinned resource %1 no longer matches this endpoint. Unpin or Inspect again.")
                .arg(mPinnedResource);
    }
    else if (resource.Problem.isEmpty() == false)
    {
        status = QStringLiteral("Unavailable · ") + resource.Problem;
    }
    else if (mPreview.IsAttached == false)
    {
        status = QStringLiteral("Unavailable · no native preview device is attached.");
    }
    else if (mPreview.ActiveGeneration == 0)
    {
        status = QStringLiteral("Unavailable · no active generation is available.");
    }
    else if (mPreview.ActiveGeneration != mPreview.PresentedGeneration || mPreview.HasPresentedCatalogue == false)
    {
        status = QStringLiteral(
            "Unavailable · active generation is awaiting a frame for its current extent/session; the last "
            "drawable image is retained.");
    }
    else if (mPreview.HasNames == false)
    {
        status = QStringLiteral("Unavailable · no verified resource-name catalogue. ") + mPreview.Problem;
    }
    else if (mPreview.RootFile.isEmpty() == false && mPreview.RootFile != mDocument.GetRootGraph())
    {
        status = QStringLiteral("Unavailable · the presented generation belongs to another root graph (%1).")
                     .arg(mPreview.RootFile);
    }
    else if (targetOrNull == nullptr)
    {
        status = QStringLiteral("Unavailable · resource is absent from the presented generation (older build, inactive "
                                "branch or optimized-out resource).");
    }
    else
    {
        if (mPreview.InputIdentity.isEmpty() == false &&
            (mIdentityRevision != mDocument.GetRevision() || mIdentityConfiguration != mPreview.Configuration))
        {
            mDraftIdentity = ComputeRenderGraphBuildIdentity(mDocument, mPreview.Configuration);
            mIdentityRevision = mDocument.GetRevision();
            mIdentityConfiguration = mPreview.Configuration;
        }
        const QString configuration =
            mRequest.Path.isEmpty() == true ? mRequest.Configuration : mRequest.Path.first().Configuration;
        if (mPreview.InputIdentity.isEmpty() == true || mPreview.RootFile.isEmpty() == true)
        {
            status = QStringLiteral("Unverified · generation source identity is unknown.");
        }
        else if (configuration != mPreview.Configuration || mPreview.InputIdentity != mDraftIdentity)
        {
            status = QStringLiteral("Stale · presented generation differs from the current draft/configuration.");
        }
        else
        {
            status = QStringLiteral("Current · presented generation matches the draft.");
        }
        if (targetOrNull->CanOpen == true)
        {
            mOpenName = targetOrNull->Name;
            mOpen->setEnabled(true);
        }
        else
        {
            status += QStringLiteral("\nUnavailable in Textures · buffer, presentation surface or unsupported image.");
        }
    }
    if (mPreview.Problem.isEmpty() == false && status.contains(mPreview.Problem) == false)
    {
        status += QLatin1Char('\n') + mPreview.Problem;
    }
    mDetails->setText(text.join(QStringLiteral("\n\n")));
    mStatus->setText(status);
}
} // namespace ludus::editor
