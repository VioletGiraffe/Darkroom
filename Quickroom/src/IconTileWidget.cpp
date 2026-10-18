#include "IconTileWidget.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

// Mirror ThumbnailWidget's caption band and framed contents margins (border 1 + padding 2).
static constexpr int CAPTION_BAND_HEIGHT = 20;
static constexpr int FRAME_CONTENT_MARGIN = 3;

IconTileWidget::IconTileWidget(const QIcon& icon, const QString& caption, int tileSize, QWidget* parent)
	: QWidget(parent), _icon{ icon }, _caption{ caption }
{
	setFixedSize(tileSize, tileSize);
	setContextMenuPolicy(Qt::CustomContextMenu);

	// The central sheet styles this object name; see ThumbnailWidget::applyStyleSettings.
	setObjectName("framedThumbnail");
	setAttribute(Qt::WA_StyledBackground);
	setAttribute(Qt::WA_Hover);
	setContentsMargins(FRAME_CONTENT_MARGIN, FRAME_CONTENT_MARGIN, FRAME_CONTENT_MARGIN, FRAME_CONTENT_MARGIN);
}

void IconTileWidget::setOnActivatedCallback(std::function<void()> handler)
{
	_onActivated = std::move(handler);
}

void IconTileWidget::setOnMouseWheelCallback(std::function<void(int steps)> handler)
{
	_onWheel = std::move(handler);
}

void IconTileWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && _onActivated)
		_onActivated();

	QWidget::mouseDoubleClickEvent(event);
}

void IconTileWidget::wheelEvent(QWheelEvent* event)
{
	const int dy = event->angleDelta().y();
	if (_onWheel && (event->modifiers() & Qt::ControlModifier) && dy != 0)
	{
		_onWheel(dy > 0 ? 1 : -1);
		event->accept();
		return;
	}
	QWidget::wheelEvent(event);
}

void IconTileWidget::paintEvent(QPaintEvent*)
{
	QPainter painter(this);

	const QRect content = contentsRect();
	const QRect imageArea(content.left(), content.top(), content.width(), content.height() - CAPTION_BAND_HEIGHT);

	const int iconSide = qMin(imageArea.width(), imageArea.height()) * 3 / 5;
	QRect iconRect(0, 0, iconSide, iconSide);
	iconRect.moveCenter(imageArea.center());
	_icon.paint(&painter, iconRect);

	const QRect labelRect(content.left() + 2, content.bottom() - CAPTION_BAND_HEIGHT + 1, content.width() - 4, CAPTION_BAND_HEIGHT);
	painter.drawText(labelRect, Qt::AlignCenter, painter.fontMetrics().elidedText(_caption, Qt::ElideMiddle, labelRect.width()));
}
