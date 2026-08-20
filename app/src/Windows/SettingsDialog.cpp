#include "Windows/SettingsDialog.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "theme/cthemecontroller.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

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

	QSettings s;
	_ffmpegPath = new QLineEdit(s.value(Settings::FfmpegPath).toString(), this);

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

	CThemeController& themes = CThemeController::instance();
	_originalScheme = themes.schemePreference();
	_originalLightTheme = themes.themeName(false);
	_originalDarkTheme = themes.themeName(true);

	_schemeToggle = new SegmentedToggle({ tr("System"), tr("Light"), tr("Dark") }, this);
	_schemeToggle->setCurrentIndex(toggleIndex(_originalScheme));
	connect(_schemeToggle, &SegmentedToggle::currentChanged, this, [](int index) {
		CThemeController::instance().setSchemePreference(schemeForToggleIndex(index));
	});
	form->addRow(tr("Color scheme:"), _schemeToggle);

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
		combo->setCurrentText(themes.themeName(dark));
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
	QSettings s;
	s.setValue(Settings::FfmpegPath, _ffmpegPath->text().trimmed());
}

void GeneralSettingsPage::rejectSettings()
{
	CThemeController& controller = CThemeController::instance();
	controller.setSchemePreference(_originalScheme);
	controller.setThemeName(false, _originalLightTheme);
	controller.setThemeName(true, _originalDarkTheme);
}

EncodingSettingsPage::EncodingSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Encoding"));

	QSettings s;
	const bool useTiff  = s.value(Settings::UseTiff,     Defaults::UseTiff).toBool();
	const int  quality  = s.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();
	const int  step     = s.value(Settings::FrameStep,   Defaults::FrameStep).toInt();

	_formatToggle = new SegmentedToggle({ tr("JPEG"), tr("TIFF") }, this);
	_formatToggle->setCurrentIndex(useTiff ? 1 : 0);

	_quality = new QSpinBox(this);
	_quality->setRange(1, 31);
	_quality->setValue(quality);
	_quality->setEnabled(!useTiff);

	auto* qualityHint = new QLabel(tr("1 = best quality / largest file, 31 = worst / smallest"), this);
	Style::applyThemedSheet(qualityHint, [] {
		return QStringLiteral("color: %1;").arg(Theme::current().instructionText.name());
	});
	qualityHint->setEnabled(!useTiff);

	connect(_formatToggle, &SegmentedToggle::currentChanged, this, [this, qualityHint](int index) {
		const bool jpeg = index == 0;
		_quality->setEnabled(jpeg);
		qualityHint->setEnabled(jpeg);
	});

	auto* qualityRow = new QHBoxLayout;
	qualityRow->addWidget(_quality);
	qualityRow->addWidget(qualityHint);
	qualityRow->addStretch();

	_frameStep = new QSpinBox(this);
	_frameStep->setRange(1, 100);
	_frameStep->setValue(step);
	_frameStep->setSpecialValueText(tr("1 (every frame)"));

	auto* form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->addRow(tr("Output format:"), _formatToggle);
	form->addRow(tr("JPEG quality:"), qualityRow);
	form->addRow(tr("Extract every N-th frame:"), _frameStep);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void EncodingSettingsPage::acceptSettings()
{
	QSettings s;
	s.setValue(Settings::UseTiff,     _formatToggle->currentIndex() == 1);
	s.setValue(Settings::JpegQuality, _quality->value());
	s.setValue(Settings::FrameStep,   _frameStep->value());
}

SettingsDialog::SettingsDialog(QWidget* parent) : CSettingsDialog(parent)
{
	addSettingsPage(new GeneralSettingsPage(this), tr("General"));
	addSettingsPage(new EncodingSettingsPage(this), tr("Encoding"));
}
