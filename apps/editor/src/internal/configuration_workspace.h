#pragma once

#include <ludus/foundation/config/config.hpp>

#include <QByteArray>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
QT_END_NAMESPACE

namespace ludus::runtime::configuration
{
struct SchemaEntry;
}

namespace ludus::editor
{
// Private offline authoring workspace. It edits one explicitly selected engine
// schema (GameHost by default), never accesses a running game, and saves only
// explicit preference overrides.
class ConfigurationWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit ConfigurationWorkspace(QWidget* parent = nullptr);
    ~ConfigurationWorkspace() override;
    [[nodiscard]] bool Load(const QString& path, foundation::config::Layer layer);
    [[nodiscard]] bool SavePreferences(const QString& path);
    // Save the applied preference draft; the separate Apply command owns the
    // value-entry buffer. Reuse the last path, or request one on the first save.
    void Save(bool choosePath = false);
    [[nodiscard]] bool Edit(std::string_view name, const QString& text, bool reset = false);
    [[nodiscard]] bool ConfirmDiscard();
    // Replace the preview with an empty context for `identity`. Asks before
    // discarding unsaved preferences; returns false when cancelled or unknown.
    [[nodiscard]] bool SelectSchema(std::string_view identity);
    [[nodiscard]] std::string_view GetSchemaIdentity() const noexcept;
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Dirty_;
    }
    [[nodiscard]] const foundation::config::Context* Preview() const noexcept
    {
        return Context_;
    }

private:
    void Render();
    [[nodiscard]] bool resetContext(const runtime::configuration::SchemaEntry& schema);
    bool Report(foundation::config::Status status, const foundation::config::Diagnostic& error = {});
    foundation::config::Context* Context_ = nullptr;
    const runtime::configuration::SchemaEntry* mSchemaOrNull = nullptr;
    QComboBox* mSchemaSelectorOrNull = nullptr;
    QTableWidget* Table_ = nullptr;
    QLineEdit* Value_ = nullptr;
    QLabel* Status_ = nullptr;
    QString PreferencePath_;
    QByteArray PreferenceStamp_;
    bool Dirty_ = false;
};
} // namespace ludus::editor
