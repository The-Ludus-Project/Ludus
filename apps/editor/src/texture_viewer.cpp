#include "internal/texture_viewer.h"
#include "design_tokens.h"

#include "internal/render_graph_preview.h"
#include "internal/render_graph_targets.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <ludus/foundation/profiling/profiling.hpp>

#include <cmath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 NAME_ROLE = Qt::UserRole;
/// Range spin boxes: wide enough for HDR values, with depth precision.
constexpr float64 RANGE_LIMIT = 65504.0;
constexpr int32 RANGE_DECIMALS = 6;
constexpr float64 RANGE_STEP = 0.01;
constexpr float64 EXPOSURE_LIMIT = 16.0;
constexpr float64 EXPOSURE_STEP = 0.5;
constexpr int32 EXPOSURE_DECIMALS = 1;
/// One wheel notch (120 eighths of a degree) doubles or halves the zoom.
constexpr float32 WHEEL_NOTCH = 120.0F;
constexpr int32 MINIMUM_CANVAS_EXTENT = 160;
constexpr int32 LIST_WIDTH = 220;
/// A press that moves less than this is a click, not a pan.
constexpr float64 CLICK_SLOP = 3.0;
constexpr QRgb BACKGROUND_COLOUR = design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true);
constexpr QRgb FRAME_COLOUR = qRgb(80, 88, 96);

struct ChannelChoice final
{
    const char* Label;
    TextureChannel Channel;
};
constexpr ChannelChoice CHANNELS[] = {
    {"RGBA", TextureChannel::Rgba},
    {"R", TextureChannel::Red},
    {"G", TextureChannel::Green},
    {"B", TextureChannel::Blue},
    {"A", TextureChannel::Alpha},
    {"Luminance", TextureChannel::Luminance},
};
} // namespace

