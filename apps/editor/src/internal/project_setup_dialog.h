#pragma once

#include <ludus/foundation/base/types.h>

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QWidget;

namespace ludus::editor
{
struct ProjectSetupOptions
{
    QString Sdk;
    QString WebSdk;
    bool PrepareEngine = false;
    bool DisableWeb = false;
};

/// What the Repair dialog knows about the open project and this editor, used to
/// validate and discover engine installs. Empty fields disable those checks.
struct ProjectSetupContext
{
    QString ExpectedFlavor; ///< SDK flavor required by the saved profile (`Development`, ...).
    QString ToolingRoot;    ///< This editor's tooling checkout; its out/install folders are offered.
};

class ProjectSetupDialog final : public QDialog
{
public:
    explicit ProjectSetupDialog(const QString& projectDirectory,
                                QWidget* parentOrNull = nullptr,
                                const ProjectSetupContext& context = {});
    [[nodiscard]] ProjectSetupOptions Options() const noexcept;

private:
    void Refresh() noexcept;

    enum class EngineChoice : foundation::uint8
    {
        Current,
        Build,
        Installed,
    };
    QComboBox* Engine_ = nullptr;
    QLabel* EngineHelp_ = nullptr;
    QWidget* NativeFields_ = nullptr;
    QLineEdit* Sdk_ = nullptr;
    QComboBox* mDetectedEnginesOrNull = nullptr;
    QLabel* mSdkCheckOrNull = nullptr;
    QString mExpectedFlavor;
    QCheckBox* Browser_ = nullptr;
    QWidget* BrowserFields_ = nullptr;
    QLineEdit* WebSdk_ = nullptr;
    QDialogButtonBox* Buttons_ = nullptr;
};
} // namespace ludus::editor
