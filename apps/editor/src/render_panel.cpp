#include "internal/render_panel.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_load_job.h"
#include "internal/render_scene.h"

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/play_viewport_source.h"

#    include <ludus/graphics/rhi/native_interop.h>
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace ludus::editor
{
RenderPanel::RenderPanel(QWidget* parent) : QWidget(parent), Preview_(this)
{
    setObjectName(QStringLiteral("worldRenderPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    Viewport_ = new PreviewViewport(&Preview_, this);
    Viewport_->setObjectName(QStringLiteral("worldViewport"));
    Viewport_->hide();
    QHBoxLayout* actions = new QHBoxLayout;
    Open_ = new QPushButton(QStringLiteral("Resume viewport"), this);
    Open_->setObjectName(QStringLiteral("worldPreview3D"));
    Open_->setToolTip(QStringLiteral("Validate the World snapshot and project render.preview.json, then resume the "
                                     "embedded viewport."));
    Refresh_ = new QPushButton(QStringLiteral("Reload preview assets"), this);
    Refresh_->setObjectName(QStringLiteral("worldPreviewReload"));
    Stop_ = new QPushButton(QStringLiteral("Pause viewport"), this);
    Stop_->setObjectName(QStringLiteral("worldPreviewStop"));
    auto* details = new QPushButton(QStringLiteral("Camera and diagnostics ▸"), this);
    details->setCheckable(true);
    details->setObjectName(QStringLiteral("worldPreviewDetails"));
    Tint_ = new QPushButton(QStringLiteral("Tint…"), this);
    Tint_->setObjectName(QStringLiteral("worldPickTint"));
    Tint_->setToolTip(QStringLiteral("Load valid preview bindings, select a renderable and finish pending fields. The "
                                     "display color picker supplies linear RGB fields for Apply."));
    // One compact row, so the viewport below keeps the height.
    for (QPushButton* button : {Open_, Refresh_, Stop_, Tint_, details})
    {
        button->setProperty("density", QStringLiteral("compact"));
        actions->addWidget(button);
    }
    actions->addStretch();
    layout->addLayout(actions);
    Status_ = new QLabel(
        QStringLiteral(
            "Valid World fields update the embedded viewport. The project supplies authoring/render.preview.json."),
        this);
    Status_->setObjectName(QStringLiteral("worldPreviewStatus"));
    Status_->setTextFormat(Qt::PlainText);
    Status_->setWordWrap(true);
    layout->addWidget(Status_);
    auto* settings = new QWidget(this);
    auto* form = new QFormLayout(settings);
    Cameras_ = new QComboBox(settings);
    Cameras_->setObjectName(QStringLiteral("worldPreviewCamera"));
    Cameras_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    Cameras_->setMinimumContentsLength(12);
    Cameras_->setAccessibleName(QStringLiteral("Preview camera"));
    Cameras_->addItem(QStringLiteral("Editor camera (does not edit the world)"), QString());
    form->addRow(QStringLiteral("View through"), Cameras_);
    struct SliderSpec final
    {
        QString Name;
        QString Label;
        foundation::int32 Low, High, Value;
    };
    const auto slider = [settings, form](const SliderSpec& spec) {
        auto* control = new QSlider(Qt::Horizontal, settings);
        control->setObjectName(spec.Name);
        control->setAccessibleName(spec.Label);
        control->setRange(spec.Low, spec.High);
        control->setValue(spec.Value);
        form->addRow(spec.Label, control);
        return control;
    };
    Yaw_ = slider({QStringLiteral("worldPreviewYaw"), QStringLiteral("Orbit horizontally"), -180, 180, 25});
    Pitch_ = slider({QStringLiteral("worldPreviewPitch"), QStringLiteral("Orbit vertically"), -80, 80, 20});
    Zoom_ = new QDoubleSpinBox(settings);
    Zoom_->setObjectName(QStringLiteral("worldPreviewZoom"));
    Zoom_->setAccessibleName(QStringLiteral("Focus distance in world units"));
    Zoom_->setDecimals(6);
    Zoom_->setRange(0.000001, 10000000);
    Zoom_->setValue(3);
    form->addRow(QStringLiteral("Focus distance"), Zoom_);
    auto* fit = new QPushButton(QStringLiteral("Frame all visible objects"), settings);
    fit->setObjectName(QStringLiteral("worldPreviewFrameAll"));
    form->addRow(fit);
    Reference_ = new QCheckBox(QStringLiteral("Reference: disable culling, sorting and instancing"), settings);
    Reference_->setObjectName(QStringLiteral("worldPreviewReference"));
    form->addRow(Reference_);
    Trace_ = new QLabel(settings);
    Trace_->setObjectName(QStringLiteral("worldPreviewTrace"));
    Trace_->setTextFormat(Qt::PlainText);
    Trace_->setWordWrap(true);
    Trace_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(Trace_);
    auto* settingsScroll = new QScrollArea(this);
    settingsScroll->setWidget(settings);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setMaximumHeight(120);
    settingsScroll->setMinimumHeight(100);
    settingsScroll->setObjectName(QStringLiteral("worldPreviewSettingsScroll"));
    layout->addWidget(settingsScroll);
    settingsScroll->hide();
    connect(details, &QPushButton::toggled, this, [details, settingsScroll](bool show) {
        settingsScroll->setVisible(show);
        details->setText(show ? QStringLiteral("Camera and diagnostics ▾")
                              : QStringLiteral("Camera and diagnostics ▸"));
    });
    connect(Open_, &QPushButton::clicked, this, [this]() { (void)Refresh(true); });
    connect(Refresh_, &QPushButton::clicked, this, [this]() { (void)Refresh(false); });
    connect(Stop_, &QPushButton::clicked, this, [this]() { Stop(); });
    connect(Tint_, &QPushButton::clicked, this, [this]() {
        Q_EMIT TintRequested(Selected_, Preview_.Scene().RenderableBinding);
    });
    connect(Cameras_, &QComboBox::currentIndexChanged, this, [this]() { navigate(); });
    connect(Yaw_, &QSlider::valueChanged, this, [this](foundation::int32 value) {
        const QJsonObject state = Preview_.Navigation();
        Preview_.Navigate(-1,
                          {static_cast<foundation::float32>(value),
                           static_cast<foundation::float32>(state.value(QStringLiteral("pitch")).toDouble())},
                          static_cast<foundation::float32>(state.value(QStringLiteral("zoom")).toDouble()),
                          Reference_->isChecked());
    });
    connect(Pitch_, &QSlider::valueChanged, this, [this](foundation::int32 value) {
        const QJsonObject state = Preview_.Navigation();
        Preview_.Navigate(-1,
                          {static_cast<foundation::float32>(state.value(QStringLiteral("yaw")).toDouble()),
                           static_cast<foundation::float32>(value)},
                          static_cast<foundation::float32>(state.value(QStringLiteral("zoom")).toDouble()),
                          Reference_->isChecked());
    });
    connect(Zoom_, &QDoubleSpinBox::valueChanged, this, [this](foundation::float64 value) {
        Preview_.SetFocusDistance(static_cast<foundation::float32>(value));
    });
    connect(Reference_, &QCheckBox::toggled, this, [this](bool isReference) {
        const QJsonObject state = Preview_.Navigation();
        Preview_.Navigate(Cameras_->currentIndex() - 1,
                          {static_cast<foundation::float32>(state.value(QStringLiteral("yaw")).toDouble()),
                           static_cast<foundation::float32>(state.value(QStringLiteral("pitch")).toDouble())},
                          static_cast<foundation::float32>(state.value(QStringLiteral("zoom")).toDouble()),
                          isReference);
    });
    connect(fit, &QPushButton::clicked, this, [this]() {
        Preview_.FrameAll();
        Cameras_->setCurrentIndex(0);
    });
    connect(&Preview_, &RenderPreview::Changed, this, &RenderPanel::ShowStatus);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // The game's images are imported onto the viewport's device; a Vulkan import must be
    // destroyed before that device, so Play ends before the device stops.
    connect(&Preview_, &RenderPreview::DeviceStopping, this, [this]() { EndPlay(); });
#endif
    connect(&Preview_, &RenderPreview::Stopped, this, [this]() {
        ++Generation_;
        Again_ = false;
        QueuedLaunch_ = false;
    });
    connect(&Preview_, &RenderPreview::NavigationChanged, this, [this]() {
        const QJsonObject state = Preview_.Navigation();
        const QSignalBlocker yaw(Yaw_);
        const QSignalBlocker pitch(Pitch_);
        const QSignalBlocker zoom(Zoom_);
        const QSignalBlocker camera(Cameras_);
        const QSignalBlocker reference(Reference_);
        Yaw_->setValue(qRound(state.value(QStringLiteral("yaw")).toDouble()));
        Pitch_->setValue(qRound(state.value(QStringLiteral("pitch")).toDouble()));
        Zoom_->setValue(state.value(QStringLiteral("distance")).toDouble());
        const foundation::int32 index = Cameras_->findData(state.value(QStringLiteral("camera")).toString());
        Cameras_->setCurrentIndex(index < 0 ? 0 : index);
        Reference_->setChecked(state.value(QStringLiteral("reference")).toBool());
        updateNavigationControls();
    });
    SetWorld({}, nullptr);
}
RenderPanel::~RenderPanel()
{
    EndPlay();
    delete Job_;
    Job_ = nullptr;
}
namespace
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Short parent for the play session's private socket directory: sockaddr_un paths are at most 103 bytes.
const QString PLAY_SOCKET_PARENT = QStringLiteral("/tmp");
#endif
} // namespace
bool RenderPanel::BeginPlay(QString& outAddress, QString& outReason)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mPlayUnavailableReason.clear();
    if (mIsPlaying == true)
    {
        outReason = QStringLiteral("A game is already playing in this viewport.");
        return false;
    }
    // Game images are imported onto Qt's own device; the readback canvas cannot show them.
    if (PreviewViewport::GetPresentation() != ViewportPresentation::SharedDevice)
    {
        outReason = QStringLiteral("Play in the viewport needs the on-screen GPU viewport; this editor shows the "
                                   "World through an offscreen copy.");
        return false;
    }
    // Linux: Qt's Vulkan device imports the game's memory only with VK_KHR_external_memory_fd,
    // and a viewport that fell back to the readback canvas has no Qt device to import onto.
    if (Viewport_->CanImportHostedImages() == false)
    {
        outReason = QStringLiteral("Play in the viewport needs a GPU viewport whose device can import the game's "
                                   "images (Vulkan VK_KHR_external_memory_fd); this one cannot.");
        return false;
    }
    // The game's frames are imported onto the edit preview's device, the one Qt samples with.
    graphics::rhi::NativeDeviceLease lease;
    if (Preview_.PinDevice(lease) != graphics::rhi::RasterStatus::Ready)
    {
        outReason = QStringLiteral("Play in the viewport needs the World viewport's GPU device, which has not "
                                   "started yet.");
        return false;
    }
    graphics::rhi::NativeDeviceInfo device;
    const graphics::rhi::RasterStatus described = graphics::rhi::GetNativeDeviceInfo(lease, device);
    static_cast<void>(graphics::rhi::ReleaseNativeDevice(lease));
    if (described != graphics::rhi::RasterStatus::Ready)
    {
        outReason = QStringLiteral("Play in the viewport needs the World viewport's GPU device, which is shutting "
                                   "down.");
        return false;
    }
    if (mPlayOrNull == nullptr)
    {
        mPlayOrNull = new PlayViewportSource(this);
        connect(mPlayOrNull, &PlayViewportSource::StateChanged, this, &RenderPanel::updatePlay);
    }
    const foundation::float64 ratio = Viewport_->devicePixelRatioF();
    const foundation::uint32 width =
        static_cast<foundation::uint32>(qMax(1, qRound(static_cast<foundation::float64>(Viewport_->width()) * ratio)));
    const foundation::uint32 height =
        static_cast<foundation::uint32>(qMax(1, qRound(static_cast<foundation::float64>(Viewport_->height()) * ratio)));
    if (mPlayOrNull->Start(PLAY_SOCKET_PARENT,
                           device.Identity,
                           width,
                           height,
                           static_cast<foundation::float32>(ratio)) == false)
    {
        outReason = mPlayOrNull->GetUnavailableReason();
        return false;
    }
    mIsPlaying = true;
    outAddress = mPlayOrNull->GetChannelAddress();
    Status_->setText(QStringLiteral("Starting the game in this viewport. The World stays as edited until its first "
                                    "frame arrives."));
    Q_EMIT PlayStateChanged();
    return true;
#else
    static_cast<void>(outAddress);
    outReason = QStringLiteral("Play in the viewport needs the native GPU preview.");
    return false;
#endif
}
void RenderPanel::EndPlay() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsPlaying == false)
    {
        return;
    }
    // Cleared first: stopping the source reports a state change of its own.
    mIsPlaying = false;
    Viewport_->SetPlayInput(nullptr);
    // Edit frames return; every game frame read ends through the play source.
    Viewport_->SetFrameSource(nullptr);
    Preview_.SetPresentationVisible(true);
    mPlayOrNull->Stop();
    Q_EMIT PlayStateChanged();