TextureViewer::TextureViewer(QWidget* parentOrNull)
    : QWidget(parentOrNull), mTargets(new QListWidget(this)), mChannel(new QComboBox(this)),
      mRange(new QComboBox(this)), mMinimum(new QDoubleSpinBox(this)), mMaximum(new QDoubleSpinBox(this)),
      mExposure(new QDoubleSpinBox(this)), mExposureLabel(new QLabel(QStringLiteral("Exposure (stops)"), this)),
      mInfo(new QLabel(this)), mProbe(new QLabel(this)), mStatus(new QLabel(this)), mCanvas(new TextureCanvas(this))
{
    setObjectName(QStringLiteral("renderGraphTextureViewer"));
    mTargets->setObjectName(QStringLiteral("textureViewerTargets"));
    mChannel->setObjectName(QStringLiteral("textureViewerChannel"));
    mRange->setObjectName(QStringLiteral("textureViewerRange"));
    mMinimum->setObjectName(QStringLiteral("textureViewerMinimum"));
    mMaximum->setObjectName(QStringLiteral("textureViewerMaximum"));
    mExposure->setObjectName(QStringLiteral("textureViewerExposure"));
    mInfo->setObjectName(QStringLiteral("textureViewerInfo"));
    mProbe->setObjectName(QStringLiteral("textureViewerProbe"));
    mStatus->setObjectName(QStringLiteral("textureViewerStatus"));
    for (const ChannelChoice& choice : CHANNELS)
    {
        mChannel->addItem(QString::fromLatin1(choice.Label), static_cast<int32>(choice.Channel));
    }
    mRange->addItem(QStringLiteral("Manual"), static_cast<int32>(TextureRangeMode::Manual));
    mRange->addItem(QStringLiteral("Auto min/max"), static_cast<int32>(TextureRangeMode::Automatic));
    for (QDoubleSpinBox* box : {mMinimum, mMaximum})
    {
        box->setRange(-RANGE_LIMIT, RANGE_LIMIT);
        box->setDecimals(RANGE_DECIMALS);
        box->setSingleStep(RANGE_STEP);
    }
    mMaximum->setValue(1);
    mExposure->setRange(-EXPOSURE_LIMIT, EXPOSURE_LIMIT);
    mExposure->setSingleStep(EXPOSURE_STEP);
    mExposure->setDecimals(EXPOSURE_DECIMALS);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mProbe->setFont(fixed);
    mProbe->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mInfo->setWordWrap(true);
    mStatus->setWordWrap(true);
    mTargets->setMinimumWidth(LIST_WIDTH);
    mTargets->setMaximumWidth(LIST_WIDTH * 2);

    QWidget* controls = new QWidget(this);
    QFormLayout* form = new QFormLayout(controls);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(QStringLiteral("Channel"), mChannel);
    form->addRow(QStringLiteral("Range"), mRange);
    QHBoxLayout* limits = new QHBoxLayout;
    limits->addWidget(mMinimum);
    limits->addWidget(mMaximum);
    form->addRow(QStringLiteral("Min / max"), limits);
    form->addRow(mExposureLabel, mExposure);
    QVBoxLayout* side = new QVBoxLayout;
    side->addWidget(new QLabel(QStringLiteral("Textures"), this));
    side->addWidget(mTargets, 1);
    side->addWidget(controls);
    QVBoxLayout* view = new QVBoxLayout;
    view->addWidget(mInfo);
    view->addWidget(mCanvas, 1);
    view->addWidget(mProbe);
    view->addWidget(mStatus);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->addLayout(side);
    layout->addLayout(view, 1);

    connect(mTargets, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
        if (mIsUpdatingControls == false && current != nullptr)
        {
            SelectTarget(current->data(NAME_ROLE).toString());
        }
    });
    const auto changed = [this]() {
        if (mIsUpdatingControls == false)
        {
            refreshDisplay();
        }
    };
    connect(mChannel, &QComboBox::currentIndexChanged, this, changed);
    connect(mRange, &QComboBox::currentIndexChanged, this, changed);
    connect(mMinimum, &QDoubleSpinBox::valueChanged, this, changed);
    connect(mMaximum, &QDoubleSpinBox::valueChanged, this, changed);
    connect(mExposure, &QDoubleSpinBox::valueChanged, this, changed);
    connect(mCanvas, &TextureCanvas::TexelHovered, this, [this](uint32 x, uint32 y) {
        if (mHasPinnedProbe == false)
        {
            showProbe(x, y, false);
        }
    });
    connect(mCanvas, &TextureCanvas::TexelClicked, this, &TextureViewer::ProbeTexel);
    refreshTargets();
}

TextureViewer::~TextureViewer()
{
    SetPreview(nullptr);
}

void TextureViewer::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (mPreview != nullptr)
    {
        mPreview->SetInspectionEnabled(true);
    }
}

void TextureViewer::hideEvent(QHideEvent* event)
{
    if (mPreview != nullptr)
    {
        mPreview->SetInspectionEnabled(false);
    }
    QWidget::hideEvent(event);
}

void TextureViewer::SetPreview(RenderGraphPreview* previewOrNull)
{
    if (mPreview != nullptr)
    {
        mPreview->SetInspectionEnabled(false);
        disconnect(mPreview, nullptr, this, nullptr);
    }
    mPreview = previewOrNull;
    if (mPreview != nullptr)
    {
        connect(mPreview, &RenderGraphPreview::TargetsChanged, this, &TextureViewer::refreshTargets);
        connect(mPreview, &RenderGraphPreview::TargetInspected, this, &TextureViewer::refreshTexture);
        mPreview->SetInspectionEnabled(isVisible());
        mPreview->SetInspectedTarget(mSelected);
    }
    refreshTargets();
}

void TextureViewer::SelectTarget(const QString& name)
{
    if (name != mSelected)
    {
        mHasPinnedProbe = false;
    }
    mSelected = name;
    if (mPreview != nullptr)
    {
        mPreview->SetInspectedTarget(name);
    }
    refreshTargets();
}

