#pragma once

#include "appdialogs/csettingspage.h"
#include "theme/cthemecontroller.h"

class QLineEdit;

// The ffmpeg path and the theme choices, a page of both apps' settings dialogs
class GeneralSettingsPage final : public CSettingsPage
{
public:
	explicit GeneralSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;
	void rejectSettings() override;

private:
	QLineEdit* _ffmpegPath = nullptr;
	const CThemeController::Selection _initialThemeSelection = CThemeController::instance().selection();
};
