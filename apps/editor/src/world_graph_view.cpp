#include "internal/world_graph_view.h"

#include "internal/render_graph_preview.h"
#include "internal/renderer_workspace.h"

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
}
WorldGraphView::WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent)
    : QWidget(parent), mWorkspace(inoutWorkspace), mPreview(*inoutWorkspace.GetPreviewOrNull()),
      mTargets(new QComboBox(this)), mStatus(new QLabel(this))
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
    connect(&mWorkspace, &RendererWorkspace::JobFinished, this, [this]() {
        mStatus->setText(mWorkspace.GetStatusText());
        mStatus->setVisible(mStatus->text().isEmpty() == false);
    });
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, [this]() { refreshStatus(); });
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
void WorldGraphView::ClearFrame()
{
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
            mImage = mPreview.GetImage();
            ++mPresentedFrames;
        }
        refreshStatus();
    }
    update();
}
void WorldGraphView::refreshStatus()
{
    QString text;
    if (mWorkspace.GetDocument().IsLoaded() == false)
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
    mStatus->setText(text);
    mStatus->setVisible(text.isEmpty() == false);
}
void WorldGraphView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    if (mImage.isNull() == false)
    {
        const QSize size = mImage.size().scaled(this->size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        painter.drawImage(target, mImage);
    }
}
} // namespace ludus::editor
