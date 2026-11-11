#include "internal/world_graph_view.h"
#include "design_tokens.h"
#include "internal/overlay_widgets.h"
#include "internal/viewport_object_interaction.h"

#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 MINIMUM_TARGET_TEXT_CHARACTERS = 4;
constexpr foundation::int32 EXPOSURE_DECIMALS = 2;
constexpr foundation::float64 EXPOSURE_STEP_STOPS = 0.25;
constexpr foundation::int32 STATISTICS_REFRESH_MILLISECONDS = 250;
constexpr foundation::int32 STATISTICS_MARGIN_PIXEL = design::SPACING_SMALL;
/// Output-menu item role holding a built-in debug view's PreviewShading; graph targets leave it empty.
constexpr foundation::int32 DEBUG_VIEW_ROLE = Qt::UserRole + 1;
} // namespace
WorldGraphView::WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent)
    : QWidget(parent), mWorkspace(inoutWorkspace), mPreview(*inoutWorkspace.GetPreviewOrNull()),
      mTargets(new QComboBox(this)), mStatus(new QLabel(this)), mStatistics(new HudLabel(this))
{
    setObjectName(QStringLiteral("worldGraphView"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setMinimumSize(1, 1);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mTargets->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mTargets->setMinimumContentsLength(MINIMUM_TARGET_TEXT_CHARACTERS);
    mTargets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mTargets->setObjectName(QStringLiteral("worldGraphTargets"));
    mTargets->setAccessibleName(QStringLiteral("Render graph output"));
    mTargets->setToolTip(
        QStringLiteral("Final output and every render-graph attachment, then built-in debug views. A debug view "
                       "draws the World with the editor's raster renderer, regardless of what the project's graph "
                       "draws, showing one material, geometry or lighting quantity per pixel (albedo, normals, "
                       "depth, wireframe…). Other special views use configurations authored in the project's "
                       "graph."));
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
        mDebugViewProblem.clear();
        mPreview.SetOutputTarget(mTargets->currentData().toString());
        refreshFrame();
        Q_EMIT DebugViewChanged();
    });
    refreshTargets();
    refreshFrame();
}
foundation::int32 ComputeComboBoxWidth(const QComboBox& comboBox, const QString& text)
{
    // Mirrors QComboBox's own size hint for one item: the text's advance, grown by the style's
    // frame, padding and arrow.
    QStyleOptionComboBox option;
    option.initFrom(&comboBox);
    option.editable = comboBox.isEditable();
    option.frame = comboBox.hasFrame();
    option.currentText = text;
    const QFontMetrics metrics = comboBox.fontMetrics();
    const QSize contents(metrics.horizontalAdvance(text), metrics.height());
    return comboBox.style()->sizeFromContents(QStyle::CT_ComboBox, &option, contents, &comboBox).width();
}
void FitComboBoxToCurrentText(QComboBox& inoutComboBox)
{
    const char* const IS_FITTED_PROPERTY = "isFittedToCurrentText";
    inoutComboBox.setMinimumWidth(ComputeComboBoxWidth(inoutComboBox, inoutComboBox.currentText()));
    if (inoutComboBox.property(IS_FITTED_PROPERTY).toBool() == true)
    {
        return;
    }
    inoutComboBox.setProperty(IS_FITTED_PROPERTY, true);
    QObject::connect(&inoutComboBox, &QComboBox::currentTextChanged, &inoutComboBox, [comboBox = &inoutComboBox]() {
        comboBox->setMinimumWidth(ComputeComboBoxWidth(*comboBox, comboBox->currentText()));
    });
}
WorldGraphControls WorldGraphView::TakeControls(QWidget* parent, RenderPreview& inoutSource)
{
    // Sized by its contents, so the toolbar overflows the group whole when it does not fit
    // rather than squeezing both choices until their text disappears.
    QWidget* controls = new QWidget(parent);
    controls->setObjectName(QStringLiteral("worldGraphOutputControls"));
    controls->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    QHBoxLayout* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(design::SPACING_SMALL);
    QComboBox* configurations = new QComboBox(controls);
    configurations->setObjectName(QStringLiteral("worldGraphConfigurations"));
    configurations->setAccessibleName(QStringLiteral("Render graph configuration"));
    configurations->setToolTip(QStringLiteral("Render graph configuration this viewport builds. Shown when the "
                                              "project graph defines more than one."));
    const auto refresh = [this, configurations]() {
        const QSignalBlocker blocker(configurations);
        configurations->clear();
        configurations->addItems(mWorkspace.GetConfigurations());
        configurations->setCurrentText(mWorkspace.GetConfiguration());
        // A single configuration offers no choice, and none leaves nothing to show.
        configurations->setVisible(configurations->count() > 1);
        FitComboBoxToCurrentText(*configurations);
    };
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, refresh);
    refresh();
    connect(configurations, &QComboBox::currentTextChanged, this, [this](const QString& configuration) {
        mWorkspace.SetConfiguration(configuration);
    });
    mTargets->setParent(controls);
    mTargets->show();
    layout->addWidget(configurations);
    layout->addWidget(mTargets);

    // An action with a menu: on the row it is a drop-down button, and in the overflow menu a submenu.
    QAction* exposureButton = new QAction(QStringLiteral("Exposure"), parent);
    exposureButton->setObjectName(QStringLiteral("viewportExposureAction"));
    QMenu* exposureMenu = new QMenu(parent);
    exposureButton->setMenu(exposureMenu);
    QDoubleSpinBox* exposure = new QDoubleSpinBox(exposureMenu);
    exposure->setObjectName(QStringLiteral("viewportExposureStops"));
    exposure->setAccessibleName(QStringLiteral("Exposure compensation in stops"));
    exposure->setSuffix(QStringLiteral(" stops"));
    exposure->setDecimals(EXPOSURE_DECIMALS);
    constexpr foundation::float64 LIMIT =
        static_cast<foundation::float64>(graphics::renderer::ViewDescription::MAXIMUM_RASTER_EXPOSURE_STOPS);
    exposure->setRange(-LIMIT, LIMIT);
    exposure->setSingleStep(EXPOSURE_STEP_STOPS);
    exposure->setKeyboardTracking(false);
    QWidgetAction* exposureField = new QWidgetAction(exposureMenu);
    exposureField->setDefaultWidget(exposure);
    exposureMenu->addAction(exposureField);
    QAction* reset = exposureMenu->addAction(QStringLiteral("Reset to graph exposure"));
    reset->setObjectName(QStringLiteral("viewportResetExposure"));
    connect(exposure, &QDoubleSpinBox::valueChanged, &inoutSource, [&inoutSource](foundation::float64 stops) {
        inoutSource.SetRasterExposureStops(static_cast<foundation::float32>(stops));
    });
    connect(reset, &QAction::triggered, &inoutSource, [&inoutSource]() { inoutSource.SetRasterExposureStops(0); });
    const auto refreshExposure = [this, exposureButton, exposure, reset, &inoutSource]() {
        const bool canAdjust =
            mTargets->currentIndex() == 0 && mPreview.CanAdjustRasterExposure() == true &&
            (inoutSource.Scene().Objects.isEmpty() == false || inoutSource.Scene().Atmosphere.IsEnabled == true);
        const foundation::float32 stops = inoutSource.GetRasterExposureStops();
        exposureButton->setEnabled(canAdjust);
        exposure->setEnabled(canAdjust);
        exposureButton->setToolTip(
            canAdjust == true ? QStringLiteral("Adjust this viewport in stops relative to the graph's exposure.")
                              : QStringLiteral("Exposure needs Final output from a clustered deferred raster "
                                               "graph with ToneMap enabled and a World with geometry or atmosphere."));
        const QSignalBlocker blocker(exposure);
        exposure->setValue(static_cast<foundation::float64>(stops));
        reset->setEnabled(canAdjust == true && stops != 0);
    };
    connect(&mPreview, &RenderGraphPreview::Changed, exposureButton, refreshExposure);
    connect(&mPreview, &RenderGraphPreview::TargetsChanged, exposureButton, refreshExposure);
    connect(&mPreview, &RenderGraphPreview::RayPathCaptureChanged, exposureButton, refreshExposure);
    connect(&inoutSource, &RenderPreview::Changed, exposureButton, refreshExposure);
    connect(&inoutSource, &RenderPreview::RasterExposureChanged, exposureButton, refreshExposure);
    connect(mTargets, &QComboBox::currentIndexChanged, exposureButton, refreshExposure);
    refreshExposure();
    return { .Output = controls, .Exposure = exposureButton };
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
bool WorldGraphView::TryGetDebugView(PreviewShading& outShading) const
{
    const QVariant shading = mTargets->currentData(DEBUG_VIEW_ROLE);
    if (shading.isValid() == false)
    {
        return false;
    }
    outShading = static_cast<PreviewShading>(shading.toInt());
    return true;
}
void WorldGraphView::RejectDebugView(const QString& reason)
{
    PreviewShading shading = PreviewShading::Lit;
    if (TryGetDebugView(shading) == false)
    {
        return;
    }
    const QString problem = QStringLiteral("%1 view is unavailable for this World, so the final output is shown. %2")
                                .arg(GetPreviewShadingName(shading), reason);
    mTargets->setCurrentIndex(0);
    // Selecting the final output clears the previous problem; this one explains that selection.
    mDebugViewProblem = problem;
    refreshStatus();
}
void WorldGraphView::refreshTargets()
{
    const QString selected = mTargets->currentData().toString();
    const QVariant selectedDebugView = mTargets->currentData(DEBUG_VIEW_ROLE);
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
    mTargets->insertSeparator(mTargets->count());
    for (foundation::usize i = 0; i < PREVIEW_SHADING_COUNT; ++i)
    {
        const PreviewShading shading = static_cast<PreviewShading>(i);
        if (IsPreviewGraphDebugView(shading) == false)
        {
            continue;
        }
        const foundation::int32 item = mTargets->count();
        mTargets->addItem(QStringLiteral("Debug: %1").arg(GetPreviewShadingName(shading)));
        mTargets->setItemData(item, static_cast<foundation::int32>(shading), DEBUG_VIEW_ROLE);
        mTargets->setItemData(item, GetPreviewShadingLabel(shading), Qt::ToolTipRole);
    }
    const foundation::int32 index = selectedDebugView.isValid() == true
                                        ? mTargets->findData(selectedDebugView, DEBUG_VIEW_ROLE)
                                        : mTargets->findData(selected);
    mTargets->setCurrentIndex(index >= 0 ? index : 0);
    // The blocker hides this change from the fitting connection, so fit the new choice here.
    FitComboBoxToCurrentText(*mTargets);
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
    QString fullText;
    mHasProblemChip = false;
    const bool isBuildFailure = mWorkspace.GetLastResult().Request.Request != 0 &&
                                mWorkspace.GetLastResult().IsSuccessful == false &&
                                mWorkspace.GetLastResult().Request.Action == QStringLiteral("build");
    if (mWorkspace.GetRenderProblem().isEmpty() == false && isBuildFailure == true && mWorkspace.IsJobActive() == false)
    {
        // The compiler's words are in the Error List and the tooltip; the picture only says what happened.
        mHasProblemChip = true;
        fullText = mWorkspace.GetRenderProblem();
        text =
            QStringLiteral("%1. %2").arg(mWorkspace.IsUsingDefaultRenderer() == true && mWorkspace.IsBuiltIn() == false
                                             ? QStringLiteral("Shader build failed, using default renderer")
                                             : QStringLiteral("Shader build failed, preview unchanged"),
                                         GetProblemChipLink());
    }
    else if (mWorkspace.GetRenderProblem().isEmpty() == false)
    {
        text = mWorkspace.GetRenderProblem();
    }
    else if (mWorkspace.GetDocument().IsLoaded() == false ||
             (mWorkspace.IsJobActive() == false && mWorkspace.GetLastResult().Request.Request != 0 &&
              mWorkspace.GetLastResult().IsSuccessful == false))
    {
        text = mWorkspace.GetStatusText();
    }
    else if (mWorkspace.IsJobActive() == true)
    {
        text = QStringLiteral("Compiling shaders…");
    }
    else
    {
        text = mPreview.GetDisplayStatus();
    }
    if (mDebugViewProblem.isEmpty() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += mDebugViewProblem;
    }
    if (mWorkspace.GetRenderProblem().isEmpty() == true && mWorkspace.GetRenderWarning().isEmpty() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += mWorkspace.GetRenderWarning();
    }
    if (mHasProblemChip == false && mWorkspace.IsUsingDefaultRenderer() == true && mWorkspace.IsBuiltIn() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += QStringLiteral("Engine default renderer fallback.");
    }
    // The view lets pointer input through to the viewport that navigates; only while the chip is shown does it take
    // presses, and a press outside the chip is ignored so it still reaches the viewport.
    setAttribute(Qt::WA_TransparentForMouseEvents, mHasProblemChip == false);
    mStatus->setText(text);
    mStatus->setToolTip(fullText);
    mStatus->setVisible(text.isEmpty() == false);
    Q_EMIT StatusChanged(text);
}
QString WorldGraphView::GetProblemChipLink()
{
    return QStringLiteral("Show in Error List");
}
void WorldGraphView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && mHasProblemChip == true && mStatus->isVisible() == true &&
        mStatus->geometry().contains(event->position().toPoint()) == true)
    {
        mWorkspace.RequestErrorList();
        event->accept();
        return;
    }
    event->ignore();
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
