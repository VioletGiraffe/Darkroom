#include "Theme/Icons.h"
#include "Theme/Theme.h"

#include <QColor>
#include <QGuiApplication>
#include <QIconEngine>
#include <QPaintDevice>
#include <QPainter>
#include <QPalette>
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
// fixed 1x/2x pixmap that QIcon would then bitmap-scale for a 1.5x screen.
class TintedSvgIconEngine final : public QIconEngine
{
	// Disabled-icon opacity, applied to the palette's (opaque) muted disabled tone (see colorForMode).
	static constexpr float DisabledOpacity = 0.5f;

public:
	TintedSvgIconEngine(QString resource, const char* Theme::ThemeColors::* colorField)
		: _resource(std::move(resource)), _colorField(colorField) {}

	QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
	{
		return Theme::tintedPixmap(_resource, colorForMode(mode), size, scale);
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
	// Resolved per render (not captured at construction): reading Theme::current() / the app palette here is
	// what makes the icon follow a live colour-scheme switch, since the scheme-change repaint re-invokes this.
	[[nodiscard]] QColor colorForMode(QIcon::Mode mode) const
	{
		if (mode == QIcon::Disabled)
		{
			// Both disabled cues combined: the palette's muted disabled tone, then dropped to half opacity
			// (the palette colour is opaque, so setting the alpha == fading it).
			QColor c = QGuiApplication::palette().color(QPalette::Disabled, QPalette::WindowText);
			c.setAlphaF(DisabledOpacity);
			return c;
		}
		return QColor(QString::fromLatin1(Theme::current().*_colorField));
	}

	QString _resource;
	const char* Theme::ThemeColors::* _colorField;
};

} // namespace

QIcon Theme::tintedIcon(const QString& svgResource, const char* ThemeColors::* colorField)
{
	return QIcon(new TintedSvgIconEngine(svgResource, colorField));   // QIcon takes ownership of the engine
}
