#include "Theme/Theme.h"

#include "assert/advanced_assert.h"
#include "theme/colorutils.h"
#include "theme/cthemecontroller.h"

#include <algorithm>
#include <cmath>

namespace {

QColor c(QRgb rgb) { return QColor::fromRgb(rgb); }

Theme::Theme s_active;

// Rotates `colour` the short way round toward `towardHue` degrees, keeping saturation and value.
// A negative hue means the target is achromatic and carries no direction.
QColor hueShifted(const QColor& colour, int towardHue, float t)
{
	if (towardHue < 0)
		return colour;

	const float from = colour.hsvHueF() * 360.0f;
	float delta = towardHue - from;
	if (delta > 180.0f)
		delta -= 360.0f;
	else if (delta < -180.0f)
		delta += 360.0f;

	const float shifted = std::fmod(from + delta * t + 360.0f, 360.0f);
	return QColor::fromHsvF(shifted / 360.0f, colour.hsvSaturationF(), colour.valueF());
}

// The library's flat accent tint is sized for a text highlight; the media grid fills a whole card with it.
// Blends the surface toward the accent until the fill separates from the window by TARGET_CONTRAST,
// falling back to the accent itself where no blend reaches it.
QColor accentTint(const CBasePalette& p)
{
	constexpr double TARGET_CONTRAST = 1.31;
	constexpr int STEPS = 50;

	QColor tint = p.surface;
	for (int i = 1; i <= STEPS; ++i)
	{
		tint = ColorUtils::mix(p.surface, p.accent, float(i) / STEPS);
		if (ColorUtils::contrastRatio(tint, p.windowBg) >= TARGET_CONTRAST)
			break;
	}
	return tint;
}

// Fills every app colour left unset, so a theme authors only its palette. Star tones are shared by
// every theme of a polarity; the rest follow the palette.
void resolveAppColors(Theme::Theme& t)
{
	const CBasePalette& p = t.palette;

	if (!t.starActive.isValid())
		t.starActive = c(0xcc8a1f);
	if (!t.starButtonDefault.isValid())
		t.starButtonDefault = t.dark ? c(0x443d35) : c(0xd0d0cc);
	if (!t.starButtonHoverUnchecked.isValid())
		t.starButtonHoverUnchecked = t.dark ? c(0xbab2a7) : c(0x5f5e5a);
	if (!t.starButtonCheckedHover.isValid())
		t.starButtonCheckedHover = t.dark ? c(0x16120d) : c(0xf3f1ea);

	if (!t.instructionText.isValid())
		t.instructionText = ColorUtils::mix(p.text, p.windowBg, 0.27f);
	// A light theme's paper recesses far less than a dark theme's ground before it stops reading as paper.
	if (!t.thumbnailMatte.isValid())
		t.thumbnailMatte = ColorUtils::mix(p.windowBg, Qt::black, t.dark ? 0.33f : 0.08f);
	// A neutral green, pulled a fifth of the way toward the accent so it belongs to the theme.
	if (!t.readyGreen.isValid())
		t.readyGreen = hueShifted(t.dark ? c(0x69c06d) : c(0x3fa257), p.accent.hsvHue(), 0.2f);
}

} // namespace

