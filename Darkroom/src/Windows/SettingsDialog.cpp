#include "Windows/SettingsDialog.h"
#include "DarkroomSettings.h"
#include "SharedSettings.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "UiComponents/GeneralSettingsPage.h"
#include "UiComponents/ImagesSettingsPage.h"
#include "UiComponents/SegmentedToggle.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

EncodingSettingsPage::EncodingSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Encoding"));

	const QSettings settings = sharedSettings();
	const bool useTiff  = settings.value(Settings::UseTiff,     Defaults::UseTiff).toBool();
	const int  quality  = settings.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();
	const int  step     = QSettings{}.value(Settings::FrameStep,   Defaults::FrameStep).toInt();

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
	QSettings shared = sharedSettings();
	shared.setValue(Settings::UseTiff,     _formatToggle->currentIndex() == 1);
	shared.setValue(Settings::JpegQuality, _quality->value());
	QSettings{}.setValue(Settings::FrameStep, _frameStep->value());
}

SettingsDialog::SettingsDialog(QWidget* parent) : CSettingsDialog(parent)
{
	addSettingsPage(new GeneralSettingsPage(this), tr("General"));
	addSettingsPage(new ImagesSettingsPage(this), tr("Images"));
	addSettingsPage(new EncodingSettingsPage(this), tr("Encoding"));
}
