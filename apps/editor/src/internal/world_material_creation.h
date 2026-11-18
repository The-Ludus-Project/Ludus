#pragma once

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::resources
{
struct SceneDefinition;
struct SceneMaterial;
} // namespace ludus::resources

namespace ludus::editor
{
/// Members of a World `scene.material_definitions` entry. The scene cook reads the same names
/// (docs/architecture/content-resources.md, created materials).
inline constexpr char CREATED_MATERIAL_MODEL_KEY[] = "model";
inline constexpr char CREATED_MATERIAL_BASE_COLOR_KEY[] = "base_color";
inline constexpr char CREATED_MATERIAL_METALLIC_KEY[] = "metallic";
inline constexpr char CREATED_MATERIAL_ROUGHNESS_KEY[] = "roughness";
/// `model` of a created Lambertian material: it holds only a base colour.
inline constexpr char CREATED_MATERIAL_DIFFUSE[] = "diffuse";
/// `model` of a created metallic-roughness material: it holds a base colour, metallic and roughness.
inline constexpr char CREATED_MATERIAL_METALLIC_ROUGHNESS[] = "metallic_roughness";

/// Validate one created material: `{model: "diffuse", base_color}` or
/// `{model: "metallic_roughness", base_color, metallic, roughness}`, with a linear RGB base colour in [0, 1], a
/// metallic in [0, 1] and a roughness in [MIN_AUTHORED_ROUGHNESS, 1].
/// @param value The entry to check.
/// @param outError On failure, names the field and its accepted range, or the expected members.
/// @return Whether the entry is a valid created material.
[[nodiscard]] bool ValidateCreatedMaterial(const QJsonValue& value, QString& outError);

/// Start a metallic-roughness created material from an imported record: its reflectance as the base colour, and
/// its metallic and roughness clamped into the created material's ranges.
[[nodiscard]] QJsonObject MakeCreatedMaterialDraft(const resources::SceneMaterial& material);

/// Name the imported inputs a created material keeps: its base colour, surface and emission maps, emission and
/// alpha mask. A solid-colour base texture, the swatch an untextured import cooks, is a constant and is not listed.
/// @param scene The loaded scene that owns the material's textures.
/// @param material A material of scene.
[[nodiscard]] QStringList DescribeKeptMaterialInputs(const resources::SceneDefinition& scene,
                                                     const resources::SceneMaterial& material);

/// What the creation dialog shows and starts from.
struct MaterialCreationRequest final
{
    QString SourceName;        ///< Imported material name.
    QString SourceId;          ///< Imported material resource ID, the key of the created material.
    QStringList AffectedSlots; ///< "Name · entity ID" of every World slot the material replaces.
    QStringList KeptInputs;    ///< DescribeKeptMaterialInputs of the imported record.
    QJsonObject Draft;         ///< Initial created material (ValidateCreatedMaterial accepts it).
    bool IsEditing = false;    ///< The source already has a created material, which Draft holds.
};

/// Modal draft of a created material that replaces an unsupported imported material for every slot that shares it.
/// It names the source identity, the affected slots and the imported inputs it keeps, and its fields start from the
/// imported values. **Create and apply** (or **Apply changes** when editing) checks the fields and emits
/// CreationRequested; a rejected field or edit is reported in the dialog, which stays open with the typed text.
/// Cancel is the default button. GUI thread only.
class WorldMaterialCreationDialog final : public QDialog
{
    Q_OBJECT

public:
    /// @param request Source identity, affected slots and initial draft.
    /// @param parentOrNull Optional owning widget.
    explicit WorldMaterialCreationDialog(const MaterialCreationRequest& request, QWidget* parentOrNull = nullptr);

    /// Report a rejected request beside the fields, keeping them as typed. An empty message hides the report.
    void ShowError(const QString& message);

Q_SIGNALS:
    /// The fields hold a valid created material. Close the dialog with accept() once the edit is applied, or report
    /// a failure with ShowError and leave it open.
    void CreationRequested(const QJsonObject& definition);

private:
    void requestCreation();
    void updateModel();

    QComboBox* mModel;
    foundation::StaticArray<QLineEdit*, content::scene_format::MATERIAL_RGB_CHANNELS> mBaseColor{};
    QLineEdit* mMetallic;
    QLineEdit* mRoughness;
    QLabel* mError;
    QPushButton* mApply;
};
} // namespace ludus::editor
