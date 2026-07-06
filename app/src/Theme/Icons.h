#pragma once

#include <QIcon>
#include <QPixmap>
#include <QSize>

class QColor;
class QString;

namespace Theme {

// Renders a monochrome SVG resource (e.g. ":/UI/icon_search.svg") tinted to `color`, keeping the glyph's
// antialiased alpha (the SVG's own stroke color is irrelevant - only its coverage is used). These are the
// app's chrome glyphs (sidebar/toolbar), authored to be tinted to a Theme color at the call site. Like the
// per-widget stylesheets, the tint is captured when the pixmap/icon is built, so a live light/dark switch is
// only reflected on the next build (a delegate re-renders on repaint; QIcon consumers rebuild on refresh).
[[nodiscard]] QPixmap tintedPixmap(const QString& svgResource, const QColor& color, QSize logicalSize, qreal dpr);

// Convenience wrapper for QIcon consumers (QPushButton::setIcon, QLineEdit::addAction, combo items). Bakes a
// 1x and a 2x pixmap so the icon stays crisp on any screen without the caller tracking device pixel ratio.
[[nodiscard]] QIcon tintedIcon(const QString& svgResource, const QColor& color, int logicalSize = 16);

// Non-square variant, for a glyph whose SVG carries asymmetric padding (e.g. the sort chip's icon has
// right padding baked in for a gap before the button text). The caller must pass the same size to
// QAbstractButton::setIconSize so the button reserves the padded box rather than clamping to a square.
[[nodiscard]] QIcon tintedIcon(const QString& svgResource, const QColor& color, QSize logicalSize);

} // namespace Theme
