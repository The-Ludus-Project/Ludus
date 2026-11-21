#include "internal/world_graph_view.h"
#include "design_tokens.h"
#include "internal/overlay_widgets.h"
#include "internal/viewport_object_interaction.h"

#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_hud_text.h"
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/world_feature.hpp>

#include <QAction>
#include <QActionGroup>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHash>
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
/// Marks a combo box whose minimum width already follows its current choice.
constexpr const char* IS_FITTED_TO_CURRENT_TEXT_PROPERTY = "isFittedToCurrentText";
constexpr foundation::int32 EXPOSURE_DECIMALS = 2;
constexpr foundation::float64 EXPOSURE_STEP_STOPS = 0.25;
constexpr foundation::int32 STATISTICS_REFRESH_MILLISECONDS = 250;
constexpr foundation::int32 STATISTICS_MARGIN_PIXEL = design::SPACING_SMALL;
/// Output-menu item role holding a built-in debug view's PreviewShading; graph targets leave it empty.
constexpr foundation::int32 DEBUG_VIEW_ROLE = Qt::UserRole + 1;
/// Output-menu item roles of a lighting view: its DeferredLightingView code and the source ID it isolates.
constexpr foundation::int32 LIGHTING_VIEW_ROLE = Qt::UserRole + 2;
constexpr foundation::int32 LIGHTING_LIGHT_ROLE = Qt::UserRole + 3;
using graphics::renderer::DeferredLightingView;
/// One whole-scene lighting view of the output menu.
struct LightingViewEntry final
{
    DeferredLightingView View;
    const char* Name;
    const char* Help;
};
constexpr foundation::usize LIGHTING_VIEW_ENTRY_COUNT = 7;
constexpr foundation::StaticArray<LightingViewEntry, LIGHTING_VIEW_ENTRY_COUNT> LIGHTING_VIEW_ENTRIES{{
    {DeferredLightingView::DirectLights,
     "Direct lights",
     "Shadowed light from every Sun, Moon, point and spot light, without sky or emission."},
    {DeferredLightingView::SkyDiffuse,
     "Sky diffuse",
     "Diffuse light from the Atmosphere sky or environment, after sky visibility."},
    {DeferredLightingView::SkySpecular,
     "Sky specular",
     "Reflections of the Atmosphere sky or environment, after sky visibility."},
    {DeferredLightingView::AreaLights, "Area lights", "Shadowed light from emissive meshes."},
    {DeferredLightingView::AerialPerspective,
     "Aerial perspective",
     "Light the Atmosphere scatters between the camera and each surface."},
    {DeferredLightingView::DominantLight,
     "Dominant light",
     "Each pixel in the colour of its largest contributor, at the final brightness. The legend names the colours."},
    {DeferredLightingView::SkyVisibility,
     "Sky visibility",
     "How much of the sky each surface sees past the World's geometry: white open, black covered."},
}};
/// One lighting-view entry of the output menu.
struct LightingViewTarget final
{
    QString Name;
    QString Help;
    DeferredLightingView View = DeferredLightingView::Final;
    /// Light the view isolates; zero for whole-scene views.
    foundation::uint64 SourceId = 0;
};
/// Menu and legend name of a light: its World name, or its kind and source ID.
QString GetLightingViewLightName(const graphics::renderer::DirectLight& light,
                                 const QHash<foundation::uint64, QString>& names)
{
    QString name = names.value(light.SourceId);
    if (name.isEmpty() == false)
    {
        return name;
    }
    const QString kind = light.Kind == graphics::renderer::LightKind::Directional ? QStringLiteral("Directional light")
                         : light.Kind == graphics::renderer::LightKind::Point     ? QStringLiteral("Point light")
                         : light.Kind == graphics::renderer::LightKind::Spot      ? QStringLiteral("Spot light")
                                                                                  : QStringLiteral("Area light");
    constexpr foundation::int32 HEXADECIMAL_BASE = 16;
    return QStringLiteral("%1 %2").arg(kind).arg(light.SourceId, 0, HEXADECIMAL_BASE);
}
/// The extraction's lights in ascending source ID order: the resolve's packed order, which numbers the
/// dominant-light view's light categories.
QVector<graphics::renderer::DirectLight> SortLightingViewLights(QVector<graphics::renderer::DirectLight> lights)
{
    // At most MAX_DIRECT_LIGHTS: an insertion sort is enough.
    for (qsizetype index = 1; index < lights.size(); ++index)
    {
        const graphics::renderer::DirectLight light = lights[index];
        qsizetype position = index;
        while (position > 0 && lights[position - 1].SourceId > light.SourceId)
        {
            lights[position] = lights[position - 1];
            --position;
        }
        lights[position] = light;
    }
    return lights;
}
/// Rich-text swatch of a dominant-light category in its display colour.
QString MakeLegendSwatch(foundation::uint32 category, const QString& name)
{
    const foundation::math::Vector3 hue = graphics::renderer::GetDeferredLightingViewHue(category);
    const QColor colour = QColor::fromRgbF(hue.X, hue.Y, hue.Z);
    return QStringLiteral("<span style=\"color:%1\">&#9632;</span>&nbsp;%2").arg(colour.name(), name.toHtmlEscaped());
}
} // namespace
WorldGraphView::WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent)
    : QWidget(parent), mWorkspace(inoutWorkspace), mPreview(*inoutWorkspace.GetPreviewOrNull()),
      mTargets(new QComboBox(this)), mStatus(new QLabel(this)), mLegend(new QLabel(this)),
      mStatistics(new HudGridLabel(this))
{
    setObjectName(QStringLiteral("worldGraphView"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setMinimumSize(1, 1);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mTargets->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mTargets->setMinimumContentsLength(MINIMUM_TARGET_TEXT_CHARACTERS);
    mTargets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mTargets->setObjectName(QStringLiteral("worldGraphTargets"));
    // The output choice sits in the viewport toolbar and speaks its quiet language.
    mTargets->setProperty("visualRole", QStringLiteral("ghost"));
    mTargets->setAccessibleName(QStringLiteral("Render graph output"));
    mTargets->setToolTip(
        QStringLiteral("Final output and every render-graph attachment, then built-in debug views. A debug view "
                       "draws the World with the editor's raster renderer, regardless of what the project's graph "
                       "draws, showing one material, geometry or lighting quantity per pixel (albedo, normals, "
                       "depth, wireframe…). Lighting views follow: the graph's own resolve draws one term of its "
                       "lighting, such as one light, the sky or its shadow coverage. Other special views use "
                       "configurations authored in the project's graph."));
    mStatistics->setObjectName(QStringLiteral("worldGraphStatisticsOverlay"));
    mStatistics->setTextFormat(Qt::PlainText);
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
    mLegend->setObjectName(QStringLiteral("worldGraphLightingLegend"));
    mLegend->setTextFormat(Qt::RichText);
    mLegend->setWordWrap(true);
    mLegend->setAttribute(Qt::WA_TransparentForMouseEvents);
    mLegend->hide();
    layout->addWidget(mLegend);
    layout->addWidget(mStatus);
    connect(&mPreview, &RenderGraphPreview::TargetsChanged, this, [this]() { refreshTargets(); });
    connect(&mPreview, &RenderGraphPreview::FramePresented, this, [this]() { refreshFrame(); });
    connect(&mPreview, &RenderGraphPreview::Changed, this, [this]() {
        refreshStatus();
        refreshLightingTargets();
    });
    connect(&mWorkspace, &RendererWorkspace::JobFinished, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::DocumentChanged, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::RenderProblemChanged, this, [this]() { refreshStatus(); });
    connect(&mWorkspace, &RendererWorkspace::CommandsChanged, this, [this]() { refreshStatus(); });
    connect(mTargets, &QComboBox::currentIndexChanged, this, [this]() {
        mDebugViewProblem.clear();
        mPreview.SetOutputTarget(mTargets->currentData().toString());
        applyLightingView();
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
    inoutComboBox.setMinimumWidth(ComputeComboBoxWidth(inoutComboBox, inoutComboBox.currentText()));
    if (inoutComboBox.property(IS_FITTED_TO_CURRENT_TEXT_PROPERTY).toBool() == true)
    {
        return;
    }
    inoutComboBox.setProperty(IS_FITTED_TO_CURRENT_TEXT_PROPERTY, true);
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
    configurations->setProperty("visualRole", QStringLiteral("ghost"));
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
    QAction* exposureAction = new QAction(QStringLiteral("Exposure"), parent);
    exposureAction->setObjectName(QStringLiteral("viewportExposureAction"));
    QMenu* exposureMenu = new QMenu(parent);
    exposureAction->setMenu(exposureMenu);
    // Base exposure: metered, a physical camera setting, or the graph's own multiplier.
    QActionGroup* modes = new QActionGroup(exposureMenu);
    // Each mode is a checkable action named for tests and automation, holding its mode as data.
    const auto addMode = [&](ViewportExposureMode mode) {
        const QString text = mode == ViewportExposureMode::Automatic ? QStringLiteral("Automatic")
                             : mode == ViewportExposureMode::Manual  ? QStringLiteral("Physical camera")
                                                                     : QStringLiteral("Graph exposure");
        const QString name = mode == ViewportExposureMode::Automatic ? QStringLiteral("viewportExposureAutomatic")
                             : mode == ViewportExposureMode::Manual  ? QStringLiteral("viewportExposureManual")
                                                                     : QStringLiteral("viewportExposureGraph");
        QAction* action = exposureMenu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(static_cast<foundation::int32>(mode));
        modes->addAction(action);
        return action;
    };
    QAction* automatic = addMode(ViewportExposureMode::Automatic);
    QAction* manual = addMode(ViewportExposureMode::Manual);
    QAction* graphExposure = addMode(ViewportExposureMode::Graph);
    automatic->setToolTip(QStringLiteral("Meter the World's Sun, sky, Moon and directional lights like an incident "
                                         "light meter. Worlds without an Atmosphere use the graph's exposure."));
    manual->setToolTip(QStringLiteral("Expose like a camera at ISO 100: 15 for clear sun, 12 for overcast, "
                                      "about -3 under a full Moon."));
    graphExposure->setToolTip(QStringLiteral("Use the graph's ToneMap multiplier, where one nit is near white."));
    QDoubleSpinBox* exposureValue = new QDoubleSpinBox(exposureMenu);
    exposureValue->setObjectName(QStringLiteral("viewportExposureValue100"));
    exposureValue->setAccessibleName(QStringLiteral("Exposure value at ISO 100"));
    exposureValue->setPrefix(QStringLiteral("EV100 "));
    exposureValue->setDecimals(EXPOSURE_DECIMALS);
    exposureValue->setRange(static_cast<foundation::float64>(graphics::renderer::MIN_EXPOSURE_VALUE_100),
                            static_cast<foundation::float64>(graphics::renderer::MAX_EXPOSURE_VALUE_100));
    exposureValue->setSingleStep(EXPOSURE_STEP_STOPS);
    exposureValue->setKeyboardTracking(false);
    QWidgetAction* exposureValueField = new QWidgetAction(exposureMenu);
    exposureValueField->setDefaultWidget(exposureValue);
    exposureMenu->addAction(exposureValueField);
    exposureMenu->addSeparator();
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
    QAction* reset = exposureMenu->addAction(QStringLiteral("Reset compensation"));
    reset->setObjectName(QStringLiteral("viewportResetExposure"));
    connect(modes, &QActionGroup::triggered, &inoutSource, [&inoutSource](QAction* action) {
        const ViewportExposureMode mode = static_cast<ViewportExposureMode>(action->data().toInt());
        // Switching from a metered exposure to the physical camera starts from the metered value.
        const ViewportExposure current = inoutSource.ResolveExposure();
        if (mode == ViewportExposureMode::Manual && current.Mode == graphics::renderer::ExposureMode::Physical)
        {
            inoutSource.SetManualExposureValue100(current.ExposureValue100);
        }
        inoutSource.SetExposureMode(mode);
    });
    connect(exposureValue, &QDoubleSpinBox::valueChanged, &inoutSource, [&inoutSource](foundation::float64 value) {
        // Typing a value chooses the physical camera it sets.
        inoutSource.SetManualExposureValue100(static_cast<foundation::float32>(value));
        inoutSource.SetExposureMode(ViewportExposureMode::Manual);
    });
    connect(exposure, &QDoubleSpinBox::valueChanged, &inoutSource, [&inoutSource](foundation::float64 stops) {
        inoutSource.SetRasterExposureStops(static_cast<foundation::float32>(stops));
    });
    connect(reset, &QAction::triggered, &inoutSource, [&inoutSource]() { inoutSource.SetRasterExposureStops(0); });
    const auto refreshExposure =
        [this, exposureAction, exposure, exposureValue, reset, automatic, manual, graphExposure, &inoutSource]() {
            // Final output and the lighting views, which keep the pass's ToneMap, take the exposure.
            const bool isFinalOutput =
                mTargets->currentIndex() == 0 || mTargets->currentData(LIGHTING_VIEW_ROLE).isValid();
            const bool canAdjust =
                isFinalOutput == true && mPreview.CanAdjustRasterExposure() == true &&
                (inoutSource.Scene().Objects.isEmpty() == false || inoutSource.Scene().Atmosphere.IsEnabled == true);
            const foundation::float32 stops = inoutSource.GetRasterExposureStops();
            const ViewportExposureMode mode = inoutSource.GetExposureMode();
            const ViewportExposure resolved = inoutSource.ResolveExposure();
            const bool isPhysical = resolved.Mode == graphics::renderer::ExposureMode::Physical;
            exposureAction->setEnabled(canAdjust);
            exposure->setEnabled(canAdjust);
            exposureValue->setEnabled(canAdjust);
            exposureAction->setToolTip(
                canAdjust == false
                    ? QStringLiteral("Exposure needs Final output from a clustered deferred raster "
                                     "graph with ToneMap enabled and a World with geometry or atmosphere.")
                : isPhysical == true ? QStringLiteral("Exposed at EV100 %1, adjusted in stops.")
                                           .arg(static_cast<foundation::float64>(resolved.ExposureValue100), 0, 'f', 1)
                                     : QStringLiteral("Graph exposure, adjusted in stops."));
            // Automatic shows what it metered, so switching to Physical camera keeps the picture.
            automatic->setText(mode == ViewportExposureMode::Automatic && isPhysical == true
                                   ? QStringLiteral("Automatic (EV100 %1)")
                                         .arg(static_cast<foundation::float64>(resolved.ExposureValue100), 0, 'f', 1)
                               : mode == ViewportExposureMode::Automatic
                                   ? QStringLiteral("Automatic (no Atmosphere: graph exposure)")
                                   : QStringLiteral("Automatic"));
            automatic->setChecked(mode == ViewportExposureMode::Automatic);
            manual->setChecked(mode == ViewportExposureMode::Manual);
            graphExposure->setChecked(mode == ViewportExposureMode::Graph);
            const QSignalBlocker valueBlocker(exposureValue);
            exposureValue->setValue(static_cast<foundation::float64>(
                isPhysical == true ? resolved.ExposureValue100 : inoutSource.GetManualExposureValue100()));
            const QSignalBlocker blocker(exposure);
            exposure->setValue(static_cast<foundation::float64>(stops));
            reset->setEnabled(canAdjust == true && stops != 0);
        };
    connect(&mPreview, &RenderGraphPreview::Changed, exposureAction, refreshExposure);
    connect(&mPreview, &RenderGraphPreview::TargetsChanged, exposureAction, refreshExposure);
    connect(&mPreview, &RenderGraphPreview::RayPathCaptureChanged, exposureAction, refreshExposure);
    connect(&inoutSource, &RenderPreview::Changed, exposureAction, refreshExposure);
    connect(&inoutSource, &RenderPreview::RasterExposureChanged, exposureAction, refreshExposure);
    connect(mTargets, &QComboBox::currentIndexChanged, exposureAction, refreshExposure);
    refreshExposure();

    QAction* workLightAction = new QAction(QStringLiteral("Work Light"), parent);
    workLightAction->setObjectName(QStringLiteral("viewportWorkLightAction"));
    workLightAction->setCheckable(true);
    const auto refreshWorkLight = [this, workLightAction, &inoutSource]() {
        const bool canUseWorkLight = mTargets->currentIndex() == 0 && mPreview.CanShowWorkLight() == true &&
                                     inoutSource.Scene().Objects.isEmpty() == false;
        workLightAction->setChecked(mPreview.IsWorkLightEnabled());
        workLightAction->setEnabled(canUseWorkLight);
        workLightAction->setToolTip(
            canUseWorkLight == true
                ? QStringLiteral("Light nearby surfaces with a spotlight that follows the editor camera.")
                : QStringLiteral("Work Light needs geometry and the default raster graph’s Final output. "
                                 "Select Final output and use Force raster if another graph is active."));
    };
    connect(workLightAction, &QAction::triggered, &mPreview, &RenderGraphPreview::SetWorkLightEnabled);
    connect(&mPreview, &RenderGraphPreview::Changed, workLightAction, refreshWorkLight);
    connect(&mPreview, &RenderGraphPreview::TargetsChanged, workLightAction, refreshWorkLight);
    connect(&mPreview, &RenderGraphPreview::RayPathCaptureChanged, workLightAction, refreshWorkLight);
    connect(&inoutSource, &RenderPreview::Changed, workLightAction, refreshWorkLight);
    connect(mTargets, &QComboBox::currentIndexChanged, workLightAction, refreshWorkLight);
    refreshWorkLight();

    mSourceOrNull = &inoutSource;
    connect(&inoutSource, &RenderPreview::Changed, this, [this]() { refreshLightingTargets(); });
    refreshTargets();
    return { .OutputOrNull = controls, .ExposureOrNull = exposureAction, .WorkLightOrNull = workLightAction };
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
        mFullStatisticsText.clear();
    }
    updatePointerTransparency();
}

void WorldGraphView::updatePointerTransparency()
{
    setAttribute(Qt::WA_TransparentForMouseEvents, HasStatusLink() == false && mStatistics->isVisible() == false);
}
QString WorldGraphView::GetStatisticsText() const
{
    return mFullStatisticsText;
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
    const foundation::uint64 nowNanoseconds = foundation::time::Now().Nanoseconds;
    mFullStatisticsText = FormatGraphViewportStatistics(mPreview.GetStatistics(), deviceStatistics, nowNanoseconds);
    mStatistics->SetStatisticsText(
        FormatCompactGraphViewportStatistics(mPreview.GetStatistics(), deviceStatistics, nowNanoseconds),
        mFullStatisticsText,
        FormatGraphViewportStatisticsSummary(mPreview.GetStatistics(), nowNanoseconds));
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
    const QVariant selectedLightingView = mTargets->currentData(LIGHTING_VIEW_ROLE);
    const QVariant selectedLightingLight = mTargets->currentData(LIGHTING_LIGHT_ROLE);
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
    // Lighting views of the clustered deferred resolve: whole-scene terms, then each light alone and its shadow-map
    // coverage. The lights follow the resolve's source ID order, which also numbers the dominant-light colours.
    const QHash<foundation::uint64, QString> names =
        mSourceOrNull != nullptr ? mSourceOrNull->Scene().LightNames : QHash<foundation::uint64, QString>{};
    const QVector<graphics::renderer::DirectLight> lights = SortLightingViewLights(mPreview.GetExtractionLights());
    mLightingTargetsKey.clear();
    for (const graphics::renderer::DirectLight& light : lights)
    {
        mLightingTargetsKey += QStringLiteral("%1=%2;").arg(light.SourceId).arg(GetLightingViewLightName(light, names));
    }
    foundation::int32 lightingIndex = -1;
    if (mPreview.CanShowLightingViews() == true)
    {
        mTargets->insertSeparator(mTargets->count());
        const auto addLightingView = [this](const LightingViewTarget& target) {
            const foundation::int32 item = mTargets->count();
            mTargets->addItem(target.Name);
            mTargets->setItemData(item, static_cast<foundation::int32>(target.View), LIGHTING_VIEW_ROLE);
            mTargets->setItemData(item, QVariant::fromValue<qulonglong>(target.SourceId), LIGHTING_LIGHT_ROLE);
            mTargets->setItemData(item, target.Help, Qt::ToolTipRole);
        };
        for (const LightingViewEntry& entry : LIGHTING_VIEW_ENTRIES)
        {
            addLightingView(
            {
                .Name = QStringLiteral("Lighting: %1").arg(QLatin1StringView(entry.Name)),
                .Help = QLatin1StringView(entry.Help),
                .View = entry.View,
            });
        }
        for (const graphics::renderer::DirectLight& light : lights)
        {
            const QString name = GetLightingViewLightName(light, names);
            addLightingView(
            {
                .Name = QStringLiteral("Light: %1").arg(name),
                .Help = QStringLiteral("Shadowed light from %1 alone.").arg(name),
                .View = DeferredLightingView::Light,
                .SourceId = light.SourceId,
            });
            addLightingView(
            {
                .Name = QStringLiteral("Shadow coverage: %1").arg(name),
                .Help = QStringLiteral("The shadow-map test of %1 at each surface: white lit, black shadowed, "
                                       "magenta where no map of it reaches (it lights unshadowed there), cyan "
                                       "when it has no map.")
                            .arg(name),
                .View = DeferredLightingView::ShadowCoverage,
                .SourceId = light.SourceId,
            });
        }
        for (foundation::int32 item = 0; item < mTargets->count() && selectedLightingView.isValid() == true; ++item)
        {
            if (mTargets->itemData(item, LIGHTING_VIEW_ROLE) == selectedLightingView &&
                mTargets->itemData(item, LIGHTING_LIGHT_ROLE) == selectedLightingLight)
            {
                lightingIndex = item;
            }
        }
    }
    const foundation::int32 index = selectedLightingView.isValid() == true ? lightingIndex
                                    : selectedDebugView.isValid() == true
                                        ? mTargets->findData(selectedDebugView, DEBUG_VIEW_ROLE)
                                        : mTargets->findData(selected);
    mTargets->setCurrentIndex(index >= 0 ? index : 0);
    // The blocker hides this change from the fitting connection, so fit the new choice here.
    FitComboBoxToCurrentText(*mTargets);
    mPreview.SetOutputTarget(mTargets->currentData().toString());
    applyLightingView();
}
void WorldGraphView::refreshLightingTargets()
{
    const QHash<foundation::uint64, QString> names =
        mSourceOrNull != nullptr ? mSourceOrNull->Scene().LightNames : QHash<foundation::uint64, QString>{};
    QString key;
    for (const graphics::renderer::DirectLight& light : SortLightingViewLights(mPreview.GetExtractionLights()))
    {
        key += QStringLiteral("%1=%2;").arg(light.SourceId).arg(GetLightingViewLightName(light, names));
    }
    if (key != mLightingTargetsKey)
    {
        refreshTargets();
    }
    refreshLegend();
}
void WorldGraphView::applyLightingView()
{
    if (mSourceOrNull != nullptr)
    {
        const QVariant view = mTargets->currentData(LIGHTING_VIEW_ROLE);
        mSourceOrNull->SetLightingView(view.isValid() == true ? static_cast<DeferredLightingView>(view.toInt())
                                                              : DeferredLightingView::Final,
                                       mTargets->currentData(LIGHTING_LIGHT_ROLE).toULongLong());
    }
    refreshLegend();
}
void WorldGraphView::refreshLegend()
{
    const QVariant view = mTargets->currentData(LIGHTING_VIEW_ROLE);
    if (view.isValid() == false ||
        static_cast<DeferredLightingView>(view.toInt()) != DeferredLightingView::DominantLight)
    {
        mLegend->hide();
        mLegend->clear();
        return;
    }
    const QHash<foundation::uint64, QString> names =
        mSourceOrNull != nullptr ? mSourceOrNull->Scene().LightNames : QHash<foundation::uint64, QString>{};
    QStringList swatches{
        MakeLegendSwatch(graphics::renderer::DEFERRED_LIGHTING_VIEW_SKY_CATEGORY, QStringLiteral("Sky")),
        MakeLegendSwatch(graphics::renderer::DEFERRED_LIGHTING_VIEW_AREA_CATEGORY, QStringLiteral("Area lights")),
    };
    const QVector<graphics::renderer::DirectLight> lights = SortLightingViewLights(mPreview.GetExtractionLights());
    for (qsizetype index = 0; index < lights.size(); ++index)
    {
        swatches.append(MakeLegendSwatch(graphics::renderer::DEFERRED_LIGHTING_VIEW_FIRST_LIGHT_CATEGORY +
                                             static_cast<foundation::uint32>(index),
                                         GetLightingViewLightName(lights[index], names)));
    }
    mLegend->setText(QStringLiteral("Dominant light: ") + swatches.join(QStringLiteral(" &middot; ")));
    mLegend->show();
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
    mHasFeatureLink = false;
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
    if (mWorkspace.GetRenderProblem().isEmpty() == true)
    {
        const QString& warning = mWorkspace.GetRenderWarning();
        if (warning.isEmpty() == false)
        {
            if (text.isEmpty() == false)
            {
                text += QLatin1Char('\n');
            }
            text += warning;
        }
        // The omitted World features are one short line over the picture; the full explanation (the functions to
        // call and the other repairs) is the Error List row it links to, and the tooltip.
        if (mWorkspace.GetOmittedFeatureWarning().isEmpty() == false)
        {
            if (text.isEmpty() == false)
            {
                text += QLatin1Char('\n');
            }
            text += FormatFeatureStatusLine(mWorkspace.GetOmittedFeatureSummary());
            fullText = mWorkspace.GetOmittedFeatureWarning();
            mHasFeatureLink = true;
        }
    }
    if (mHasProblemChip == false && mWorkspace.IsUsingDefaultRenderer() == true && mWorkspace.IsBuiltIn() == false)
    {
        if (text.isEmpty() == false)
        {
            text += QLatin1Char('\n');
        }
        text += QStringLiteral("Engine default renderer fallback.");
    }
    // The view lets pointer input through to the viewport that navigates; only while the chip or
    // the statistics overlay can be clicked does it take presses, and a press outside those
    // widgets is ignored so it still reaches the viewport.
    updatePointerTransparency();
    mStatus->setText(text);
    mStatus->setToolTip(fullText);
    mStatus->setVisible(text.isEmpty() == false);
    Q_EMIT StatusChanged(text);
}
QString WorldGraphView::GetProblemChipLink()
{
    return QStringLiteral("Show in Error List");
}
QString WorldGraphView::FormatFeatureStatusLine(const QString& summary)
{
    return QStringLiteral("%1 · Details").arg(summary);
}
void WorldGraphView::ActivateStatusLink()
{
    if (mHasProblemChip == true)
    {
        mWorkspace.RequestErrorList();
    }
    else if (mHasFeatureLink == true)
    {
        mWorkspace.RequestErrorListEntry(QString::fromLatin1(graphics::renderer::WORLD_FEATURE_WARNING_CODE));
    }
}
void WorldGraphView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && HasStatusLink() == true && mStatus->isVisible() == true &&
        mStatus->geometry().contains(event->position().toPoint()) == true)
    {
        ActivateStatusLink();
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
