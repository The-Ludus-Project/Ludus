#include "internal/world_material_creation.h"

#include "internal/world_material_document.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/runtime/resources/scene.h>

#include <initializer_list>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
namespace format = content::scene_format;

constexpr qsizetype RGB_CHANNELS = static_cast<qsizetype>(format::MATERIAL_RGB_CHANNELS);
/// Members of a diffuse created material: model and base colour.
constexpr qsizetype DIFFUSE_MEMBERS = 2;
/// Members of a metallic-roughness created material: model, base colour, metallic and roughness.
constexpr qsizetype METALLIC_ROUGHNESS_MEMBERS = 4;
/// Members of a coated-diffuse created material: model, base colour and the three coat controls.
constexpr qsizetype COATED_DIFFUSE_MEMBERS = 5;
/// Labels of the base colour channels, in channel order.
constexpr foundation::StaticArray<const char*, format::MATERIAL_RGB_CHANNELS> BASE_COLOR_LABELS{
    {"Base colour red", "Base colour green", "Base colour blue"}};

QJsonObject MakeNumberProperty(const char* label, foundation::float64 minimum, foundation::float64 maximum)
{
    return {{QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("label"), QString::fromUtf8(label)},
            {QStringLiteral("min"), minimum},
            {QStringLiteral("max"), maximum}};
}
QJsonObject MakeBaseColorProperty(foundation::usize channel)
{
    return MakeNumberProperty(BASE_COLOR_LABELS[channel], 0, 1);
}
QJsonObject MakeMetallicProperty()
{
    return MakeNumberProperty("Metallic", 0, 1);
}
QJsonObject MakeRoughnessProperty()
{
    return MakeNumberProperty("Roughness", format::MIN_AUTHORED_ROUGHNESS, 1);
}
QJsonObject MakeCoatingAlphaProperty()
{
    return MakeNumberProperty("Coating GGX alpha",
                              static_cast<foundation::float64>(format::LUDUS_COATING_MIN_ALPHA),
                              1);
}
QJsonObject MakeCoatingEtaProperty()
{
    return MakeNumberProperty("Coating IOR",
                              1,
                              static_cast<foundation::float64>(format::MAX_MATERIAL_OPTICAL_CONSTANT));
}
QJsonObject MakeCoatingThicknessProperty()
{
    return MakeNumberProperty("Coating optical thickness",
                              0,
                              static_cast<foundation::float64>(format::MAX_MATERIAL_OPTICAL_CONSTANT));
}

[[nodiscard]] bool IsNumberInRange(const QJsonObject& property, const QJsonValue& value, QString& outError)
{
    const foundation::float64 number = value.toDouble(-1);
    const foundation::float64 minimum = property.value(QStringLiteral("min")).toDouble();
    const foundation::float64 maximum = property.value(QStringLiteral("max")).toDouble();
    if (value.isDouble() == true && foundation::math::IsFinite(number) == true && number >= minimum &&
        number <= maximum)
    {
        return true;
    }
    outError = QStringLiteral("%1: enter a finite value from %2 to %3.")
                   .arg(property.value(QStringLiteral("label")).toString(),
                        QString::number(minimum),
                        QString::number(maximum));
    return false;
}

foundation::float64 ClampToProperty(const QJsonObject& property, foundation::float64 value)
{
    return foundation::math::Clamp(value,
                                   property.value(QStringLiteral("min")).toDouble(),
                                   property.value(QStringLiteral("max")).toDouble());
}

/// Bytes of one RGBA8 texel of a SceneTexture.
constexpr foundation::usize TEXEL_BYTES = 4;

