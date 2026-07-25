#include "Theme/Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

// Warm neutrals with wine-tinted secondary surfaces and blue selection accents.
static constexpr Theme::ThemeColors Dark{
	.StarButtonDefault = "#443d35",
	.StarButtonHoverUnchecked = "#bab2a7",
	.StarButtonCheckedHover = "#16120d",
	.MutedText = "#a97e84",
	.InstructionText = "#bab2a7",
	.BackgroundPrimary = "#1f1b16",
	.BackgroundSecondary = "#3c2528",
	.TextPrimary = "#ece5da",
	.BorderSubtle = "#34302b",
	.BorderMedium = "#4a453f",
	.BorderStrong = "#78716a",
	.AccentBorder = "#5aa0e8",
	.AccentText = "#8fc3f0",
	.AccentBg = "#22303f",
	.SelectionHighlight = "#26619f",
	.SelectedText = "#ece5da",
	.ThumbnailMatte = "#16120d",
	.ReadyGreen = "#69c06d",
};

// Warm paper neutrals with sage-tinted secondary surfaces.
static constexpr Theme::ThemeColors Light{
	.StarButtonDefault = "#d0d0cc",
	.StarButtonHoverUnchecked = "#5f5e5a",
	.StarButtonCheckedHover = "#f3f1ea",
	.MutedText = "#7b815f",
	.InstructionText = "#5f5e5a",
	.BackgroundPrimary = "#faf5f0",
	.BackgroundSecondary = "#edefe1",
	.TextPrimary = "#241d15",
	.BorderSubtle = "#dedad5",
	.BorderMedium = "#c7c3bf",
	.BorderStrong = "#918c85",
	.AccentBorder = "#378add",
	.AccentText = "#185fa5",
	.AccentBg = "#e6f1fb",
	.SelectionHighlight = "#4a90da",
	.SelectedText = "#241d15",
	.ThumbnailMatte = "#e7e4db",
	.ReadyGreen = "#3fa257",
};

bool Theme::isDark()
{
	return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

const Theme::ThemeColors& Theme::current()
{
	return isDark() ? Dark : Light;
}
