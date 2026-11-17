#include "UiComponents/GeneralSettingsPage.h"
#include "SharedSettings.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

namespace {

// The toggle's segments are System, Light, Dark - mapped explicitly, not by the enum's numbering.
int toggleIndex(Qt::ColorScheme scheme)
{
	switch (scheme)
	{
	case Qt::ColorScheme::Light: return 1;
	case Qt::ColorScheme::Dark: return 2;
	case Qt::ColorScheme::Unknown: break;
	}
	return 0;
}

Qt::ColorScheme schemeForToggleIndex(int index)
{
	switch (index)
	{
	case 1: return Qt::ColorScheme::Light;
	case 2: return Qt::ColorScheme::Dark;
	default: return Qt::ColorScheme::Unknown;
	}
}

} // namespace

GeneralSettingsPage::GeneralSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("General"));

	_ffmpegPath = new QLineEdit(sharedSettings().value(Settings::FfmpegPath).toString(), this);

	// Show the empty-field fallback, not the configured path.
	const QString detectedFfmpeg = autoDetectedFfmpegPath();
	_ffmpegPath->setPlaceholderText(detectedFfmpeg.isEmpty()
		? tr("Not found - set the path to the ffmpeg binary")
		: tr("Auto-detected: %1").arg(QDir::toNativeSeparators(detectedFfmpeg)));

	auto* browseFfmpeg = new QPushButton(tr("Browse..."), this);

	connect(browseFfmpeg, &QPushButton::clicked, this, [this] {
#ifdef Q_OS_WIN
		const QString filter = tr("Executables (*.exe);;All files (*)");
#else
		const QString filter = tr("All files (*)");
#endif
		const QString path = QFileDialog::getOpenFileName(this, tr("Select ffmpeg executable"), _ffmpegPath->text(), filter);
		if (!path.isEmpty())
			_ffmpegPath->setText(QDir::toNativeSeparators(path));
	});

	auto* ffmpegRow = new QHBoxLayout;
	ffmpegRow->addWidget(_ffmpegPath, 1);
	ffmpegRow->addWidget(browseFfmpeg);

	auto* form = new QFormLayout;
	form->addRow(tr("ffmpeg path:"), ffmpegRow);

	auto* schemeToggle = new SegmentedToggle({ tr("System"), tr("Light"), tr("Dark") }, this);
	schemeToggle->setCurrentIndex(toggleIndex(_initialThemeSelection.schemePreference));
	connect(schemeToggle, &SegmentedToggle::currentChanged, this, [](int index) {
		CThemeController::instance().setSchemePreference(schemeForToggleIndex(index));
	});
	form->addRow(tr("Color scheme:"), schemeToggle);

	// One selector per polarity; each shows only its themes and applies live, like the scheme toggle
	const auto addThemeSelector = [&](bool dark, const QString& label) {
		auto* combo = new QComboBox(this);
		for (const Theme::Theme& theme : Theme::allThemes())
		{
			if (theme.dark == dark)
				combo->addItem(theme.name);
		}
		// A stored name the combo does not hold leaves index 0 - the same first-of-polarity
		// fallback the theme resolution applies
		combo->setCurrentText(dark ? _initialThemeSelection.darkThemeName : _initialThemeSelection.lightThemeName);
		connect(combo, &QComboBox::currentTextChanged, this, [dark](const QString& name) {
			CThemeController::instance().setThemeName(dark, name);
		});
		form->addRow(label, combo);
	};
	addThemeSelector(false, tr("Light theme:"));
	addThemeSelector(true, tr("Dark theme:"));

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void GeneralSettingsPage::acceptSettings()
{
	sharedSettings().setValue(Settings::FfmpegPath, _ffmpegPath->text().trimmed());
}

void GeneralSettingsPage::rejectSettings()
{
	CThemeController::instance().setSelection(_initialThemeSelection);
}
