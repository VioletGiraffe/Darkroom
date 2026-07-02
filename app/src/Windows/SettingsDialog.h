#pragma once

#include "settingsui/csettingsdialog.h"
#include "settingsui/csettingspage.h"

class QLineEdit;
class QRadioButton;
class QSpinBox;

// ── General page: root folder + ffmpeg path ───────────────────────────────────

class GeneralSettingsPage final : public CSettingsPage
{
public:
	explicit GeneralSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QLineEdit*    m_rootFolder    = nullptr;
	QLineEdit*    m_ffmpegPath    = nullptr;
	QRadioButton* m_schemeSystem  = nullptr;
	QRadioButton* m_schemeLight   = nullptr;
	QRadioButton* m_schemeDark    = nullptr;
};

// ── Encoding page: output format + JPEG quality ───────────────────────────────

class EncodingSettingsPage final : public CSettingsPage
{
public:
	explicit EncodingSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	QRadioButton* m_jpeg      = nullptr;
	QRadioButton* m_tiff      = nullptr;
	QSpinBox*     m_quality   = nullptr;
	QSpinBox*     m_frameStep = nullptr;
};

// ── Top-level dialog ─────────────────────────────────────────────────────────

class SettingsDialog final : public CSettingsDialog
{
public:
	explicit SettingsDialog(QWidget* parent = nullptr);
};