/// Whether every texel of a texture equals its first: the solid-colour swatch an untextured import cooks. Stops at the
/// first differing texel.
[[nodiscard]] bool IsSolidTexture(const resources::SceneTexture& texture)
{
    const foundation::usize bytes = texture.Pixels.GetSize();
    if (bytes < TEXEL_BYTES)
    {
        return false;
    }
    const foundation::uint8* pixels = texture.Pixels.GetData();
    for (foundation::usize offset = TEXEL_BYTES; offset + TEXEL_BYTES <= bytes; offset += TEXEL_BYTES)
    {
        for (foundation::usize channel = 0; channel < TEXEL_BYTES; ++channel)
        {
            if (pixels[offset + channel] != pixels[channel])
            {
                return false;
            }
        }
    }
    return true;
}

QLabel* MakeCreationLabel(const QString& text, QWidget* parent)
{
    QLabel* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}
} // namespace

bool ValidateCreatedMaterial(const QJsonValue& value, QString& outError)
{
    const QJsonObject definition = value.toObject();
    const QString model = definition.value(QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY)).toString();
    const bool isDiffuse = model == QString::fromLatin1(CREATED_MATERIAL_DIFFUSE);
    const bool isMetallicRoughness = model == QString::fromLatin1(CREATED_MATERIAL_METALLIC_ROUGHNESS);
    const bool isCoatedDiffuse = model == QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE);
    const QJsonValue baseColor = definition.value(QString::fromLatin1(CREATED_MATERIAL_BASE_COLOR_KEY));
    const qsizetype expectedMembers = isDiffuse == true          ? DIFFUSE_MEMBERS
                                      : isMetallicRoughness == true ? METALLIC_ROUGHNESS_MEMBERS
                                                                    : COATED_DIFFUSE_MEMBERS;
    if (value.isObject() == false || (isDiffuse == false && isMetallicRoughness == false && isCoatedDiffuse == false) ||
        definition.size() != expectedMembers || baseColor.isArray() == false ||
        baseColor.toArray().size() != RGB_CHANNELS ||
        (isMetallicRoughness == true &&
         (definition.contains(QString::fromLatin1(CREATED_MATERIAL_METALLIC_KEY)) == false ||
          definition.contains(QString::fromLatin1(CREATED_MATERIAL_ROUGHNESS_KEY)) == false)) ||
        (isCoatedDiffuse == true &&
         (definition.contains(QString::fromLatin1(CREATED_MATERIAL_COATING_ALPHA_KEY)) == false ||
          definition.contains(QString::fromLatin1(CREATED_MATERIAL_COATING_ETA_KEY)) == false ||
          definition.contains(QString::fromLatin1(CREATED_MATERIAL_COATING_THICKNESS_KEY)) == false)))
    {
        outError = QStringLiteral("A created material is diffuse with a red, green and blue base colour, "
                                  "metallic-roughness with a base colour, metallic and roughness, or coated-diffuse "
                                  "with a substrate base colour and coat alpha, IOR and thickness. Create it again "
                                  "from the slot.");
        return false;
    }
    const QJsonArray channels = baseColor.toArray();
    for (foundation::usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        if (IsNumberInRange(MakeBaseColorProperty(channel), channels.at(static_cast<qsizetype>(channel)), outError) ==
            false)
        {
            return false;
        }
    }
    if (isMetallicRoughness == true)
    {
        return IsNumberInRange(MakeMetallicProperty(),
                               definition.value(QString::fromLatin1(CREATED_MATERIAL_METALLIC_KEY)),
                               outError) == true &&
               IsNumberInRange(MakeRoughnessProperty(),
                               definition.value(QString::fromLatin1(CREATED_MATERIAL_ROUGHNESS_KEY)),
                               outError) == true;
    }
    if (isCoatedDiffuse == true)
    {
        return IsNumberInRange(MakeCoatingAlphaProperty(),
                               definition.value(QString::fromLatin1(CREATED_MATERIAL_COATING_ALPHA_KEY)),
                               outError) == true &&
               IsNumberInRange(MakeCoatingEtaProperty(),
                               definition.value(QString::fromLatin1(CREATED_MATERIAL_COATING_ETA_KEY)),
                               outError) == true &&
               IsNumberInRange(MakeCoatingThicknessProperty(),
                               definition.value(QString::fromLatin1(CREATED_MATERIAL_COATING_THICKNESS_KEY)),
                               outError) == true;
    }
    return true;
}

