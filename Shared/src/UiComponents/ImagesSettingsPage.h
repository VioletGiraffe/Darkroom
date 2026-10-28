#pragma once

#include "appdialogs/csettingspage.h"

class QCheckBox;
class QComboBox;

// Image settings of the shared store, a page of both apps' settings dialogs
class ImagesSettingsPage final : public CSettingsPage
{
public:
	explicit ImagesSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QComboBox* _rawRendering = nullptr;
	QCheckBox* _colorManagement = nullptr;
};
