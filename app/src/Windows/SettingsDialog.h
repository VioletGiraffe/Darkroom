#pragma once

#include "settingsui/csettingsdialog.h"
#include "settingsui/csettingspage.h"

class QLineEdit;
class QSpinBox;
class SegmentedToggle;

class GeneralSettingsPage final : public CSettingsPage
{
public:
	explicit GeneralSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QLineEdit*       _ffmpegPath    = nullptr;
	SegmentedToggle* _schemeToggle  = nullptr;
	// Stored, not effective, scheme so cancellation can restore "System".
	int              _originalScheme = 0;
};

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
