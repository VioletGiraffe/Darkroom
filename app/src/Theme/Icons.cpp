#include "Theme/Icons.h"

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

QPixmap Theme::tintedPixmap(const QString& svgResource, const QColor& color, QSize logicalSize, qreal dpr)
{
	QPixmap pm((QSizeF(logicalSize) * dpr).toSize());
	pm.setDevicePixelRatio(dpr);   // so the painter below works in logical coordinates, and the glyph draws crisp at `dpr`
	pm.fill(Qt::transparent);

	const QRectF logicalRect(QPointF(0, 0), QSizeF(logicalSize));
	{
		QSvgRenderer renderer(svgResource);
		QPainter painter(&pm);
		renderer.render(&painter, logicalRect);
	}
	{
		// Replace the glyph's RGB with the theme color while keeping its per-pixel alpha - a monochrome tint.
		QPainter painter(&pm);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(logicalRect, color);
	}
	return pm;
}

QIcon Theme::tintedIcon(const QString& svgResource, const QColor& color, QSize logicalSize)
{
	QIcon icon;
	icon.addPixmap(tintedPixmap(svgResource, color, logicalSize, 1.0));
	icon.addPixmap(tintedPixmap(svgResource, color, logicalSize, 2.0));   // the high-DPI variant; QIcon picks by device
	return icon;
}

QIcon Theme::tintedIcon(const QString& svgResource, const QColor& color, int logicalSize)
{
	return tintedIcon(svgResource, color, QSize(logicalSize, logicalSize));
}