QJsonObject MakeCreatedMaterialDraft(const resources::SceneMaterial& material)
{
    QJsonArray baseColor;
    for (const foundation::float32 channel : {material.Reflectance.X, material.Reflectance.Y, material.Reflectance.Z})
    {
        baseColor.append(ClampToProperty(MakeBaseColorProperty(0), static_cast<foundation::float64>(channel)));
    }
    return {{QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY), QString::fromLatin1(CREATED_MATERIAL_METALLIC_ROUGHNESS)},
            {QString::fromLatin1(CREATED_MATERIAL_BASE_COLOR_KEY), baseColor},
            {QString::fromLatin1(CREATED_MATERIAL_METALLIC_KEY),
             ClampToProperty(MakeMetallicProperty(), static_cast<foundation::float64>(material.Metallic))},
            {QString::fromLatin1(CREATED_MATERIAL_ROUGHNESS_KEY),
             ClampToProperty(MakeRoughnessProperty(), static_cast<foundation::float64>(material.Roughness))}};
}

QJsonObject MakeCoatedDiffuseMaterialDraft(const resources::SceneMaterial& material)
{
    QJsonArray baseColor;
    for (const foundation::float32 channel : {material.Reflectance.X, material.Reflectance.Y, material.Reflectance.Z})
    {
        baseColor.append(ClampToProperty(MakeBaseColorProperty(0), static_cast<foundation::float64>(channel)));
    }
    // An already coated-diffuse record seeds its own coat; any other model starts from the shared coating defaults.
    const bool isCoated = material.Scattering.Model == format::MaterialModel::CoatedDiffuse;
    const foundation::float64 alpha =
        isCoated == true ? static_cast<foundation::float64>(material.Scattering.AlphaU)
                         : static_cast<foundation::float64>(format::LUDUS_COATING_MIN_ALPHA);
    const foundation::float64 eta =
        isCoated == true ? static_cast<foundation::float64>(material.Scattering.CoatingEta)
                         : static_cast<foundation::float64>(format::DEFAULT_COATING_ETA);
    const foundation::float64 thickness =
        isCoated == true ? static_cast<foundation::float64>(material.Scattering.CoatingThickness)
                         : static_cast<foundation::float64>(format::DEFAULT_COATING_THICKNESS);
    return {{QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY), QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE)},
            {QString::fromLatin1(CREATED_MATERIAL_BASE_COLOR_KEY), baseColor},
            {QString::fromLatin1(CREATED_MATERIAL_COATING_ALPHA_KEY),
             ClampToProperty(MakeCoatingAlphaProperty(), alpha)},
            {QString::fromLatin1(CREATED_MATERIAL_COATING_ETA_KEY), ClampToProperty(MakeCoatingEtaProperty(), eta)},
            {QString::fromLatin1(CREATED_MATERIAL_COATING_THICKNESS_KEY),
             ClampToProperty(MakeCoatingThicknessProperty(), thickness)}};
}

QStringList DescribeKeptMaterialInputs(const resources::SceneDefinition& scene,
                                       const resources::SceneMaterial& material)
{
    QStringList inputs;
    const bool isSwatch =
        material.Texture < scene.Textures.GetSize() && IsSolidTexture(scene.Textures[material.Texture]) == true;
    if (material.Texture != format::NO_TEXTURE && isSwatch == false)
    {
        inputs.append(QStringLiteral("base colour map"));
    }
    if (material.SurfaceTexture != format::NO_TEXTURE)
    {
        inputs.append(QStringLiteral("normal and surface map"));
    }
    if (material.EmissionTexture != format::NO_TEXTURE)
    {
        inputs.append(QStringLiteral("emission map"));
    }
    if (material.Emission != foundation::math::Vector3{})
    {
        inputs.append(QStringLiteral("emission"));
    }
    if (material.IsAlphaMasked == true)
    {
        inputs.append(QStringLiteral("alpha mask"));
    }
    return inputs;
}

