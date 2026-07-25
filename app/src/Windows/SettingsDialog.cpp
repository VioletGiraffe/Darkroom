#include "Windows/SettingsDialog.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"

#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QStyleHints>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

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

	_originalScheme = s.value(Settings::ColorScheme, Defaults::ColorScheme).toInt();
	_schemeToggle = new SegmentedToggle({ tr("System"), tr("Light"), tr("Dark") }, this);
	_schemeToggle->setCurrentIndex(_originalScheme);

	connect(_schemeToggle, &SegmentedToggle::currentChanged, this, [](int index) {
		QGuiApplication::styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(index));
	});

	// Reject must undo the live theme preview.
	if (auto* dialog = qobject_cast<QDialog*>(parent))
		connect(dialog, &QDialog::rejected, this, [this] {
			QGuiApplication::styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(_originalScheme));
		});

	form->addRow(tr("Color scheme:"), _schemeToggle);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void GeneralSettingsPage::acceptSettings()
{
	QSettings s;
	s.setValue(Settings::ColorScheme, _schemeToggle->currentIndex());
	s.setValue(Settings::FfmpegPath, _ffmpegPath->text().trimmed());
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
		return QStringLiteral("color: %1;").arg(Theme::current().InstructionText);
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
