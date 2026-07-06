#include "Windows/SettingsDialog.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QStyleHints>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

// ── GeneralSettingsPage ───────────────────────────────────────────────────────

GeneralSettingsPage::GeneralSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("General"));

	QSettings s;
	m_rootFolder = new QLineEdit(s.value(Settings::RootFolder, Defaults::RootFolder).toString(), this);
	m_ffmpegPath = new QLineEdit(s.value(Settings::FfmpegPath).toString(), this);

	auto* browseFolder = new QPushButton(tr("Browse..."), this);
	auto* browseFfmpeg = new QPushButton(tr("Browse..."), this);

	connect(browseFolder, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getExistingDirectory(this, tr("Select root folder"), m_rootFolder->text());
		if (!path.isEmpty())
			m_rootFolder->setText(QDir::toNativeSeparators(path));
	});

	connect(browseFfmpeg, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getOpenFileName(this, tr("Select ffmpeg executable"),
			m_ffmpegPath->text(), tr("Executables (*.exe);;All files (*)"));
		if (!path.isEmpty())
			m_ffmpegPath->setText(QDir::toNativeSeparators(path));
	});

	auto* folderRow = new QHBoxLayout;
	folderRow->addWidget(m_rootFolder, 1);
	folderRow->addWidget(browseFolder);

	auto* ffmpegRow = new QHBoxLayout;
	ffmpegRow->addWidget(m_ffmpegPath, 1);
	ffmpegRow->addWidget(browseFfmpeg);

	auto* form = new QFormLayout;
	form->addRow(tr("Root folder:"), folderRow);
	form->addRow(tr("ffmpeg path:"), ffmpegRow);

	const int savedScheme = s.value(Settings::ColorScheme, Defaults::ColorScheme).toInt();
	m_schemeSystem = new QRadioButton(tr("System default"), this);
	m_schemeLight  = new QRadioButton(tr("Light"),          this);
	m_schemeDark   = new QRadioButton(tr("Dark"),           this);

	if (savedScheme == static_cast<int>(Qt::ColorScheme::Dark))
		m_schemeDark->setChecked(true);
	else if (savedScheme == static_cast<int>(Qt::ColorScheme::Light))
		m_schemeLight->setChecked(true);
	else
		m_schemeSystem->setChecked(true);

	auto* schemeGroup = new QGroupBox(tr("Color scheme"), this);
	auto* schemeLayout = new QHBoxLayout(schemeGroup);
	schemeLayout->addWidget(m_schemeSystem);
	schemeLayout->addWidget(m_schemeLight);
	schemeLayout->addWidget(m_schemeDark);
	schemeLayout->addStretch();

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(schemeGroup);
	layout->addStretch();
}

void GeneralSettingsPage::acceptSettings()
{
	const QString root = m_rootFolder->text().trimmed();
	if (root.isEmpty())
	{
		QMessageBox::warning(this, tr("Settings"), tr("Root folder cannot be empty."));
		return;
	}
	const int scheme = m_schemeDark->isChecked()  ? static_cast<int>(Qt::ColorScheme::Dark)
	                 : m_schemeLight->isChecked() ? static_cast<int>(Qt::ColorScheme::Light)
	                 :                              static_cast<int>(Qt::ColorScheme::Unknown);

	QSettings s;
	s.setValue(Settings::RootFolder,  root);
	s.setValue(Settings::FfmpegPath,  m_ffmpegPath->text().trimmed());
	s.setValue(Settings::ColorScheme, scheme);
	QGuiApplication::styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(scheme));
}

// ── EncodingSettingsPage ──────────────────────────────────────────────────────

EncodingSettingsPage::EncodingSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Encoding"));

	QSettings s;
	const bool useTiff  = s.value(Settings::UseTiff,     Defaults::UseTiff).toBool();
	const int  quality  = s.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();
	const int  step     = s.value(Settings::FrameStep,   Defaults::FrameStep).toInt();

	// Output format
	m_jpeg = new QRadioButton(tr("JPEG"), this);
	m_tiff = new QRadioButton(tr("TIFF"), this);
	m_jpeg->setChecked(!useTiff);
	m_tiff->setChecked(useTiff);

	auto* formatBox    = new QGroupBox(tr("Output format"), this);
	auto* formatLayout = new QHBoxLayout(formatBox);
	formatLayout->addWidget(m_jpeg);
	formatLayout->addWidget(m_tiff);
	formatLayout->addStretch();

	// JPEG quality (disabled when TIFF is selected)
	m_quality = new QSpinBox(this);
	m_quality->setRange(1, 31);
	m_quality->setValue(quality);
	m_quality->setEnabled(!useTiff);

	auto* qualityHint = new QLabel(tr("1 = best quality / largest file, 31 = worst / smallest"), this);
	Style::applyThemedSheet(qualityHint, [] {
		return QStringLiteral("color: %1;").arg(Theme::current().InstructionText);
	});
	qualityHint->setEnabled(!useTiff);

	connect(m_jpeg, &QRadioButton::toggled, m_quality,   &QSpinBox::setEnabled);
	connect(m_jpeg, &QRadioButton::toggled, qualityHint, &QLabel::setEnabled);

	auto* qualityRow = new QHBoxLayout;
	qualityRow->addWidget(m_quality);
	qualityRow->addWidget(qualityHint);
	qualityRow->addStretch();

	m_frameStep = new QSpinBox(this);
	m_frameStep->setRange(1, 100);
	m_frameStep->setValue(step);
	m_frameStep->setSpecialValueText(tr("1 (every frame)"));

	auto* form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->addRow(tr("JPEG quality:"), qualityRow);
	form->addRow(tr("Extract every N-th frame:"), m_frameStep);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(formatBox);
	layout->addLayout(form);
	layout->addStretch();
}

void EncodingSettingsPage::acceptSettings()
{
	QSettings s;
	s.setValue(Settings::UseTiff,     m_tiff->isChecked());
	s.setValue(Settings::JpegQuality, m_quality->value());
	s.setValue(Settings::FrameStep,   m_frameStep->value());
}

// ── SettingsDialog ────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(QWidget* parent) : CSettingsDialog(parent)
{
	addSettingsPage(new GeneralSettingsPage(this),  tr("General"));
	addSettingsPage(new EncodingSettingsPage(this), tr("Encoding"));
}
