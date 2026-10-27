#pragma once

#include "appdialogs/csettingspage.h"

class QComboBox;

// Image settings of the shared store, a page of both apps' settings dialogs
class ImagesSettingsPage final : public CSettingsPage
{
public:
	explicit ImagesSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QComboBox* _rawRendering = nullptr;
};