#endif
}
void RenderPanel::RequestPlayClose() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsPlaying == true)
    {
        static_cast<void>(mPlayOrNull->GetEndpoint().SendClose());
    }
#endif
}
bool RenderPanel::IsPlaying() const noexcept
{
    return mIsPlaying;
}
QString RenderPanel::GetPlayUnavailableReason() const
{
    return mPlayUnavailableReason;
}
void RenderPanel::SetPlayingElsewhere(bool isPlayingElsewhere)
{
    if (isPlayingElsewhere == mIsPlayingElsewhere)
    {
        return;
    }
    mIsPlayingElsewhere = isPlayingElsewhere;
    if (isPlayingElsewhere == true)
    {
        Status_->setText(QStringLiteral("Playing in a separate window. The viewport shows the World as edited and "
                                        "resumes when the game exits."));
        return;
    }
    if (Preview_.Running() == true)
    {
        ShowStatus();
        return;
    }
    Status_->setText(QStringLiteral("Viewport paused. World data is preserved."));
}
bool RenderPanel::IsPlayingElsewhere() const noexcept
{
    return mIsPlayingElsewhere;
}
void RenderPanel::updatePlay()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsPlaying == false)
    {
        return;
    }
    const PlaySourceState state = mPlayOrNull->GetState();
    if (state == PlaySourceState::Playing && &Viewport_->GetFrameSource() != mPlayOrNull)
    {
        // The game's first frame is pending: switching now replaces the edit image with it.
        Viewport_->SetFrameSource(mPlayOrNull);
        Viewport_->SetPlayInput(&mPlayOrNull->GetEndpoint());
        // The edit preview keeps its device but stops drawing frames nobody sees.
        Preview_.SetPresentationVisible(false);
        Status_->setText(QStringLiteral("Playing in the viewport. Click it to give the game the keyboard; Shift+F1 "
                                        "gives the keyboard back to the editor and Shift+F6 stops the game."));
        Q_EMIT PlayStateChanged();
        return;
    }
    if (state == PlaySourceState::Disconnected || state == PlaySourceState::Unavailable)
    {
        const bool isRefused = state == PlaySourceState::Unavailable;
        const QString reason = isRefused == true
                                   ? mPlayOrNull->GetUnavailableReason()
                                   : QStringLiteral("The game closed its window. The World is shown as edited.");
        // Set before EndPlay reports the end, so the workspace can offer a separate window.
        mPlayUnavailableReason = isRefused == true ? reason : QString();
        EndPlay();
        Status_->setText(reason);
    }
