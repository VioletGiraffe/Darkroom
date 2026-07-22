#pragma once

#include "settingsui/csettingsdialog.h"
#include "settingsui/csettingspage.h"

class QLineEdit;
class QSpinBox;
class SegmentedToggle;

// ── General page: ffmpeg path + color scheme ──────────────────────────────────

class GeneralSettingsPage final : public CSettingsPage
{
public:
	explicit GeneralSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QLineEdit*       _ffmpegPath    = nullptr;
	SegmentedToggle* _schemeToggle  = nullptr;
	// The scheme applied when the dialog opened; restored live if the dialog is cancelled. Holds the stored
	// Qt::ColorScheme value (not the effective one), so a "System" choice reverts back to System, not its
	// resolved light/dark.
	int              _originalScheme = 0;
};

// ── Encoding page: output format + JPEG quality ───────────────────────────────

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

// ── Top-level dialog ─────────────────────────────────────────────────────────

class SettingsDialog final : public CSettingsDialog
{
public:
	explicit SettingsDialog(QWidget* parent = nullptr);
};