bool TextureViewer::SelectCanvasNode(const QString& graphId, const QString& nodeId)
{
    if (mPreview == nullptr)
    {
        return false;
    }
    const QStringList names = ResolveCanvasTargets(mPreview->GetTargetCatalogue(), graphId, nodeId);
    if (names.isEmpty() == true)
    {
        return false;
    }
    // Reselecting a node cycles through its attachments (a Raster node's colour, then depth).
    const qsizetype current = names.indexOf(mSelected);
    SelectTarget(names.at(current < 0 ? 0 : (current + 1) % names.size()));
    return true;
}

void TextureViewer::ProbeTexel(uint32 x, uint32 y)
{
    mHasPinnedProbe = true;
    mPinnedX = x;
    mPinnedY = y;
    showProbe(x, y, true);
}

void TextureViewer::SetDisplaySettings(const TextureDisplaySettings& settings)
{
    mIsUpdatingControls = true;
    for (int32 i = 0; i < mChannel->count(); ++i)
    {
        if (mChannel->itemData(i).toInt() == static_cast<int32>(settings.Channel))
        {
            mChannel->setCurrentIndex(i);
        }
    }
    mRange->setCurrentIndex(settings.Range == TextureRangeMode::Manual ? 0 : 1);
    mMinimum->setValue(static_cast<float64>(settings.Minimum));
    mMaximum->setValue(static_cast<float64>(settings.Maximum));
    mExposure->setValue(static_cast<float64>(settings.ExposureStops));
    mIsUpdatingControls = false;
    refreshDisplay();
}

TextureDisplaySettings TextureViewer::GetDisplaySettings() const
{
    TextureDisplaySettings settings;
    settings.Channel = static_cast<TextureChannel>(mChannel->currentData().toInt());
    settings.Range = static_cast<TextureRangeMode>(mRange->currentData().toInt());
    settings.Minimum = static_cast<float32>(mMinimum->value());
    settings.Maximum = static_cast<float32>(mMaximum->value());
    settings.ExposureStops = static_cast<float32>(mExposure->value());
    return settings;
}

QString TextureViewer::GetInfoText() const
{
    return mInfo->text();
}

QString TextureViewer::GetProbeText() const
{
    return mProbe->text();
}

QString TextureViewer::GetStatusText() const
{
    return mStatus->text();
}

