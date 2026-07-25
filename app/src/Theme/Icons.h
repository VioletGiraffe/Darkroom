#pragma once

#include "Theme/Theme.h"

#include <QIcon>
#include <QPixmap>
#include <QSize>

class QColor;
class QString;

namespace Theme {

// Rasterizes a monochrome SVG's alpha at logicalSize and dpr, tinted with the supplied snapshot color.
[[nodiscard]] QPixmap tintedPixmap(const QString& svgResource, const QColor& color, QSize logicalSize, qreal dpr);

// On-demand QIcon engine: renders at each requested size/DPR and resolves colorField against the current theme.
// Disabled icons use the palette's faded disabled tone.
[[nodiscard]] QIcon tintedIcon(const QString& svgResource, const char* ThemeColors::* colorField);

} // namespace Theme
