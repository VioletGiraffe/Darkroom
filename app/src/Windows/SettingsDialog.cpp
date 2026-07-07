#include "Windows/SettingsDialog.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"

#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QStyleHints>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

	// Segment order matches Qt::ColorScheme's own values (Unknown/System = 0, Light = 1, Dark = 2), so the
	// segment index is directly the scheme value - no lookup table needed here or in acceptSettings().
	m_originalScheme = s.value(Settings::ColorScheme, Defaults::ColorScheme).toInt();
	m_schemeToggle = new SegmentedToggle({ tr("System"), tr("Light"), tr("Dark") }, this);
	m_schemeToggle->setCurrentIndex(m_originalScheme);   // silent

	// Apply the scheme live as the user toggles, for immediate preview (Style::install() re-themes the app on
	// QStyleHints::colorSchemeChanged). acceptSettings() only persists it; a cancel reverts it below.
	connect(m_schemeToggle, &SegmentedToggle::currentChanged, this, [](int index) {
		QGuiApplication::styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(index));
	});

	// Undo the live preview when the dialog is dismissed without accepting (Cancel / Esc / close button all
	// route through QDialog::reject()). On accept, only accepted() fires, so this leaves the choice in place.
	if (auto* dialog = qobject_cast<QDialog*>(parent))
		connect(dialog, &QDialog::rejected, this, [this] {
			QGuiApplication::styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(m_originalScheme));
		});

	form->addRow(tr("Color scheme:"), m_schemeToggle);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void GeneralSettingsPage::acceptSettings()
{
	QSettings s;
	// The scheme is already applied live; persist it here. Done before the root-folder guard so an empty-root
	// warning doesn't discard the user's confirmed scheme choice.
	s.setValue(Settings::ColorScheme, m_schemeToggle->currentIndex());

	const QString root = m_rootFolder->text().trimmed();
	if (root.isEmpty())
	{
		QMessageBox::warning(this, tr("Settings"), tr("Root folder cannot be empty."));
		return;
	}

	s.setValue(Settings::RootFolder, root);
	s.setValue(Settings::FfmpegPath, m_ffmpegPath->text().trimmed());
}

// ── EncodingSettingsPage ──────────────────────────────────────────────────────

EncodingSettingsPage::EncodingSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Encoding"));

	QSettings s;
	const bool useTiff  = s.value(Settings::UseTiff,     Defaults::UseTiff).toBool();
	const int  quality  = s.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();
	const int  step     = s.value(Settings::FrameStep,   Defaults::FrameStep).toInt();

	// Output format - segment 0 = JPEG, 1 = TIFF
	m_formatToggle = new SegmentedToggle({ tr("JPEG"), tr("TIFF") }, this);
	m_formatToggle->setCurrentIndex(useTiff ? 1 : 0);   // silent

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

	// JPEG quality only applies to JPEG output; grey it out (with its hint) while TIFF is selected.
	connect(m_formatToggle, &SegmentedToggle::currentChanged, this, [this, qualityHint](int index) {
		const bool jpeg = index == 0;
		m_quality->setEnabled(jpeg);
		qualityHint->setEnabled(jpeg);
	});

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
	form->addRow(tr("Output format:"), m_formatToggle);
	form->addRow(tr("JPEG quality:"), qualityRow);
	form->addRow(tr("Extract every N-th frame:"), m_frameStep);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void EncodingSettingsPage::acceptSettings()
{
	QSettings s;
	s.setValue(Settings::UseTiff,     m_formatToggle->currentIndex() == 1);
	s.setValue(Settings::JpegQuality, m_quality->value());
	s.setValue(Settings::FrameStep,   m_frameStep->value());
}

// ── SettingsDialog ────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(QWidget* parent) : CSettingsDialog(parent)
{
	addSettingsPage(new GeneralSettingsPage(this),  tr("General"));
	addSettingsPage(new EncodingSettingsPage(this), tr("Encoding"));
}