WorldMaterialCreationDialog::WorldMaterialCreationDialog(const MaterialCreationRequest& request, QWidget* parentOrNull)
    : QDialog(parentOrNull), mModel(new QComboBox(this)), mMetallic(new QLineEdit(this)),
      mRoughness(new QLineEdit(this)), mCoatingAlpha(new QLineEdit(this)), mCoatingEta(new QLineEdit(this)),
      mCoatingThickness(new QLineEdit(this)), mError(MakeCreationLabel(QString(), this)),
      mApply(new QPushButton(request.IsEditing == true ? QStringLiteral("Apply changes")
                                                       : QStringLiteral("Create and apply"),
                             this))
{
    setObjectName(QStringLiteral("worldMaterialCreation"));
    setWindowTitle(request.IsEditing == true ? QStringLiteral("Edit created material")
                                             : QStringLiteral("Create material from import"));
    setModal(true);
    QVBoxLayout* layout = new QVBoxLayout(this);
    const QString source = request.SourceName.isEmpty() == true
                               ? request.SourceId
                               : QStringLiteral("%1 · %2").arg(request.SourceName, request.SourceId);
    const bool isCoatedDraft = request.Draft.value(QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY)).toString() ==
                               QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE);
    // A coated-diffuse draft layers a coat over a resolved slot; the other drafts replace an unsupported import.
    const QString summary =
        isCoatedDraft == true
            ? QStringLiteral("Material: %1\nCreate an ordered dielectric coat over a diffuse substrate (its base "
                             "colour). The created material replaces it for %2 World slots and for future imports of "
                             "this source material. Edit the coat parameters in the Material document.")
            : QStringLiteral("Imported material: %1\nIts source model has no Ludus shading implementation. The created "
                             "material replaces it for %2 World slots and for future imports of this source material. "
                             "The source file keeps its unsupported inputs for reimport.");
    QLabel* identity = MakeCreationLabel(summary.arg(source).arg(request.AffectedSlots.size()), this);
    identity->setObjectName(QStringLiteral("worldMaterialCreationSource"));
    layout->addWidget(identity);
    QLabel* affected = MakeCreationLabel(request.AffectedSlots.join(QLatin1Char('\n')), this);
    affected->setObjectName(QStringLiteral("worldMaterialCreationSlots"));
    layout->addWidget(affected);
    QLabel* kept = MakeCreationLabel(
        request.KeptInputs.isEmpty() == true
            ? QStringLiteral("The import kept no maps or emission for this material.")
            : QStringLiteral("Kept from the import: %1.").arg(request.KeptInputs.join(QStringLiteral(", "))),
        this);
    kept->setObjectName(QStringLiteral("worldMaterialCreationKept"));
    layout->addWidget(kept);

    QFormLayout* fields = new QFormLayout;
    mModel->setObjectName(QStringLiteral("worldMaterialCreationModel"));
    mModel->setAccessibleName(QStringLiteral("Created material model"));
    mModel->addItem(QStringLiteral("Metallic-roughness"), QString::fromLatin1(CREATED_MATERIAL_METALLIC_ROUGHNESS));
    mModel->addItem(QStringLiteral("Diffuse"), QString::fromLatin1(CREATED_MATERIAL_DIFFUSE));
    mModel->addItem(QStringLiteral("Coated diffuse (varnish over substrate)"),
                    QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE));
    const QString model = request.Draft.value(QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY)).toString();
    const foundation::int32 modelIndex = mModel->findData(model);
    mModel->setCurrentIndex(modelIndex < 0 ? 0 : modelIndex);
    fields->addRow(QStringLiteral("Model"), mModel);
    const QJsonArray baseColor = request.Draft.value(QString::fromLatin1(CREATED_MATERIAL_BASE_COLOR_KEY)).toArray();
    for (foundation::usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        QLineEdit* input = new QLineEdit(this);
        input->setObjectName(QStringLiteral("worldMaterialCreationBaseColor%1").arg(channel));
        input->setAccessibleName(QString::fromUtf8(BASE_COLOR_LABELS[channel]));
        SetCoatingInput(*input, baseColor.at(static_cast<qsizetype>(channel)));
        fields->addRow(QString::fromUtf8(BASE_COLOR_LABELS[channel]), input);
        mBaseColor[channel] = input;
    }
    // A diffuse draft has no metallic or roughness; offer the imported ones if the author switches models.
    const auto readNumber = [&request](const char* key, const QJsonObject& property, foundation::float64 fallback) {
        const QJsonValue value = request.Draft.value(QString::fromLatin1(key));
        return QJsonValue(value.isDouble() == true ? value.toDouble() : ClampToProperty(property, fallback));
    };
    mMetallic->setObjectName(QStringLiteral("worldMaterialCreationMetallic"));
    mMetallic->setAccessibleName(QStringLiteral("Metallic"));
    SetCoatingInput(*mMetallic, readNumber(CREATED_MATERIAL_METALLIC_KEY, MakeMetallicProperty(), 0));
    fields->addRow(QStringLiteral("Metallic"), mMetallic);
    mRoughness->setObjectName(QStringLiteral("worldMaterialCreationRoughness"));
    mRoughness->setAccessibleName(QStringLiteral("Roughness"));
    SetCoatingInput(*mRoughness, readNumber(CREATED_MATERIAL_ROUGHNESS_KEY, MakeRoughnessProperty(), 1));
    fields->addRow(QStringLiteral("Roughness"), mRoughness);
    // A coated-diffuse draft layers an ordered dielectric coat over the substrate base colour. The coat controls
    // start from the shared coating defaults; a slot or shared coating instance overrides them after creation.
    mCoatingAlpha->setObjectName(QStringLiteral("worldMaterialCreationCoatingAlpha"));
    mCoatingAlpha->setAccessibleName(QStringLiteral("Coating GGX alpha"));
    SetCoatingInput(*mCoatingAlpha,
                    readNumber(CREATED_MATERIAL_COATING_ALPHA_KEY,
                               MakeCoatingAlphaProperty(),
                               static_cast<foundation::float64>(format::LUDUS_COATING_MIN_ALPHA)));
    fields->addRow(QStringLiteral("Coating GGX alpha"), mCoatingAlpha);
    mCoatingEta->setObjectName(QStringLiteral("worldMaterialCreationCoatingEta"));
    mCoatingEta->setAccessibleName(QStringLiteral("Coating IOR"));
    SetCoatingInput(*mCoatingEta,
                    readNumber(CREATED_MATERIAL_COATING_ETA_KEY,
                               MakeCoatingEtaProperty(),
                               static_cast<foundation::float64>(format::DEFAULT_COATING_ETA)));
    fields->addRow(QStringLiteral("Coating IOR"), mCoatingEta);
    mCoatingThickness->setObjectName(QStringLiteral("worldMaterialCreationCoatingThickness"));
    mCoatingThickness->setAccessibleName(QStringLiteral("Coating optical thickness"));
    SetCoatingInput(*mCoatingThickness,
                    readNumber(CREATED_MATERIAL_COATING_THICKNESS_KEY,
                               MakeCoatingThicknessProperty(),
                               static_cast<foundation::float64>(format::DEFAULT_COATING_THICKNESS)));
    fields->addRow(QStringLiteral("Coating optical thickness"), mCoatingThickness);
    layout->addLayout(fields);

    mError->setObjectName(QStringLiteral("worldMaterialCreationError"));
    mError->hide();
    layout->addWidget(mError);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(mApply, QDialogButtonBox::AcceptRole);
    mApply->setObjectName(QStringLiteral("worldConfirmMaterialCreation"));
    mApply->setAutoDefault(false);
    QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setDefault(true);
    cancel->setFocus();
    layout->addWidget(buttons);
    connect(mApply, &QPushButton::clicked, this, &WorldMaterialCreationDialog::requestCreation);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(mModel, &QComboBox::currentIndexChanged, this, &WorldMaterialCreationDialog::updateModel);
    updateModel();
}

