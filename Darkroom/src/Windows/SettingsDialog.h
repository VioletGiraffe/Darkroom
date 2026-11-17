#pragma once

#include "appdialogs/csettingsdialog.h"
#include "appdialogs/csettingspage.h"

class QSpinBox;
class SegmentedToggle;

class EncodingSettingsPage final : public CSettingsPage
{
public:
	explicit EncodingSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	SegmentedToggle* _formatToggle = nullptr;
	QSpinBox*        _quality      = nullptr;
	QSpinBox*        _frameStep    = nullptr;
};

class SettingsDialog final : public CSettingsDialog
{
public:
	explicit SettingsDialog(QWidget* parent = nullptr);
};
