#include "internal/world_graph_view.h"
#include "internal/overlay_widgets.h"
#include "internal/viewport_object_interaction.h"

#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 MINIMUM_TARGET_TEXT_CHARACTERS = 4;
constexpr foundation::int32 STATISTICS_REFRESH_MILLISECONDS = 250;
constexpr foundation::int32 STATISTICS_MARGIN_PIXEL = 8;
} // namespace
WorldGraphView::WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent)
    : QWidget(parent), mWorkspace(inoutWorkspace), mPreview(*inoutWorkspace.GetPreviewOrNull()),
      mTargets(new QComboBox(this)), mStatus(new QLabel(this)), mStatistics(new HudLabel(this))
{
    setObjectName(QStringLiteral("worldGraphView"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setMinimumSize(1, 1);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mTargets->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mTargets->setMinimumContentsLength(MINIMUM_TARGET_TEXT_CHARACTERS);
    mTargets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mTargets->setObjectName(QStringLiteral("worldGraphTargets"));
    mTargets->setAccessibleName(QStringLiteral("Render graph output"));
    mTargets->setToolTip(QStringLiteral("Final output and every render-graph attachment. Special debug views use "
                                        "configurations authored in the project's graph."));
    mStatistics->setObjectName(QStringLiteral("worldGraphStatisticsOverlay"));
    mStatistics->setTextFormat(Qt::PlainText);
    mStatistics->setAttribute(Qt::WA_TransparentForMouseEvents);
    mStatistics->move(STATISTICS_MARGIN_PIXEL, STATISTICS_MARGIN_PIXEL);
    mStatistics->hide();
    mStatisticsTimer.setInterval(STATISTICS_REFRESH_MILLISECONDS);
    connect(&mStatisticsTimer, &QTimer::timeout, this, &WorldGraphView::refreshStatistics);
    mStatus->setObjectName(QStringLiteral("worldGraphStatus"));
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setWordWrap(true);
    mStatus->setAttribute(Qt::WA_TransparentForMouseEvents);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // Controls live in the owning viewport toolbar; only the frame receives navigation.
    mTargets->hide();
    layout->addStretch();
    layout->addWidget(mStatus);
    connect(&mPreview, &RenderGraphPreview::TargetsChanged, this, [this]() { refreshTargets(); });
    connect(&mPreview, &RenderGraphPreview::FramePresented, this, [this]() { refreshFrame(); });
    connect(&mPreview, &RenderGraphPreview::Changed, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::JobFinished, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::RenderProblemChanged, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::CommandsChanged, this, [this]() { refreshStatus(); });
    connect(mTargets, &QComboBox::currentIndexChanged, this, [this]() {
        mPreview.SetOutputTarget(mTargets->currentData().toString());
        refreshFrame();
    });
    refreshTargets();
    refreshFrame();
}
QWidget* WorldGraphView::TakeControls(QWidget* parent)
{
    QWidget* controls = new QWidget(parent);
    controls->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    QHBoxLayout* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    QComboBox* configurations = new QComboBox(controls);
    configurations->setObjectName(QStringLiteral("worldGraphConfigurations"));
    configurations->setAccessibleName(QStringLiteral("Render graph configuration"));
    const auto refresh = [this, configurations]() {
        const QSignalBlocker blocker(configurations);
        configurations->clear();
        configurations->addItems(mWorkspace.GetConfigurations());
        configurations->setCurrentText(mWorkspace.GetConfiguration());
    };
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, refresh);
    refresh();
    connect(configurations, &QComboBox::currentTextChanged, this, [this](const QString& configuration) {
        mWorkspace.SetConfiguration(configuration);
    });
    mTargets->setParent(controls);
    mTargets->show();
    layout->addWidget(configurations);
    layout->addWidget(mTargets, 1);
    return controls;
}
void WorldGraphView::SetCanPresent(bool canPresent)
{
    mCanPresent = canPresent;
    if (canPresent == true)
    {
        refreshFrame();
    }
}
void WorldGraphView::SetStatisticsVisible(bool isVisible)
{
    mIsStatisticsVisible = isVisible;
    mStatistics->setVisible(isVisible);
    if (isVisible == true)
    {
        mStatisticsTimer.start();
        refreshStatistics();
    }
    else
    {
        mStatisticsTimer.stop();
        mStatistics->clear();
    }
}
QString WorldGraphView::GetStatisticsText() const
{
    return mStatistics->text();
}
void WorldGraphView::refreshStatistics()
{
    if (mIsStatisticsVisible == false)
    {
        return;
    }
    const RenderPreview* sourceOrNull = mWorkspace.GetPreviewSourceOrNull();
    const ViewportStatistics deviceStatistics =
        sourceOrNull != nullptr ? sourceOrNull->GetStatistics() : ViewportStatistics{};
    mStatistics->setText(
        FormatGraphViewportStatistics(mPreview.GetStatistics(), deviceStatistics, foundation::time::Now().Nanoseconds));
    mStatistics->adjustSize();
    mStatistics->raise();
    Q_EMIT StatisticsChanged();
}
void WorldGraphView::ClearFrame()
{
    mPaintTrace.Cancel();
    mImage = {};
    update();
}
const QImage& WorldGraphView::GetImage() const noexcept
{
    return mImage;
}
foundation::uint64 WorldGraphView::GetPresentedFrames() const noexcept
{
    return mPresentedFrames;
}
void WorldGraphView::refreshTargets()
{
    const QString selected = mTargets->currentData().toString();
    const QSignalBlocker blocker(mTargets);
    mTargets->clear();
    mTargets->addItem(QStringLiteral("Final output"), QString());
    for (const RenderGraphTarget& target : mPreview.GetTargetCatalogue().Targets)
    {
        if (target.IsSurface == false)
        {
            mTargets->addItem(target.Name, target.Name);
        }
    }
    const foundation::int32 index = mTargets->findData(selected);
    mTargets->setCurrentIndex(index >= 0 ? index : 0);
    mPreview.SetOutputTarget(mTargets->currentData().toString());
}
void WorldGraphView::refreshFrame()
{
    if (mCanPresent == false)
    {
        return;
    }
    if (mPreview.GetImage().isNull() == false)
    {
        if (mImage.cacheKey() != mPreview.GetImage().cacheKey())
        {
            ViewportFrameTrace& trace = mPreview.GetFrameTrace();
            mPaintTrace.Queue(trace.GetPublishedIdentity(), trace.GetPublishedBeginTicks());
            mImage = mPreview.GetImage();
            ++mPresentedFrames;
        }
    }
    refreshStatus();
    update();
}
void WorldGraphView::refreshStatus()
{
    QString text;
    if (mWorkspace.GetRenderProblem().isEmpty() == false)
    {
        text = mWorkspace.GetRenderProblem();
    }
    else if (mWorkspace.GetDocument().IsLoaded() == false)
    {
        text = mWorkspace.GetStatusText();
    }
    else if (mWorkspace.IsJobActive() == true)
    {
        text = QStringLiteral("Compiling shaders…");
    }
    else if (mWorkspace.GetLastResult().Request.Request != 0 && mWorkspace.GetLastResult().IsSuccessful == false)
    {
        text = mWorkspace.GetStatusText();
    }
    else
    {
        text = mPreview.GetDisplayStatus();
    }
    if (mWorkspace.GetRenderProblem().isEmpty() == true && mWorkspace.GetRenderWarning().isEmpty() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += mWorkspace.GetRenderWarning();
    }
    if (mWorkspace.IsUsingDefaultRenderer() == true && mWorkspace.IsBuiltIn() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += QStringLiteral("Engine default renderer fallback.");
    }
    mStatus->setText(text);
    mStatus->setVisible(text.isEmpty() == false);
    Q_EMIT StatusChanged(text);
}
void WorldGraphView::SetObjectInteraction(ViewportObjectInteraction& inoutInteraction)
{
    mInteractionOrNull = &inoutInteraction;
    connect(&inoutInteraction, &ViewportObjectInteraction::Changed, this, [this]() { update(); });
}
void WorldGraphView::paintEvent(QPaintEvent*)
{
    LUDUS_PROFILE_SCOPE(EditorWorldGraphPaint);
    mPaintTrace.BeginPaint();
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        if (mImage.isNull() == false)
        {
            const QSize size = mImage.size().scaled(this->size(), Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
            painter.drawImage(target, mImage);
            if (mInteractionOrNull != nullptr)
            {
                mInteractionOrNull->Paint(painter, target);
            }
        }
    }
    mPaintTrace.EndPaint();
}
} // namespace ludus::editor
