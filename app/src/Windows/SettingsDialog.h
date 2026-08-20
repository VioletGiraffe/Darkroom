#pragma once

#include "settingsui/csettingsdialog.h"
#include "settingsui/csettingspage.h"

#include <QString>
#include <Qt>

class QLineEdit;
class QSpinBox;
class SegmentedToggle;

class GeneralSettingsPage final : public CSettingsPage
{
public:
	explicit GeneralSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;
	void rejectSettings() override;

private:
	QLineEdit*       _ffmpegPath   = nullptr;
	SegmentedToggle* _schemeToggle = nullptr;
	// The stored preferences, not the effective ones, so cancellation can restore "System" and
	// names that were falling back.
	Qt::ColorScheme  _originalScheme = Qt::ColorScheme::Unknown;
	QString          _originalLightTheme;
	QString          _originalDarkTheme;
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
