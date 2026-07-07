#include "Theme/Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

// Warm dark ramp from the mockup: neutrals lean warm brown/taupe (R > G > B, hue ~33 deg) rather than the
// yellow-green/olive cast a R=G grey would give. Two colors carry the mockup's deliberate hue-shift off that
// neutral ramp - BackgroundSecondary and MutedText, both wine-tinted (hue ~353) so the raised-row surface and
// the least-prominent text have a character of their own instead of being one more grey step. Blue (Accent*)
// is the selection accent - a deliberate divergence from the mockup's leaf-green info tokens (the mockup's own
// selected-row bar is this blue).
static constexpr Theme::ThemeColors Dark{
	.StarButtonDefault = "#443d35",         // a step darker (toward bg) than TextTertiary
	.StarButtonHoverUnchecked = "#bab2a7",  // == InstructionText
	.StarButtonCheckedHover = "#16120d",    // == ThumbnailMatte (recessed tone) - nearly invisible on dark bg
	.MutedText = "#a97e84",                 // wine-tinted tertiary (see ramp comment above)
	.InstructionText = "#bab2a7",
	.BackgroundPrimary = "#1f1b16",
	.BackgroundSecondary = "#3c2528",       // wine-tinted raised-row step up from BackgroundPrimary
	.TextPrimary = "#ece5da",
	.BorderSubtle = "#34302b",              // the mockup's rgba(244,238,228,.10) hairline flattened over BackgroundPrimary
	.BorderMedium = "#4a453f",              // the .20 sibling of BorderSubtle - floating-surface / container outlines
	.BorderStrong = "#78716a",              // inline-control resting edge, tuned toward TextPrimary (not a mockup token)
	.AccentBorder = "#5aa0e8",
	.AccentText = "#8fc3f0",          // text color on accent surface, tuned to be legible on AccentBg
	.AccentBg = "#22303f",
	.SelectionHighlight = "#26619f",  // AccentBorder darkened, saturation kept - the light TextPrimary reads on it (~5:1)
	.SelectedText = "#ece5da",        // == TextPrimary; the selection recolors the background only
	.ThumbnailMatte = "#16120d",
};

// Warm paper light ramp, the sibling of the dark ramp above - same source, not a derivative of the old greys.
// The hue-shifted pair here is sage-tinted (hue ~70) rather than wine: same idea, per-theme tuning.
static constexpr Theme::ThemeColors Light{
	.StarButtonDefault = "#d0d0cc",         // a step lighter (toward bg) than TextTertiary
	.StarButtonHoverUnchecked = "#5f5e5a",  // == InstructionText
	.StarButtonCheckedHover = "#f3f1ea",    // a near-surface warm tone just off BackgroundPrimary - nearly invisible on light bg
	.MutedText = "#7b815f",                 // sage-tinted tertiary (see ramp comment above)
	.InstructionText = "#5f5e5a",
	.BackgroundPrimary = "#faf5f0",   // warm off-white surface — lighter than BackgroundSecondary/ThumbnailMatte,
	                                  // just a visible step below a pure-white page/desktop
	.BackgroundSecondary = "#edefe1", // sage-tinted raised-row step down from BackgroundPrimary
	.TextPrimary = "#241d15",         // near-black but visibly warm (on the brown neutral ramp), not a flat neutral - high contrast on the paper bg
	.BorderSubtle = "#dedad5",        // the mockup's rgba(20,20,16,.12) hairline flattened over BackgroundPrimary
	.BorderMedium = "#c7c3bf",        // the .22 sibling of BorderSubtle - floating-surface / container outlines
	.BorderStrong = "#918c85",        // inline-control resting edge, tuned toward TextPrimary (not a mockup token)
	.AccentBorder = "#378add",
	.AccentText = "#185fa5",
	.AccentBg = "#e6f1fb",
	.SelectionHighlight = "#4a90da",  // AccentBorder kept bright - the dark TextPrimary reads on it (~5:1)
	.SelectedText = "#241d15",        // == TextPrimary; the selection recolors the background only
	.ThumbnailMatte = "#e7e4db",
};

bool Theme::isDark()
{
	return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

const Theme::ThemeColors& Theme::current()
{
	return isDark() ? Dark : Light;
}
