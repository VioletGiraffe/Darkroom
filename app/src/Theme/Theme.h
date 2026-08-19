#pragma once

#include "theme/cbasepalette.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <vector>

namespace Theme {

// Geometry the stylesheet and custom painting share. Per theme, so a theme can reshape and not only
// recolour; the defaults are the house values, and a theme overrides only where it differs.
struct Metrics
{
	int controlRadius = 6;
	int menuRadius = 8;
	int menuItemRadius = 5;
	int popoverRadius = 10;
	int checkboxRadius = 4;
	int thumbnailMatteRadius = 4;
	int scrollBarThickness = 10;
	int sliderGrooveThickness = 4;
	int sliderHandleDiameter = 16;
	// FocusFrameStyle expands the label-tight base focus rect by this amount.
	int focusRectOutset = 2;

	// Derived, so they always follow their base values.
	int scrollBarHandleRadius() const { return scrollBarThickness / 2; }
	int sliderGrooveRadius() const { return sliderGrooveThickness / 2; }
	int sliderHandleRadius() const { return sliderHandleDiameter / 2; }
};

// One selectable look. Pure data; the identity is `name` (unique within its polarity).
struct Theme
{
	QString name;
	bool dark = false;

	CBasePalette palette;
	Metrics metrics;

	// App colours with no cross-app meaning. Independent hues that cannot derive from the palette
	// core, authored per theme so each stays legible against its backgrounds.
	QColor starActive; // the active Best star glyph
	QColor starButtonDefault;
	QColor starButtonHoverUnchecked;
	QColor starButtonCheckedHover;
	QColor instructionText; // explanatory text, a step quieter than palette.textDim
	QColor thumbnailMatte;
	QColor readyGreen;

	QString qssFragment; // optional per-theme QSS, appended after the app sheet so it wins ties
};

// Every selectable theme, both polarities. The settings dialog lists these.
[[nodiscard]] const std::vector<Theme>& allThemes();

// The theme in effect - a copy independent of allThemes() storage. Valid once Style::install() ran.
[[nodiscard]] const Theme& current();

// Re-resolves current() from CThemeController's selection, falling back to the first theme of the
// active polarity when the stored name no longer exists. Style::apply() calls this; nothing else needs to.
void selectActiveTheme();

} // namespace Theme