void WorldMaterialCreationDialog::ShowError(const QString& message)
{
    mError->setText(message);
    mError->setVisible(message.isEmpty() == false);
}

void WorldMaterialCreationDialog::updateModel()
{
    const QString model = mModel->currentData().toString();
    const bool isMetallicRoughness = model == QString::fromLatin1(CREATED_MATERIAL_METALLIC_ROUGHNESS);
    const bool isCoatedDiffuse = model == QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE);
    mMetallic->setEnabled(isMetallicRoughness);
    mRoughness->setEnabled(isMetallicRoughness);
    mCoatingAlpha->setEnabled(isCoatedDiffuse);
    mCoatingEta->setEnabled(isCoatedDiffuse);
    mCoatingThickness->setEnabled(isCoatedDiffuse);
}

void WorldMaterialCreationDialog::requestCreation()
{
    const QString model = mModel->currentData().toString();
    QJsonObject definition{{QString::fromLatin1(CREATED_MATERIAL_MODEL_KEY), model}};
    QJsonArray baseColor;
    QString error;
    bool hasChanged = false;
    for (foundation::usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        QJsonValue value;
        if (ReadMaterialNumberInput(MakeBaseColorProperty(channel), *mBaseColor[channel], value, hasChanged, error) ==
            false)
        {
            ShowError(error);
            return;
        }
        baseColor.append(value);
    }
    definition.insert(QString::fromLatin1(CREATED_MATERIAL_BASE_COLOR_KEY), baseColor);
    if (model == QString::fromLatin1(CREATED_MATERIAL_METALLIC_ROUGHNESS))
    {
        QJsonValue metallic;
        QJsonValue roughness;
        if (ReadMaterialNumberInput(MakeMetallicProperty(), *mMetallic, metallic, hasChanged, error) == false ||
            ReadMaterialNumberInput(MakeRoughnessProperty(), *mRoughness, roughness, hasChanged, error) == false)
        {
            ShowError(error);
            return;
        }
        definition.insert(QString::fromLatin1(CREATED_MATERIAL_METALLIC_KEY), metallic);
        definition.insert(QString::fromLatin1(CREATED_MATERIAL_ROUGHNESS_KEY), roughness);
    }
    else if (model == QString::fromLatin1(CREATED_MATERIAL_COATED_DIFFUSE))
    {
        QJsonValue alpha;
        QJsonValue eta;
        QJsonValue thickness;
        if (ReadMaterialNumberInput(MakeCoatingAlphaProperty(), *mCoatingAlpha, alpha, hasChanged, error) == false ||
            ReadMaterialNumberInput(MakeCoatingEtaProperty(), *mCoatingEta, eta, hasChanged, error) == false ||
            ReadMaterialNumberInput(MakeCoatingThicknessProperty(),
                                    *mCoatingThickness,
                                    thickness,
                                    hasChanged,
                                    error) == false)
        {
            ShowError(error);
            return;
        }
        definition.insert(QString::fromLatin1(CREATED_MATERIAL_COATING_ALPHA_KEY), alpha);
        definition.insert(QString::fromLatin1(CREATED_MATERIAL_COATING_ETA_KEY), eta);
        definition.insert(QString::fromLatin1(CREATED_MATERIAL_COATING_THICKNESS_KEY), thickness);
    }
    if (ValidateCreatedMaterial(definition, error) == false)
    {
        ShowError(error);
        return;
    }
    ShowError(QString());
    Q_EMIT CreationRequested(definition);
}
} // namespace ludus::editor
