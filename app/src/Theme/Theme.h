#pragma once

namespace Theme {
// Invariant gold remains legible on both palettes.
inline constexpr const char* StarActive      = "#cc8a1f";
inline constexpr const char* StarActiveHover = "#ffd24d";

inline constexpr int ControlRadius = 6;
inline constexpr int MenuRadius = 8;
inline constexpr int MenuItemRadius = 5;
inline constexpr int PopoverRadius = 10;
inline constexpr int CheckboxRadius = 4;
inline constexpr int ThumbnailMatteRadius = 4;
inline constexpr int ScrollBarThickness = 10;
inline constexpr int ScrollBarHandleRadius = ScrollBarThickness / 2;
inline constexpr int SliderGrooveThickness = 4;
inline constexpr int SliderGrooveRadius = SliderGrooveThickness / 2;
inline constexpr int SliderHandleDiameter = 16;
inline constexpr int SliderHandleRadius = SliderHandleDiameter / 2;

// FocusFrameStyle expands the label-tight base focus rect by this amount.
inline constexpr int FocusRectOutset = 2;

struct ThemeColors {
	const char* StarButtonDefault;
	const char* StarButtonHoverUnchecked;
	const char* StarButtonCheckedHover;
	const char* MutedText;
	const char* InstructionText;
	const char* BackgroundPrimary;
	const char* BackgroundSecondary;
	const char* TextPrimary;
	const char* BorderSubtle;
	const char* BorderMedium;
	const char* BorderStrong;
	const char* AccentBorder;
	const char* AccentText;
	const char* AccentBg;
	const char* SelectionHighlight;
	const char* SelectedText;
	const char* ThumbnailMatte;
	const char* ReadyGreen;
};

bool isDark();

const ThemeColors& current();

} // namespace Theme
