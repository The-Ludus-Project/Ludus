#include "internal/project_rendering_settings.h"
#include "internal/render_graph_resources.h"
#include "internal/renderer_workspace.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 SCALE_PRESET_CUSTOM = 5;
constexpr int32 SCALE_THREE_QUARTERS_NUMERATOR = 3;
constexpr int32 SCALE_QUARTER_DENOMINATOR = 4;
constexpr int32 SCALE_PRESET_HALF = 3;
constexpr int32 SCALE_PRESET_QUARTER = 4;
QJsonArray GetScalePreset(int32 index)
{
    switch (index)
    {
        case 0:
            return {1, 1};
        case 1:
            return {SCALE_THREE_QUARTERS_NUMERATOR, SCALE_QUARTER_DENOMINATOR};
        case 2:
            return {2, SCALE_THREE_QUARTERS_NUMERATOR};
        case SCALE_PRESET_HALF:
            return {1, 2};
        case SCALE_PRESET_QUARTER:
            return {1, SCALE_QUARTER_DENOMINATOR};
        default:
            return {};
    }
}
} // namespace

ProjectRenderingSettings::ProjectRenderingSettings(RendererWorkspace& inoutRenderer, QWidget* parentOrNull)
    : QWidget(parentOrNull), mRenderer(inoutRenderer)
{
    setObjectName(QStringLiteral("projectRenderingSettings"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QLabel* heading = new QLabel(QStringLiteral("Rendering resolution"), this);
    layout->addWidget(heading);
    mFields = new QFormLayout;
    layout->addLayout(mFields);
    mProvider = new QComboBox(this);
    mProvider->setObjectName(QStringLiteral("projectResolutionProvider"));
    mFields->addRow(QStringLiteral("Renderer configuration"), mProvider);
    mOutputMode = new QComboBox(this);
    mOutputMode->setObjectName(QStringLiteral("projectOutputMode"));
    mOutputMode->addItems({QStringLiteral("Viewport pixels"), QStringLiteral("Fixed resolution")});
    mFields->addRow(QStringLiteral("Output resolution"), mOutputMode);
    const auto makeSpin = [this](const QString& identity, const QString& label) {
        QSpinBox* spin = new QSpinBox(this);
        spin->setObjectName(identity);
        spin->setRange(1, static_cast<int32>(content::render_graph_format::MAX_TARGET_EXTENT));
        spin->setKeyboardTracking(false);
        mFields->addRow(label, spin);
        return spin;
    };
    mWidth = makeSpin(QStringLiteral("projectOutputWidth"), QStringLiteral("Width (pixels)"));
    mHeight = makeSpin(QStringLiteral("projectOutputHeight"), QStringLiteral("Height (pixels)"));
    mScale = new QComboBox(this);
    mScale->setObjectName(QStringLiteral("projectRenderScale"));
    mScale->addItems({QStringLiteral("100% (native)"),
                      QStringLiteral("75%"),
                      QStringLiteral("66.7% (2/3)"),
                      QStringLiteral("50%"),
                      QStringLiteral("25%"),
                      QStringLiteral("Custom ratio")});
    mFields->addRow(QStringLiteral("Render scale"), mScale);
    mNumerator = makeSpin(QStringLiteral("projectRenderNumerator"), QStringLiteral("Scale numerator"));
    mDenominator = makeSpin(QStringLiteral("projectRenderDenominator"), QStringLiteral("Scale denominator"));
    mStatus = new QLabel(this);
    mStatus->setObjectName(QStringLiteral("projectResolutionStatus"));
    mStatus->setWordWrap(true);
    mStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(mStatus);
    mEnable = new QPushButton(QStringLiteral("Enable resolution controls"), this);
    mEnable->setObjectName(QStringLiteral("projectEnableResolution"));
    layout->addWidget(mEnable);
    connect(mProvider, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
    connect(mOutputMode, &QComboBox::currentIndexChanged, this, [this]() { apply(); });
    connect(mScale, &QComboBox::currentIndexChanged, this, [this]() {
        if (mIsRefreshing == true)
        {
            return;
        }
        const QJsonArray scale = GetScalePreset(mScale->currentIndex());
        mIsRefreshing = true;
        if (scale.isEmpty() == false)
        {
            mNumerator->setValue(scale.at(0).toInt());
            mDenominator->setValue(scale.at(1).toInt());
        }
        mNumerator->setEnabled(mCanEdit == true && scale.isEmpty() == true);
        mDenominator->setEnabled(mCanEdit == true && scale.isEmpty() == true);
        mFields->setRowVisible(mNumerator, scale.isEmpty());
        mFields->setRowVisible(mDenominator, scale.isEmpty());
        mIsRefreshing = false;
        if (scale.isEmpty() == false)
        {
            apply();
        }
    });
    for (QSpinBox* spin : {mWidth, mHeight, mNumerator, mDenominator})
    {
        connect(spin, &QSpinBox::valueChanged, this, [this]() { apply(); });
    }
    connect(mEnable, &QPushButton::clicked, this, [this]() {
        QString error;
        if (mRenderer.EnableResolutionSettings(error) == false)
        {
            mStatus->setText(error);
        }
    });
    connect(&mRenderer, &RendererWorkspace::DocumentChanged, this, [this]() { refresh(); });
    connect(&mRenderer, &RendererWorkspace::CommandsChanged, this, [this]() { refresh(); });
    refresh();
}

void ProjectRenderingSettings::SetProjectEditable(bool canEdit)
{
    mCanEdit = canEdit;
    refresh();
}

bool ProjectRenderingSettings::ContainsFocus() const
{
    const QWidget* focusOrNull = QApplication::focusWidget();
    return focusOrNull != nullptr && (focusOrNull == this || isAncestorOf(focusOrNull) == true);
}

void ProjectRenderingSettings::Commit()
{
    if (mCanEdit == false || mProvider->currentIndex() < 0)
    {
        return;
    }
    for (QSpinBox* spin : {mWidth, mHeight, mNumerator, mDenominator})
    {
        if (spin->isEnabled() == true)
        {
            spin->interpretText();
        }
    }
}

void ProjectRenderingSettings::refresh()
{
    if (mIsRefreshing == true)
    {
        return;
    }
    mIsRefreshing = true;
    const int32 previousIndex = mProvider->currentIndex();
    const bool hasPrevious = previousIndex >= 0 && previousIndex < mSelections.size();
    const RenderGraphResolutionSelection previous =
        hasPrevious == true ? mSelections.at(previousIndex) : RenderGraphResolutionSelection{};
    mSelections = FindRenderGraphResolutionSettings(mRenderer.GetDocument());
    mProvider->clear();
    int32 currentIndex = 0;
    for (qsizetype index = 0; index < mSelections.size(); ++index)
    {
        const RenderGraphResolutionSelection& selection = mSelections.at(index);
        mProvider->addItem(
            QStringLiteral("%1 · %2 · %3").arg(selection.File, selection.Configuration, selection.RenderNode));
        if (selection.File == previous.File && selection.Configuration == previous.Configuration &&
            selection.RenderNode == previous.RenderNode)
        {
            currentIndex = static_cast<int32>(index);
        }
    }
    mProvider->setCurrentIndex(currentIndex);
    QJsonValue extent;
    QJsonArray scale;
    const bool hasPair =
        mSelections.isEmpty() == false &&
        ReadRenderGraphResolutionSettings(mRenderer.GetDocument(), mSelections.at(currentIndex), extent, scale) == true;
    const bool canEdit = mCanEdit == true && hasPair == true && mRenderer.CanEditResolutionSettings() == true;
    mProvider->setEnabled(hasPair);
    mOutputMode->setEnabled(canEdit);
    mScale->setEnabled(canEdit);
    mEnable->setVisible(hasPair == false);
    mEnable->setEnabled(mCanEdit == true && mRenderer.CanEditResolutionSettings() == true);
    const bool isFixed = hasPair == true && extent.isArray() == true;
    mOutputMode->setCurrentIndex(isFixed == true ? 1 : 0);
    mWidth->setValue(isFixed == true ? extent.toArray().at(0).toInt() : DEFAULT_RENDER_WIDTH);
    mHeight->setValue(isFixed == true ? extent.toArray().at(1).toInt() : DEFAULT_RENDER_HEIGHT);
    mFields->setRowVisible(mWidth, isFixed);
    mFields->setRowVisible(mHeight, isFixed);
    mWidth->setEnabled(canEdit == true && isFixed == true);
    mHeight->setEnabled(canEdit == true && isFixed == true);
    int32 preset = SCALE_PRESET_CUSTOM;
    if (hasPair == true)
    {
        for (int32 index = 0; index < SCALE_PRESET_CUSTOM; ++index)
        {
            if (GetScalePreset(index) == scale)
            {
                preset = index;
                break;
            }
        }
    }
    mScale->setCurrentIndex(preset);
    mNumerator->setValue(hasPair == true ? scale.at(0).toInt() : 1);
    mDenominator->setValue(hasPair == true ? scale.at(1).toInt() : 1);
    mFields->setRowVisible(mNumerator, hasPair == true && preset == SCALE_PRESET_CUSTOM);
    mFields->setRowVisible(mDenominator, hasPair == true && preset == SCALE_PRESET_CUSTOM);
    mNumerator->setEnabled(canEdit == true && preset == SCALE_PRESET_CUSTOM);
    mDenominator->setEnabled(canEdit == true && preset == SCALE_PRESET_CUSTOM);
    mStatus->setText(
        hasPair == true
            ? QStringLiteral("Stored in the active renderer graph. Save settings saves its current edits. Viewport "
                             "mode follows drawable pixels; render dimensions round up after scaling. Vendor upscaling "
                             "still requires an available backend.")
            : QStringLiteral("No output/render size pair is exposed. Enable controls for an ordinary "
                             "single-color-target renderer, or connect OutputSize to RenderSize on the graph canvas "
                             "for a custom renderer. Opening settings never changes the graph."));
    mIsRefreshing = false;
}

void ProjectRenderingSettings::apply()
{
    if (mIsRefreshing == true || mCanEdit == false)
    {
        return;
    }
    const int32 index = mProvider->currentIndex();
    if (index < 0 || index >= mSelections.size())
    {
        return;
    }
    const QJsonValue extent = mOutputMode->currentIndex() == 0
                                  ? QJsonValue(QStringLiteral("Viewport"))
                                  : QJsonValue(QJsonArray{mWidth->value(), mHeight->value()});
    const QJsonArray scale{mNumerator->value(), mDenominator->value()};
    QString error;
    if (mRenderer.SetResolutionSettings(mSelections.at(index), extent, scale, error) == false)
    {
        refresh();
        mStatus->setText(error);
    }
}
} // namespace ludus::editor
