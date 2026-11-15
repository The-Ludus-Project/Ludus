#include "internal/caption_label.h"

#include <QPainter>
#include <QSizePolicy>

namespace ludus::editor
{
CaptionLabel::CaptionLabel(QWidget* parentOrNull) : QLabel(parentOrNull)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    setProperty("visualRole", QStringLiteral("secondary"));
}

void CaptionLabel::SetCaption(const QString& text)
{
    setText(text);
    setToolTip(text);
}

QSize CaptionLabel::minimumSizeHint() const
{
    return {0, fontMetrics().height()};
}

void CaptionLabel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(contentsRect(),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
}
} // namespace ludus::editor
