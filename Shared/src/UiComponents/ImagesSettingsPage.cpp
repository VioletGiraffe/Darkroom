#include "UiComponents/ImagesSettingsPage.h"
#include "ImageDecoding/ImageDecoding.h"
#include "SharedSettings.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

using ImageDecoding::RawRendering;

ImagesSettingsPage::ImagesSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Images"));

	_rawRendering = new QComboBox(this);
	_rawRendering->addItem(tr("Embedded JPEG preview (rendered by the camera)"), static_cast<int>(RawRendering::EmbeddedPreview));
	_rawRendering->addItem(tr("Half-resolution rendering (no demosaicing)"), static_cast<int>(RawRendering::HalfSize));
	_rawRendering->addItem(tr("Full-resolution rendering (slowest)"), static_cast<int>(RawRendering::Full));
	_rawRendering->setCurrentIndex(_rawRendering->findData(static_cast<int>(rawRenderingSetting())));

	_colorManagement = new QCheckBox(tr("Apply embedded color profiles (convert to sRGB for display)"), this);
	_colorManagement->setChecked(colorManagementSetting());

	auto* form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->addRow(tr("Raw photos:"), _rawRendering);
	form->addRow(tr("Color management:"), _colorManagement);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void ImagesSettingsPage::acceptSettings()
{
	saveRawRenderingSetting(static_cast<RawRendering>(_rawRendering->currentData().toInt()));
	sharedSettings().setValue(Settings::ColorManagement, _colorManagement->isChecked());
}