#endif
}
void RenderPanel::SetWorld(const QString& root, const WorldDocument* world)
{
    {
        ++Generation_;
        Again_ = false;
        QueuedLaunch_ = false;
        Stop();
        Stale_.clear();
        (void)Preview_.SetScene({});
        const QSignalBlocker block(Cameras_);
        Cameras_->clear();
        Cameras_->addItem(QStringLiteral("Editor camera (does not edit the world)"), QString());
    }
    Root_ = root;
    World_ = world;
    mShouldUseDefaultCamera = true;
    PreviewWorld_ = world == nullptr ? WorldDocument{} : *world;
    Viewport_->ClearFrame(QStringLiteral("Open a World to render it here."));
    Selected_.clear();
    Pending_ = false;
    UpdateTintAction();
    const bool loaded = world != nullptr && world->Loaded();
    Open_->setEnabled(loaded);
    // A World without an imported scene renders through project bindings or a game adapter. Projects
    // created for scene import have neither, so say what to do instead of reporting a missing asset.
    const RenderSceneSource source = world == nullptr ? RenderSceneSource::None : RenderScene::GetSource(root, *world);
    const bool isAwaitingScene =
        loaded == true && world->SceneLevel().isEmpty() == true && source == RenderSceneSource::None;
    if (source == RenderSceneSource::WorldDemo)
    {
        // Flat-colour boxes carry no surface detail; Solid's neutral material would paint them all white.
        Preview_.SetShading(PreviewShading::Albedo);
    }
    if (isAwaitingScene == true)
    {
        Viewport_->ClearFrame(QStringLiteral("This World has no scene yet. Use Import scene… to render one here."));
    }
    else if (loaded == true && world->SceneLevel().isEmpty() == true)
    {
        (void)Refresh(true);
    }
    Refresh_->setEnabled(loaded);
    if (isAwaitingScene == true)
    {
        Status_->setText(QStringLiteral("No scene imported yet. Import scene… cooks a scene into this World."));
        return;
    }
    Status_->setText(
        loaded ? QStringLiteral("Valid fields update the viewport; Apply commits them. Games own runtime binding.")
               : QStringLiteral("Open a World with project render bindings to preview 3D."));
}
void RenderPanel::ShowSceneFailure(const QString& error)
{
    Viewport_->ClearFrame(QStringLiteral("Scene import failed: ") + error);
    Status_->setText(QStringLiteral("Scene import failed: ") + error);
}
bool RenderPanel::Refresh(bool launch)
{
    if (World_ == nullptr || World_->Loaded() == false)
    {
        return false;
    }
    const bool inheritedLaunch = Job_ != nullptr && Job_->Generation() == Generation_ && Job_->Launch();
    ++Generation_;
    if (Job_ != nullptr)
    {
        Again_ = true;
        QueuedLaunch_ = QueuedLaunch_ || launch || inheritedLaunch;
        return true;
    }
    Status_->setText(QStringLiteral("Validating World preview and assets… Editing remains available."));
    Job_ = new RenderLoadJob(Root_, PreviewWorld_, Generation_, launch, this);
    UpdateTintAction();
    Stop_->setEnabled(true);
    connect(Job_, &RenderLoadJob::Finished, this, [this]() {
        auto* finished = Job_;
        Job_ = nullptr;
        const bool current = finished->Generation() == Generation_;
        const bool success = finished->Success();
        if (current && success)
        {
            Publish(finished->Scene(), finished->Launch());
        }
        else if (current)
        {
            Stale_ = finished->Error();
            Status_->setText(
                Stale_ + (Preview_.Running()
                              ? QStringLiteral(" Previous preview retained; latest World/assets are not represented.")
                              : QString()));
        }
        Stop_->setEnabled(Preview_.Running());
        UpdateTintAction();
        finished->deleteLater();
        if (current)
        {
            Q_EMIT Loaded(success);
        }
        if (Again_)
        {
            const bool launchNext = QueuedLaunch_;
            Again_ = false;
            QueuedLaunch_ = false;
            (void)Refresh(launchNext);
        }
    });
    return true;
}
void RenderPanel::Publish(const RenderScene& scene, bool launch)
{
    Stale_.clear();
    const auto selected = Preview_.Navigation().value(QStringLiteral("camera")).toString();
    {
        const QSignalBlocker block(Cameras_);
        Cameras_->clear();
        Cameras_->addItem(QStringLiteral("Editor camera (does not edit the world)"), QString());
        for (const auto& camera : scene.Cameras)
        {
            Cameras_->addItem(camera.Name + QStringLiteral(" — ") + camera.Id, camera.Id);
        }
        const foundation::int32 index = Cameras_->findData(selected);
        Cameras_->setCurrentIndex(index < 0 ? 0 : index);
    }
    // The first scene of a newly opened World looks through its default camera; later publishes
    // keep whatever the user chose, including the editor camera.
    const bool shouldUseDefaultCamera =
        mShouldUseDefaultCamera == true && selected.isEmpty() == true && scene.DefaultCamera >= 0;
    if (Preview_.SetScene(scene) == false)
    {
        Status_->setText(Preview_.Status());
        return;
    }
    mShouldUseDefaultCamera = false;
    if (shouldUseDefaultCamera == true)
    {
        // Row 0 is the editor camera. SetScene re-synchronized the rows, so choose after it.
        const QSignalBlocker block(Cameras_);
        Cameras_->setCurrentIndex(scene.DefaultCamera + 1);
    }
    navigate();
    if (launch)
    {
        Q_EMIT Starting();
        if (Preview_.Start() == false)
        {
            Viewport_->ClearFrame(Preview_.Status());
        }
        return;
    }
    if (Preview_.Running() == false)
    {
        Status_->setText(
            QStringLiteral(
                "Preview validated: %1 objects, %2 meshes, %3 textures, %4 cameras. Resume viewport starts rendering.")
                .arg(scene.Objects.size())
                .arg(scene.Meshes.size())
                .arg(scene.Textures.size())
                .arg(scene.Cameras.size()));
    }
}
void RenderPanel::SetSelection(const QString& id, bool pending)
{
    Selected_ = id;
    Preview_.SetSelection(id);
    Pending_ = pending;
    UpdateTintAction();
}
void RenderPanel::UpdateTintAction()
{
    const auto binding = Preview_.Scene().RenderableBinding;
    const auto component = binding.value(QStringLiteral("component")).toString();
    Tint_->setEnabled(World_ != nullptr && World_->Loaded() && Selected_.isEmpty() == false && Pending_ == false &&
                      Job_ == nullptr && Stale_.isEmpty() && component.isEmpty() == false &&
                      World_->Entity(Selected_).value(QStringLiteral("components")).toObject().contains(component));
}
void RenderPanel::WorldChanged()
{
    if (World_ != nullptr)
    {
        PreviewDraft(*World_);
    }
}
QWidget* RenderPanel::TakeViewport(QWidget* parent)
{
    Viewport_->setParent(parent);
    return Viewport_;
}
void RenderPanel::PreviewDraft(const WorldDocument& draft)
{
    PreviewWorld_ = draft;
    if (draft.Loaded() && draft.SceneLevel().isEmpty())
    {
        (void)Refresh(Preview_.Running());
    }
}
void RenderPanel::Stop() noexcept
{
    Preview_.Stop();
    ++Generation_;
    Again_ = false;
    QueuedLaunch_ = false;
    Stop_->setEnabled(false);
    Open_->setEnabled(World_ != nullptr && World_->Loaded());
    Status_->setText(QStringLiteral("Viewport paused. World data is preserved."));
}
void RenderPanel::navigate()
{
    const QJsonObject state = Preview_.Navigation();
    Preview_.Navigate(Cameras_->currentIndex() - 1,
                      {static_cast<foundation::float32>(state.value(QStringLiteral("yaw")).toDouble()),
                       static_cast<foundation::float32>(state.value(QStringLiteral("pitch")).toDouble())},
                      static_cast<foundation::float32>(state.value(QStringLiteral("zoom")).toDouble()),
                      state.value(QStringLiteral("reference")).toBool());
}
void RenderPanel::updateNavigationControls()
{
    const foundation::int32 camera = Cameras_->currentIndex() - 1;
    const bool isOrbit = camera < 0;
    Yaw_->setEnabled(isOrbit);
    Pitch_->setEnabled(isOrbit);
    Zoom_->setEnabled(isOrbit);
    if (isOrbit == true)
    {
        Trace_->setText(QStringLiteral("Editor view only. F fits selection; Home fits all while preserving "
                                       "orientation. Saved cameras and transforms are unchanged."));
    }
    else if (camera < Preview_.Scene().Cameras.size())
    {
        const PreviewCamera& rig = Preview_.Scene().Cameras[camera];
        const gameplay::camera::CameraTrace& trace = rig.Trace;
        Trace_->setText(
            QStringLiteral("C0 %1 · evaluated eye (%2, %3, %4) · target %5 · reverse-Z · physical drawable aspect")
                .arg(trace.Recipe == gameplay::camera::CameraRecipe::Fixed ? QStringLiteral("fixed")
                                                                           : QStringLiteral("XY follow"))
                .arg(trace.Published.Pose.Position.X)
                .arg(trace.Published.Pose.Position.Y)
                .arg(trace.Published.Pose.Position.Z)
                .arg(rig.FollowId.isEmpty() ? QStringLiteral("None") : rig.FollowId));
    }
}
void RenderPanel::ShowStatus()
{
    Open_->setEnabled(World_ != nullptr && World_->Loaded() && Preview_.Running() == false);
    Stop_->setEnabled(Preview_.Running() || (Job_ != nullptr && Job_->Generation() == Generation_) || Again_);
    // The playing-elsewhere label stays until the game exits.
    if (Preview_.Status().isEmpty() == false && mIsPlayingElsewhere == false)
    {
        Status_->setText(
            Preview_.Status() +
            (Job_ != nullptr ? QStringLiteral(" Loading updated preview data; previous frame is shown.") : QString()) +
            (Stale_.isEmpty() ? QString()
                              : QStringLiteral(" Previous preview; latest World/assets rejected: ") + Stale_));
    }
}
} // namespace ludus::editor