void TextureViewer::refreshTargets()
{
    mIsUpdatingControls = true;
    mTargets->clear();
    const RenderGraphTargetCatalogue* catalogueOrNull = mPreview != nullptr ? &mPreview->GetTargetCatalogue() : nullptr;
    if (catalogueOrNull != nullptr)
    {
        for (const RenderGraphTarget& target : catalogueOrNull->Targets)
        {
            QListWidgetItem* item = new QListWidgetItem(
                QStringLiteral("%1  (%2)")
                    .arg(target.Name, target.IsDepth == true ? QStringLiteral("depth") : QStringLiteral("colour")),
                mTargets);
            item->setData(NAME_ROLE, target.Name);
            item->setToolTip(DescribeTarget(target));
            if (target.IsSurface == true || target.IsBuffer == true)
            {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
            if (target.Name == mSelected)
            {
                mTargets->setCurrentItem(item);
            }
        }
    }
    mIsUpdatingControls = false;
    const qsizetype index = catalogueOrNull != nullptr ? catalogueOrNull->Find(mSelected) : -1;
    if (catalogueOrNull == nullptr || catalogueOrNull->Targets.isEmpty() == true)
    {
        mInfo->setText(QStringLiteral("Preview a runtime generation to list its textures."));
    }
    else if (index < 0)
    {
        mInfo->setText(mSelected.isEmpty() == true
                           ? QStringLiteral("Select a texture here or a Target, Raster or Export node in the canvas.")
                           : QStringLiteral("%1 is not a texture of the active generation.").arg(mSelected));
    }
    else
    {
        mInfo->setText(QStringLiteral("%1 · %2").arg(mSelected, DescribeTarget(catalogueOrNull->Targets[index])));
    }
    if (catalogueOrNull != nullptr && catalogueOrNull->HasNames == false && catalogueOrNull->Targets.isEmpty() == false)
    {
        mInfo->setText(mInfo->text() + QStringLiteral(" (numeric names: %1)")
                                           .arg(catalogueOrNull->NamesProblem.isEmpty() == true
                                                    ? QStringLiteral("no resource names file beside the package")
                                                    : catalogueOrNull->NamesProblem));
    }
}

void TextureViewer::refreshTexture()
{
    if (mPreview == nullptr)
    {
        return;
    }
    const bool hasEncodingChanged =
        mTexture.IsValid() == false || mPreview->GetInspectedTexture().Encoding != mTexture.Encoding;
    mTexture = mPreview->GetInspectedTexture();
    mStatus->setText(mPreview->GetInspectionStatus().isEmpty() == false
                         ? mPreview->GetInspectionStatus()
                         : (mTexture.IsValid() == true ? QStringLiteral("Generation %1, World revision %2.")
                                                             .arg(mPreview->GetInspectedGeneration())
                                                             .arg(mPreview->GetInspectedRevision())
                                                       : QString()));
    if (hasEncodingChanged == true && mTexture.IsValid() == true)
    {
        // Depth and float data are rarely in [0, 1]; start them on the automatic range.
        TextureDisplaySettings settings = GetDisplaySettings();
        settings.Range =
            mTexture.Encoding == TexelEncoding::Rgba8Unorm ? TextureRangeMode::Manual : TextureRangeMode::Automatic;
        // Normalized colour starts on its full [0, 1] range, not the previous texture's measurement.
        settings.Minimum = 0;
        settings.Maximum = 1;
        if (IsChannelAvailable(mTexture.Encoding, settings.Channel) == false)
        {
            settings.Channel = TextureChannel::Rgba;
        }
        SetDisplaySettings(settings);
        return;
    }
    refreshDisplay();
}

void TextureViewer::refreshDisplay()
{
    if (isVisible() == false)
    {
        return;
    }
    LUDUS_PROFILE_SCOPE(EditorTextureInspectionDisplayConversion);
    refreshControls();
    const TextureDisplaySettings settings = GetDisplaySettings();
    mDisplay = MakeDisplayImage(mTexture, settings, mAppliedRange);
    mCanvas->SetImage(mDisplay);
    if (settings.Range == TextureRangeMode::Automatic && mAppliedRange.HasValues == true)
    {
        // Show the measured range so switching to Manual starts from it.
        mIsUpdatingControls = true;
        mMinimum->setValue(static_cast<float64>(mAppliedRange.Minimum));
        mMaximum->setValue(static_cast<float64>(mAppliedRange.Maximum));
        mIsUpdatingControls = false;
    }
    if (mHasPinnedProbe == true)
    {
        showProbe(mPinnedX, mPinnedY, true);
    }
}

void TextureViewer::refreshControls()
{
    const bool isValid = mTexture.IsValid();
    for (int32 i = 0; i < mChannel->count(); ++i)
    {
        const bool isAvailable =
            isValid == false ||
            IsChannelAvailable(mTexture.Encoding, static_cast<TextureChannel>(mChannel->itemData(i).toInt())) == true;
        mChannel->setItemData(i, isAvailable == true ? QVariant() : QVariant(0), Qt::UserRole - 1);
    }
    const bool isManual = static_cast<TextureRangeMode>(mRange->currentData().toInt()) == TextureRangeMode::Manual;
    mMinimum->setEnabled(isManual);
    mMaximum->setEnabled(isManual);
    // Exposure scales HDR values only; LDR and depth targets hide it.
    const bool hasExposure = isValid == true && IsHighDynamicRange(mTexture.Encoding) == true;
    mExposure->setVisible(hasExposure);
    mExposureLabel->setVisible(hasExposure);
}

void TextureViewer::showProbe(uint32 x, uint32 y, bool isPinned)
{
    const QString text = DescribeTexel(mTexture, x, y);
    mProbe->setText(text.isEmpty() == true ? QString()
                                           : (isPinned == true ? QStringLiteral("Pinned %1").arg(text) : text));
}

TextureCanvas::TextureCanvas(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("textureViewerCanvas"));
    setMinimumSize(MINIMUM_CANVAS_EXTENT, MINIMUM_CANVAS_EXTENT);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(QStringLiteral("Wheel zooms, drag pans, double-click fits, click pins the probe."));
}

