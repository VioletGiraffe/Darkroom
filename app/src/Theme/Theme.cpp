#include "Theme/Theme.h"

#include "assert/advanced_assert.h"
#include "theme/cthemecontroller.h"

#include <algorithm>

namespace {

QColor c(QRgb rgb) { return QColor::fromRgb(rgb); }

Theme::Theme s_active;

} // namespace

const std::vector<Theme::Theme>& Theme::allThemes()
{
	// Warm neutrals with wine-tinted secondary surfaces and blue selection accents.
	static const Theme dark{
		.name = QStringLiteral("Default"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1f1b16), .surface = c(0x1f1b16), .surfaceAlt = c(0x3c2528),
			.text = c(0xece5da), .textDim = c(0xa97e84),
			.button = c(0x1f1b16),
			.accent = c(0x5aa0e8),
			.selectionBg = c(0x26619f), .selectionFg = c(0xece5da),
			.border = c(0x4a453f), .borderSubtle = c(0x34302b), .borderStrong = c(0x78716a),
			.accentText = c(0x8fc3f0), .accentBg = c(0x22303f),
		},
		.starActive = c(0xcc8a1f),
		.starButtonDefault = c(0x443d35),
		.starButtonHoverUnchecked = c(0xbab2a7),
		.starButtonCheckedHover = c(0x16120d),
		.instructionText = c(0xbab2a7),
		.thumbnailMatte = c(0x16120d),
		.readyGreen = c(0x69c06d),
	};
	// Warm paper neutrals with sage-tinted secondary surfaces.
	static const Theme light{
		.name = QStringLiteral("Default"),
		.dark = false,
		.palette = {
			.windowBg = c(0xfaf5f0), .surface = c(0xfaf5f0), .surfaceAlt = c(0xedefe1),
			.text = c(0x241d15), .textDim = c(0x7b815f),
			.button = c(0xfaf5f0),
			.accent = c(0x378add),
			.selectionBg = c(0x4a90da), .selectionFg = c(0x241d15),
			.border = c(0xc7c3bf), .borderSubtle = c(0xdedad5), .borderStrong = c(0x918c85),
			.accentText = c(0x185fa5), .accentBg = c(0xe6f1fb),
		},
		.starActive = c(0xcc8a1f),
		.starButtonDefault = c(0xd0d0cc),
		.starButtonHoverUnchecked = c(0x5f5e5a),
		.starButtonCheckedHover = c(0xf3f1ea),
		.instructionText = c(0x5f5e5a),
		.thumbnailMatte = c(0xe7e4db),
		.readyGreen = c(0x3fa257),
	};

	static const std::vector<Theme> themes{ dark, light };
	return themes;
}

const Theme::Theme& Theme::current()
{
	assert_debug_only(!s_active.name.isEmpty()); // selectActiveTheme() has not run yet
	return s_active;
}

void Theme::selectActiveTheme()
{
	const CThemeController& controller = CThemeController::instance();
	const bool dark = controller.darkActive();
	const QString storedName = controller.themeName(dark);

	const std::vector<Theme>& themes = allThemes();
	const auto matchesStored = [&](const Theme& t) { return t.dark == dark && t.name == storedName; };
	auto it = std::find_if(themes.begin(), themes.end(), matchesStored);
	if (it == themes.end()) // the stored name can outlive its theme in the settings
		it = std::find_if(themes.begin(), themes.end(), [dark](const Theme& t) { return t.dark == dark; });
	assert_r(it != themes.end()); // no theme of this polarity at all - a defective theme table

	s_active = *it; // a copy, so nothing dangles if the table is ever rebuilt
	s_active.palette = resolvedPalette(s_active.palette);
}
