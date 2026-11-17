#pragma once

#include "appdialogs/csettingsdialog.h"
#include "appdialogs/csettingspage.h"

class QSpinBox;
class SegmentedToggle;

class FrameExtractionSettingsPage final : public CSettingsPage
{
public:
	explicit FrameExtractionSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	SegmentedToggle* _formatToggle = nullptr;
	QSpinBox*        _quality      = nullptr;
};

class SettingsDialog final : public CSettingsDialog
{
public:
	explicit SettingsDialog(QWidget* parent = nullptr);
};
