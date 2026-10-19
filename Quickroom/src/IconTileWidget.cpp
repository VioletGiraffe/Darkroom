#include "IconTileWidget.h"
#include "compiler/compiler_warnings_control.h"
#include "hash/wheathash.hpp"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmapCache>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <utility>

// Mirror ThumbnailWidget's caption band and framed contents margins (border 1 + padding 2).
static constexpr int CAPTION_BAND_HEIGHT = 20;
static constexpr int FRAME_CONTENT_MARGIN = 3;

// Sized as QIcon::paint sizes it, but downscaled smoothly: native icons are often much larger than the tile (256 px on Windows).
// Keyed by pixel content: QFileIconProvider returns a distinct QIcon per file, even for identical icons.
static QPixmap scaledIconPixmap(const QIcon& icon, const QSize& logicalBox, qreal dpr)
{
	const QImage native = icon.pixmap(logicalBox, dpr).toImage();

	QSize logicalSize = native.size();
	if (logicalSize.width() > logicalBox.width() || logicalSize.height() > logicalBox.height())
		logicalSize.scale(logicalBox, Qt::KeepAspectRatio);
	const QSize targetSize = logicalSize * dpr;

	if (targetSize == native.size())
	{
		QPixmap result = QPixmap::fromImage(native);
		result.setDevicePixelRatio(dpr);
		return result;
	}

	const uint64_t contentHash = wheathash64(native.constBits(), static_cast<uint64_t>(native.sizeInBytes()));
	const QString key = QStringLiteral("IconTile_%1_%2_%3_%4x%5@%6")
		.arg(contentHash).arg(static_cast<int>(native.format())).arg(native.width()).arg(targetSize.width()).arg(targetSize.height()).arg(dpr);
	QPixmap result;
	if (QPixmapCache::find(key, &result))
		return result;

	QImage scaled{ targetSize, native.format() };
	if (!ImageProcessing::resize(scaled, native))
		scaled = native.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	scaled.setDevicePixelRatio(dpr);
	result = QPixmap::fromImage(std::move(scaled));
	QPixmapCache::insert(key, result);
	return result;
}

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

	const qreal dpr = devicePixelRatioF();
	if (_scaledIcon.isNull() || _scaledIcon.devicePixelRatio() != dpr) // the tile size is fixed, so only the DPR can change
	{
		const int iconSide = qMin(imageArea.width(), imageArea.height()) * 3 / 5;
		_scaledIcon = scaledIconPixmap(_icon, QSize(iconSide, iconSide), dpr);
	}

	QRect iconRect{ QPoint{}, _scaledIcon.deviceIndependentSize().toSize() };
	iconRect.moveCenter(imageArea.center());
	painter.drawPixmap(iconRect.topLeft(), _scaledIcon);

	const QRect labelRect(content.left() + 2, content.bottom() - CAPTION_BAND_HEIGHT + 1, content.width() - 4, CAPTION_BAND_HEIGHT);
	painter.drawText(labelRect, Qt::AlignCenter, painter.fontMetrics().elidedText(_caption, Qt::ElideMiddle, labelRect.width()));
}
