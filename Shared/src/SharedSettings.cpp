#include "SharedSettings.h"
#include "theme/cthemecontroller.h"

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCoreApplication>
RESTORE_COMPILER_WARNINGS

namespace {

constexpr const char* SharedSettingsApplicationName = "DarkroomShared";

void moveSharedValuesOutOfAppStores()
{
	constexpr const char* sharedKeys[] {
		Settings::FfmpegPath,
		Settings::UseTiff,
		Settings::JpegQuality,
		Settings::VideoPlayerPauseOnSeek,
		Settings::VideoPlayerVolume,
		Settings::VideoPlayerMuted,
		Settings::VideoPlayerOscillationCurve,
		Settings::PhotoCompareIgnoreRotation,
		// CThemeController's
		"Theme/ColorScheme",
		"Theme/LightTheme",
		"Theme/DarkTheme",
	};

	QSettings shared = sharedSettings();
	// Darkroom first: its value wins where both apps stored one.
	for (const char* application : { DarkroomApplicationName, QuickroomApplicationName })
	{
		QSettings appStore{ QCoreApplication::organizationName(), application };
		for (const char* key : sharedKeys)
		{
			if (!appStore.contains(key))
				continue;

			if (!shared.contains(key))
				shared.setValue(key, appStore.value(key));
			appStore.remove(key);
		}
	}
}

} // namespace

QSettings sharedSettings()
{
	return QSettings{ QCoreApplication::organizationName(), SharedSettingsApplicationName };
}

void initSharedSettings()
{
	moveSharedValuesOutOfAppStores();
	CThemeController::setSettingsLocation(QCoreApplication::organizationName(), SharedSettingsApplicationName);
}