void TextureCanvas::SetImage(const QImage& image)
{
    const bool hasResized = image.size() != mImage.size();
    mImage = image;
    if (hasResized == true || mIsFitted == true)
    {
        Fit();
        return;
    }
    update();
}

void TextureCanvas::Fit()
{
    mTransform = FitTexture(static_cast<uint32>(mImage.width()),
                            static_cast<uint32>(mImage.height()),
                            static_cast<float32>(width()),
                            static_cast<float32>(height()));
    mIsFitted = true;
    update();
}

void TextureCanvas::SetTransform(const TextureViewTransform& transform)
{
    mTransform = transform;
    mIsFitted = false;
    update();
}

void TextureCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(BACKGROUND_COLOUR));
    if (mImage.isNull() == true)
    {
        return;
    }
    const float64 zoom = static_cast<float64>(mTransform.Zoom);
    const QRectF target(static_cast<float64>(mTransform.OffsetX),
                        static_cast<float64>(mTransform.OffsetY),
                        static_cast<float64>(mImage.width()) * zoom,
                        static_cast<float64>(mImage.height()) * zoom);
    // Nearest neighbour: every texel stays a sharp square at any zoom.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(target, mImage);
    painter.setPen(QColor(FRAME_COLOUR));
    painter.drawRect(target.adjusted(-1, -1, 0, 0));
}

void TextureCanvas::resizeEvent(QResizeEvent*)
{
    if (mIsFitted == true)
    {
        Fit();
    }
}

void TextureCanvas::wheelEvent(QWheelEvent* event)
{
    const float32 notches = static_cast<float32>(event->angleDelta().y()) / WHEEL_NOTCH;
    const QPointF anchor = event->position();
    SetTransform(ZoomTexture(mTransform,
                             std::exp2(notches),
                             static_cast<float32>(anchor.x()),
                             static_cast<float32>(anchor.y())));
    event->accept();
}

void TextureCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        mIsDragging = true;
        mDragOrigin = event->position();
        mPressPosition = event->position();
    }
}

void TextureCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (mIsDragging == true)
    {
        const QPointF delta = event->position() - mDragOrigin;
        mDragOrigin = event->position();
        TextureViewTransform moved = mTransform;
        moved.OffsetX += static_cast<float32>(delta.x());
        moved.OffsetY += static_cast<float32>(delta.y());
        SetTransform(moved);
    }
    uint32 x = 0;
    uint32 y = 0;
    if (MapToTexel(mTransform,
                   static_cast<uint32>(mImage.width()),
                   static_cast<uint32>(mImage.height()),
                   static_cast<float32>(event->position().x()),
                   static_cast<float32>(event->position().y()),
                   x,
                   y) == true)
    {
        Q_EMIT TexelHovered(x, y);
    }
}

void TextureCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || mIsDragging == false)
    {
        return;
    }
    mIsDragging = false;
    if ((event->position() - mPressPosition).manhattanLength() > CLICK_SLOP)
    {
        return;
    }
    uint32 x = 0;
    uint32 y = 0;
    if (MapToTexel(mTransform,
                   static_cast<uint32>(mImage.width()),
                   static_cast<uint32>(mImage.height()),
                   static_cast<float32>(event->position().x()),
                   static_cast<float32>(event->position().y()),
                   x,
                   y) == true)
    {
        Q_EMIT TexelClicked(x, y);
    }
}

void TextureCanvas::mouseDoubleClickEvent(QMouseEvent*)
{
    Fit();
}
} // namespace ludus::editor
