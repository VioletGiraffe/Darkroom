#include "Theme/Icons.h"

#include <QColor>
#include <QIconEngine>
#include <QPaintDevice>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

#include <utility>

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

namespace {

// Backs tintedIcon(): renders + tints the SVG on demand at exactly the size and device pixel ratio each
// request carries, so the glyph is crisp at any (including fractional) display scaling - unlike pre-baking a
// fixed 1x/2x pixmap that QIcon would then bitmap-scale for a 1.5x screen. Mode/state are ignored: these are
// flat monochrome chrome glyphs with no disabled/selected variants.
class TintedSvgIconEngine final : public QIconEngine
{
public:
	TintedSvgIconEngine(QString resource, QColor color) : m_resource(std::move(resource)), m_color(std::move(color)) {}

	QPixmap scaledPixmap(const QSize& size, QIcon::Mode, QIcon::State, qreal scale) override
	{
		return Theme::tintedPixmap(m_resource, m_color, size, scale);
	}
	QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
	{
		return scaledPixmap(size, mode, state, 1.0);
	}
	void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
	{
		const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
		painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
	}
	QIconEngine* clone() const override { return new TintedSvgIconEngine(*this); }

private:
	QString m_resource;
	QColor  m_color;
};

} // namespace

QIcon Theme::tintedIcon(const QString& svgResource, const QColor& color)
{
	return QIcon(new TintedSvgIconEngine(svgResource, color));   // QIcon takes ownership of the engine
}
