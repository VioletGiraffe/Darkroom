#include "Theme/Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

// Warm dark ramp from the mockup: neutrals lean warm brown/taupe (R > G > B, hue ~33 deg) rather than the
// yellow-green/olive cast a R=G grey would give. Blue (Accent*) stays the only cool hue, as the selection accent.
static constexpr Theme::ThemeColors Dark{
	.StarButtonDefault = "#443d35",         // a step darker (toward bg) than TextTertiary
	.StarButtonHoverUnchecked = "#a89e90",  // == InstructionText
	.StarButtonCheckedHover = "#16120d",    // == ThumbnailMatte (recessed tone) - nearly invisible on dark bg
	.MutedText = "#7b7164",
	.InstructionText = "#a89e90",
	.BackgroundPrimary = "#1f1b16",
	.TextPrimary = "#ece5da",
	.BorderSubtle = "#34302b",
	.BorderControl = "#4a453f",
	.AccentBorder = "#5aa0e8",
	.AccentText = "#8fc3f0",
	.AccentBg = "#22303f",
	.ThumbnailMatte = "#16120d",
};

// Warm paper light ramp, the sibling of the dark ramp above - same source, not a derivative of the old greys.
static constexpr Theme::ThemeColors Light{
	.StarButtonDefault = "#d0d0cc",         // a step lighter (toward bg) than TextTertiary
	.StarButtonHoverUnchecked = "#5f5e5a",  // == InstructionText
	.StarButtonCheckedHover = "#f3f1ea",    // == mockup's background-secondary - nearly invisible on light bg
	.MutedText = "#8a8980",
	.InstructionText = "#5f5e5a",
	.BackgroundPrimary = "#faf5f0",   // warm off-white surface — lighter than BackgroundSecondary/ThumbnailMatte,
	                                  // just a visible step below a pure-white page/desktop
	.TextPrimary = "#1a1a18",
	.BorderSubtle = "#e3e3e2",
	.BorderControl = "#cbcbca",
	.AccentBorder = "#378add",
	.AccentText = "#185fa5",
	.AccentBg = "#e6f1fb",
	.ThumbnailMatte = "#e7e4db",
};

const Theme::ThemeColors& Theme::current()
{
	const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
	return dark ? Dark : Light;
}
