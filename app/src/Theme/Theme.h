#pragma once

namespace Theme {
// The active star color never changes between themes (the mockup's own light/dark amber tuning averages to
// about this value; both stay clearly legible against either background without needing a per-theme split).
inline constexpr const char* StarActive      = "#cc8a1f";
inline constexpr const char* StarActiveHover = "#ffd24d";

// Standard corner radius (px) shared by the app's controls and small surfaces: buttons, inputs, combos, list
// frames, the grid card, selection pills, the segmented toggle, the combo popup. The other named radii below
// are each their own deliberate, distinct value - not stragglers that should have used this one.
inline constexpr int ControlRadius = 6;

// QMenu's own outer radius - one notch larger than ControlRadius, so a popup menu reads as a slightly bigger
// floating surface than an inline control.
inline constexpr int MenuRadius = 8;
// QMenu::item - one notch smaller than MenuRadius so a highlighted item nests inside the menu's own rounding
// without its corners poking past the menu's.
inline constexpr int MenuItemRadius = 5;
// SortControl's popped-out popover card - the largest radius in the app, for a free-floating, shadowed,
// dialog-like surface (distinct from any inline control).
inline constexpr int PopoverRadius = 10;
// QCheckBox's indicator box (styled app-wide in Style.cpp). Shares its value with ThumbnailMatteRadius below
// by coincidence only - two unrelated small surfaces that happen to look right at the same radius, not one
// concept; keep them as separate constants so either can move independently.
inline constexpr int CheckboxRadius = 4;
// The recessed matte backdrop behind a borderless single-frame thumbnail (ThumbnailWidget, framed=false). See
// CheckboxRadius above re: the shared value being coincidental.
inline constexpr int ThumbnailMatteRadius = 4;
// QScrollBar's thickness (width when vertical, height when horizontal) and its handle's radius - half the
// thickness, so the handle is always a full pill regardless of thickness.
inline constexpr int ScrollBarThickness = 10;
inline constexpr int ScrollBarHandleRadius = ScrollBarThickness / 2;
// QSlider's groove thickness and round handle diameter (both radii derived, not free: the groove is a full
// pill, the handle a circle). The diameters/thicknesses are border-box sizes; the QSS subtracts its 1px border.
inline constexpr int SliderGrooveThickness = 4;
inline constexpr int SliderGrooveRadius = SliderGrooveThickness / 2;
inline constexpr int SliderHandleDiameter = 16;
inline constexpr int SliderHandleRadius = SliderHandleDiameter / 2;

// Field comments name the mockup's CSS variable (docs/mockups/main-window-sidebar.html) each one is tuned
// to, so a future retune can go back to that source instead of guessing.
struct ThemeColors {
	const char* StarButtonDefault;         // unchecked, idle (no direct mockup token - tuned subtler than MutedText)
	const char* StarButtonHoverUnchecked;  // unchecked, hovered (no direct mockup token - tuned toward InstructionText)
	const char* StarButtonCheckedHover;    // checked (gold), hovered — "warning: will un-star"; tuned to nearly vanish into the surface
	const char* MutedText;          // --color-text-tertiary: counts, placeholders, the least prominent label
	const char* InstructionText;    // --color-text-secondary: explanatory body text, section labels
	const char* BackgroundPrimary;  // --color-background-primary: window/base/button fill (drives the app QPalette)
	const char* BackgroundSecondary; // --color-background-secondary: the raised/selected row fill (sidebar active row) - one
	                                 // hue-shifted step off BackgroundPrimary, so other colors (per-label accent bars) sit on it cleanly
	const char* TextPrimary;        // --color-text-primary: window/base/button text (drives the app QPalette)
	const char* BorderSubtle;       // --color-border-tertiary: faint hairlines - card borders, dividers, panel edges
	const char* BorderMedium;       // --color-border-secondary: floating-surface / container outlines - menus, tooltips, combo popups, list frames
	const char* BorderStrong;       // resting edge of inline interactive controls (buttons, inputs, combo field, slider handle, checkbox, segmented toggle) - tuned toward TextPrimary so they don't read as disabled
	const char* AccentBorder;       // --color-border-info: accent-colored borders/fills/dots (hover border, accent bar, checkbox fill)
	const char* AccentText;         // --color-text-info: text/icon color sitting on an AccentBg-tinted surface
	const char* AccentBg;           // --color-background-info: accent-tinted surface fill (selected/segment fills, highlighted card)
	const char* ThumbnailMatte;     // --color-background-tertiary: recessed image-well backdrop behind a single frame
};

// Returns the palette matching the current system color scheme.
const ThemeColors& current();

} // namespace Theme
