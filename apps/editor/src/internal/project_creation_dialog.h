#pragma once

#include "internal/controller.h"

#include <QDialog>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::editor
{
class ProjectCreationDialog final : public QDialog
{
public:
    explicit ProjectCreationDialog(QWidget* parent = nullptr);
    [[nodiscard]] ProjectCreationOptions Options() const;

private:
    void Update();
    QLineEdit* Name_ = nullptr;
    QLineEdit* Location_ = nullptr;
    QLineEdit* Sdk_ = nullptr;
    QCheckBox* mGitHubCiOrNull = nullptr;
    QLabel* mGitHubCiNoteOrNull = nullptr;
    QLabel* Destination_ = nullptr;
    QLabel* Validation_ = nullptr;
    QPushButton* Create_ = nullptr;
};
} // namespace ludus::editor
