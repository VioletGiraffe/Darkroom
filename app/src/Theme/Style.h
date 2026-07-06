#pragma once

#include <QString>

#include <functional>

class QWidget;

// The app-wide visual language. A single Theme-driven stylesheet installed on the application gives every
// stock control the same non-stock shape (rounded corners, hairline borders, roomy padding, soft hover),
// instead of each widget styling itself inline. This is the shared backbone; widgets that need looks QSS
// can't express (e.g. the sidebar's per-label accent bar, the segmented toggle) add custom painting on top.
namespace Style {

// Builds the application stylesheet from the current Theme palette, applies it to qApp now, and re-applies
// it whenever the system/app color scheme changes, so the globally-styled chrome follows a light/dark
// switch without a restart. Call once, after the initial color scheme is set and before the main window
// is shown.
void install();

// Applies a Theme-derived stylesheet to a single widget now, and re-applies it on every light/dark switch.
// For the few widget-local sheets that bake in Theme colors (a selection tint, an instruction-text color):
// install() rebuilds the *app* sheet on a scheme change but never a widget's own setStyleSheet string, so
// without this the local sheet keeps the old theme's colors until the widget is next reconstructed. The
// re-apply is bound to `widget`'s lifetime; `makeSheet` is re-invoked (reading the fresh Theme) each time.
void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet);

} // namespace Style