const std::vector<Theme::Theme>& Theme::allThemes()
{
	static const Theme darkroomDark{
		.name = QStringLiteral("Darkroom"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1f1b16), .surface = c(0x1f1b16), .surfaceAlt = c(0x3c2528),
			.text = c(0xece5da), .textDim = c(0xa97e84),
			.button = c(0x1f1b16),
			.accent = c(0xc5344d),
			.selectionBg = c(0x99293b), .selectionFg = c(0xece5da),
			.border = c(0x4a453f), .borderSubtle = c(0x34302b), .borderStrong = c(0x78716a),
			.accentText = c(0xef9fac), .accentBg = c(0x4a262b),
		},
	};

	static const Theme darkroomLight{
		.name = QStringLiteral("Darkroom"),
		.dark = false,
		.palette = {
			.windowBg = c(0xfaf5f0), .surface = c(0xfaf5f0), .surfaceAlt = c(0xedefe1),
			.text = c(0x241d15), .textDim = c(0x7b815f),
			.button = c(0xfaf5f0),
			.accent = c(0x1f70c1),
			.selectionBg = c(0x4a90da), .selectionFg = c(0x241d15),
			.border = c(0xc7c3bf), .borderSubtle = c(0xdedad5), .borderStrong = c(0x918c85),
			.accentText = c(0x185fa5), .accentBg = c(0xc2dbf4),
		},
	};

	static const Theme classicLight{
		.name = QStringLiteral("Classic"),
		.dark = false,
		.palette = {
			.windowBg = c(0xf3f3f3), .surface = c(0xffffff), .surfaceAlt = c(0xfafafa),
			.text = c(0x16181c), .textDim = c(0x6b7280),
			.button = c(0xfdfdfd),
			.accent = c(0x0d6bc4),
			.selectionBg = c(0x388fec), .selectionFg = c(0x16181c),
			.border = c(0xd0d3d8), .borderSubtle = c(0xe3e5e9),
			.accentFg = c(0xffffff),
			.buttonBorder = c(0xc2c6cc),
		},
	};

	static const Theme classicDark{
		.name = QStringLiteral("Classic"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1f2227), .surface = c(0x191c21), .surfaceAlt = c(0x1c1f24),
			.text = c(0xe6e8ec), .textDim = c(0x98a0ab),
			.button = c(0x262a30),
			.accent = c(0x3b8fe0),
			.selectionBg = c(0x2d5983), .selectionFg = c(0xe6e8ec),
			.border = c(0x33383f), .borderSubtle = c(0x282c32),
			.accentFg = c(0x06121f),
			.buttonBorder = c(0x3c424a),
		},
	};

	// Warm cream, deep amber accent.
	static const Theme honey{
		.name = QStringLiteral("Honey"),
		.dark = false,
		.palette = {
			.windowBg = c(0xf7f0dd), .surface = c(0xfffdf4), .surfaceAlt = c(0xfbf6e7),
			.text = c(0x221c0c), .textDim = c(0x857a58),
			.button = c(0xfdf9ec),
			.accent = c(0x8a6600),
			.selectionBg = c(0xab8619), .selectionFg = c(0x221c0c),
			.border = c(0xdfd3ae), .borderSubtle = c(0xefe7cd),
			.buttonBorder = c(0xd4c79b),
		},
	};

	// Near-black violet, golden yellow accent.
	static const Theme blackoutViolet{
		.name = QStringLiteral("Blackout Violet"),
		.dark = true,
		.palette = {
			.windowBg = c(0x100a17), .surface = c(0x09060c), .surfaceAlt = c(0x0d0814),
			.text = c(0xe9e3f2), .textDim = c(0x9488a8),
			.button = c(0x1a1128),
			.accent = c(0xffc226),
			.selectionBg = c(0x594815), .selectionFg = c(0xe9e3f2),
			.border = c(0x241a36), .borderSubtle = c(0x170f24),
			.accentFg = c(0x1f1800),
			.buttonBorder = c(0x322447),
		},
		// The derived matte is indistinguishable from a window this dark.
		.thumbnailMatte = c(0x030205),
	};

	// Warm-gray paper, dark taxicab-yellow accent.
	static const Theme taxicabLight{
		.name = QStringLiteral("Taxicab"),
		.dark = false,
		.palette = {
			.windowBg = c(0xf4f2ec), .surface = c(0xfffef9), .surfaceAlt = c(0xfbf9f2),
			.text = c(0x1c1a12), .textDim = c(0x7c7866),
			.button = c(0xfbf9f2),
			.accent = c(0x8a6d00),
			.selectionBg = c(0xa78a15), .selectionFg = c(0x1c1a12),
			.border = c(0xd9d4c4), .borderSubtle = c(0xece8da),
			.buttonBorder = c(0xcfc9b6),
		},
	};

	// Neutral ink, bright taxicab yellow.
	static const Theme taxicabDark{
		.name = QStringLiteral("Taxicab"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1c1c1a), .surface = c(0x151514), .surfaceAlt = c(0x1a1a18),
			.text = c(0xe9e7e0), .textDim = c(0x9c9a8f),
			.button = c(0x26251f),
			.accent = c(0xf8ce1c),
			.selectionBg = c(0x5b5417), .selectionFg = c(0xe9e7e0),
			.border = c(0x3a392f), .borderSubtle = c(0x262620),
			.accentFg = c(0x1f1800),
			.buttonBorder = c(0x3f3e33),
		},
	};

	// Navy, hot orange accent. Dark only: the light rendition reads as stock Ubuntu.
	static const Theme forge{
		.name = QStringLiteral("Forge"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1a2233), .surface = c(0x141b29), .surfaceAlt = c(0x172030),
			.text = c(0xe4e8f0), .textDim = c(0x8e9ab2),
			.button = c(0x232d42),
			.accent = c(0xf2683f),
			.selectionBg = c(0x724f3c), .selectionFg = c(0xe4e8f0),
			.border = c(0x303d56), .borderSubtle = c(0x222c40),
			.accentFg = c(0xffffff),
			.buttonBorder = c(0x3a4763),
		},
	};

	// Teal patina on warm sand.
	static const Theme verdigrisLight{
		.name = QStringLiteral("Verdigris"),
		.dark = false,
		.palette = {
			.windowBg = c(0xf3ead9), .surface = c(0xfffcf5), .surfaceAlt = c(0xfaf4e6),
			.text = c(0x231e14), .textDim = c(0x8a7d63),
			.button = c(0xfaf4e6),
			.accent = c(0x0c796c),
			.selectionBg = c(0x499667), .selectionFg = c(0x231e14),
			.border = c(0xddd0b8), .borderSubtle = c(0xeee5d2),
			.accentFg = c(0xffffff),
			.buttonBorder = c(0xd5c8a8),
		},
	};

	static const Theme verdigrisDark{
		.name = QStringLiteral("Verdigris"),
		.dark = true,
		.palette = {
			.windowBg = c(0x262019), .surface = c(0x1d1813), .surfaceAlt = c(0x221c15),
			.text = c(0xece6da), .textDim = c(0xa89c88),
			.button = c(0x322a20),
			.accent = c(0x2fbfa4),
			.selectionBg = c(0x1c6051), .selectionFg = c(0xece6da),
			.border = c(0x453a2c), .borderSubtle = c(0x2e261d),
			.accentFg = c(0x03231c),
			.buttonBorder = c(0x4a3e2e),
		},
	};

	// Olive accent on pale green.
	static const Theme fireflyLight{
		.name = QStringLiteral("Firefly"),
		.dark = false,
		.palette = {
			.windowBg = c(0xeff3e8), .surface = c(0xfdfff7), .surfaceAlt = c(0xf7faee),
			.text = c(0x181d10), .textDim = c(0x717d64),
			.button = c(0xf7faee),
			.accent = c(0x5f7000),
			.selectionBg = c(0x8a931d), .selectionFg = c(0x181d10),
			.border = c(0xd3dcc2), .borderSubtle = c(0xe5ecd6),
			.buttonBorder = c(0xc8d3b2),
		},
	};

	static const Theme fireflyDark{
		.name = QStringLiteral("Firefly"),
		.dark = true,
		.palette = {
			.windowBg = c(0x1c2418), .surface = c(0x161d13), .surfaceAlt = c(0x192015),
			.text = c(0xe6ebe1), .textDim = c(0x93a189),
			.button = c(0x242e1e),
			.accent = c(0xc8e224),
			.selectionBg = c(0x505d15), .selectionFg = c(0xe6ebe1),
			.border = c(0x33402c), .borderSubtle = c(0x232c1d),
			.accentFg = c(0x1c2000),
			.buttonBorder = c(0x44543a),
		},
	};

	// Sunset orange on plum. Dark only.
	static const Theme afterglow{
		.name = QStringLiteral("Afterglow"),
		.dark = true,
		.palette = {
			.windowBg = c(0x261b28), .surface = c(0x1e1420), .surfaceAlt = c(0x221824),
			.text = c(0xece4ee), .textDim = c(0xa794ab),
			.button = c(0x332338),
			.accent = c(0xf2683f),
			.selectionBg = c(0x7b463d), .selectionFg = c(0xece4ee),
			.border = c(0x443048), .borderSubtle = c(0x2e2031),
			.accentFg = c(0xffffff),
			.buttonBorder = c(0x4a3650),
		},
	};

	static const std::vector<Theme> themes{ darkroomDark, darkroomLight, classicLight, classicDark, honey,
		blackoutViolet, taxicabLight, taxicabDark, forge, verdigrisLight, verdigrisDark, fireflyLight, fireflyDark,
		afterglow };
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
	// Runs before resolvedPalette so the library's tint never applies; an authored accentBg still wins over both.
	if (!s_active.palette.accentBg.isValid())
		s_active.palette.accentBg = accentTint(s_active.palette);
	s_active.palette = resolvedPalette(s_active.palette);
	resolveAppColors(s_active);
}
