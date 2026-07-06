#pragma once

#include "Theme/Theme.h"

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

// Convenience wrapper for QIcon consumers (QPushButton::setIcon, QLineEdit::addAction, combo items). The
// returned icon is backed by an engine that renders + tints the SVG on demand at exactly the size and device
// pixel ratio each request asks for, so it stays crisp at fractional display scaling (125/150/175%) instead
// of being pre-rasterized at a fixed size and then bitmap-scaled. The consumer's own icon size drives it -
// set QAbstractButton::setIconSize for a specific box (e.g. the sort chip's non-square padded 20x15).
//
// The tint is named by a pointer to the ThemeColors field to use (e.g. &ThemeColors::MutedText), resolved
// against Theme::current() on every render rather than captured once - so the icon follows a live light/dark
// switch (the scheme-change handler in Style.cpp repaints, which re-invokes the engine). A disabled control's
// icon instead uses the palette's muted disabled tone, further faded.
[[nodiscard]] QIcon tintedIcon(const QString& svgResource, const char* ThemeColors::* colorField);

} // namespace Theme
