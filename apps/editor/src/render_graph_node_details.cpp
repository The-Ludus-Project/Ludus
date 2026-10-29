#include "internal/render_graph_node_details.h"

#include <ludus/foundation/base/types.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 NODE_EDITOR_WIDTH = 900;
constexpr int32 NODE_EDITOR_HEIGHT = 700;
} // namespace
RenderGraphNodeDetails::RenderGraphNodeDetails(QWidget* parentOrNull)
    : QDialog(parentOrNull), mScroll(new QScrollArea(this)), mPath(new QLabel(this)), mError(new QLabel(this))
{
    setObjectName(QStringLiteral("renderGraphNodeEditor"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    mPath->setTextFormat(Qt::PlainText);
    layout->addWidget(mPath);
    mError->setObjectName(QStringLiteral("renderGraphNodeError"));
    mError->setTextFormat(Qt::PlainText);
    mError->setWordWrap(true);
    mError->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    mError->hide();
    layout->addWidget(mError);
    mScroll->setWidgetResizable(true);
    layout->addWidget(mScroll);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(NODE_EDITOR_WIDTH, NODE_EDITOR_HEIGHT);
}
void RenderGraphNodeDetails::Refresh(const QString& title, const QString& path)
{
    setWindowTitle(title);
    mPath->setText(path);
    QTabWidget* previousPartsOrNull = mScroll->findChild<QTabWidget*>(QStringLiteral("renderGraphPipelineParts"));
    const int32 selectedPart = previousPartsOrNull != nullptr ? previousPartsOrNull->currentIndex() : 0;
    QWidget* previousOrNull = mScroll->takeWidget();
    if (previousOrNull != nullptr)
    {
        previousOrNull->blockSignals(true);
        for (QObject* control : previousOrNull->findChildren<QObject*>())
        {
            control->blockSignals(true);
        }
        previousOrNull->hide();
        previousOrNull->setParent(&mRetiredFields);
        previousOrNull->deleteLater();
    }
    QWidget* fields = new QWidget(mScroll);
    QFormLayout* form = new QFormLayout(fields);
    Q_EMIT PopulateRequested(form, fields);
    QTabWidget* partsOrNull = fields->findChild<QTabWidget*>(QStringLiteral("renderGraphPipelineParts"));
    if (partsOrNull != nullptr)
    {
        partsOrNull->setCurrentIndex(selectedPart);
    }
    mScroll->setWidget(fields);
}
void RenderGraphNodeDetails::ShowError(const QString& message)
{
    mError->setText(message);
    mError->setVisible(message.isEmpty() == false);
}
bool RenderGraphNodeDetails::ContainsControl(const QWidget* controlOrNull) const
{
    return controlOrNull != nullptr && mScroll->isAncestorOf(controlOrNull) == true;
}
void RenderGraphNodeDetails::RevealDiagnostic(const QString& field, const QString& description)
{
    if (field.isEmpty() == true)
    {
        return;
    }
    for (QWidget* control : mScroll->findChildren<QWidget*>())
    {
        if (control->property("renderGraphField").toString() != field &&
            control->objectName() != QStringLiteral("renderGraphField_") + field)
        {
            continue;
        }
        control->setProperty("isRenderGraphDiagnosticTarget", true);
        control->setStyleSheet(
            QStringLiteral("*[isRenderGraphDiagnosticTarget=\"true\"] { border: 2px solid #ff9a88; }"));
        for (QTabWidget* tabs : mScroll->findChildren<QTabWidget*>())
        {
            for (int32 index = 0; index < tabs->count(); ++index)
            {
                if (tabs->widget(index) == control || tabs->widget(index)->isAncestorOf(control) == true)
                {
                    tabs->setCurrentIndex(index);
                }
            }
        }
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleDescription(description);
        control->setFocus(Qt::OtherFocusReason);
        const QPointer<QWidget> targetOrNull(control);
        QTimer::singleShot(0, this, [this, targetOrNull]() {
            if (targetOrNull.isNull() == false && ContainsControl(targetOrNull.data()) == true)
            {
                mScroll->ensureWidgetVisible(targetOrNull.data());
            }
        });
        break;
    }
}
} // namespace ludus::editor
