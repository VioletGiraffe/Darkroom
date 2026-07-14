#pragma once

#include "settingsui/csettingsdialog.h"
#include "settingsui/csettingspage.h"

#include <QString>

class QLineEdit;
class QSpinBox;
class SegmentedToggle;

// ── General page: root folder + ffmpeg path ───────────────────────────────────

class GeneralSettingsPage final : public CSettingsPage
{
public:
	GeneralSettingsPage(const QString& rootFolder, QWidget* parent = nullptr);
	void acceptSettings() override;
	[[nodiscard]] QString requestedRootFolder() const;

private:
	QLineEdit*       m_rootFolder    = nullptr;
	QLineEdit*       m_ffmpegPath    = nullptr;
	SegmentedToggle* m_schemeToggle  = nullptr;
	// The scheme applied when the dialog opened; restored live if the dialog is cancelled. Holds the stored
	// Qt::ColorScheme value (not the effective one), so a "System" choice reverts back to System, not its
	// resolved light/dark.
	int              m_originalScheme = 0;
};

// ── Encoding page: output format + JPEG quality ───────────────────────────────

class EncodingSettingsPage final : public CSettingsPage
{
public:
	explicit EncodingSettingsPage(QWidget* parent = nullptr);
	void acceptSettings() override;

private:
	SegmentedToggle* m_formatToggle = nullptr;
	QSpinBox*        m_quality      = nullptr;
	QSpinBox*        m_frameStep    = nullptr;
};

// ── Top-level dialog ─────────────────────────────────────────────────────────

class SettingsDialog final : public CSettingsDialog
{
public:
	SettingsDialog(const QString& rootFolder, QWidget* parent = nullptr);
	[[nodiscard]] QString requestedRootFolder() const;

private:
	GeneralSettingsPage* m_generalPage = nullptr;
};
