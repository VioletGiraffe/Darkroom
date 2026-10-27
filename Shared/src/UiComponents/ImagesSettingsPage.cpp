#include "UiComponents/ImagesSettingsPage.h"
#include "ImageDecoding/ImageDecoding.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

using ImageDecoding::RawRendering;

ImagesSettingsPage::ImagesSettingsPage(QWidget* parent) : CSettingsPage(parent)
{
	setWindowTitle(tr("Images"));

	_rawRendering = new QComboBox(this);
	_rawRendering->addItem(tr("Embedded JPEG preview (the camera's rendering)"), static_cast<int>(RawRendering::EmbeddedPreview));
	_rawRendering->addItem(tr("Half-size rendering"), static_cast<int>(RawRendering::HalfSize));
	_rawRendering->addItem(tr("Full-size rendering (slowest)"), static_cast<int>(RawRendering::Full));
	_rawRendering->setCurrentIndex(_rawRendering->findData(static_cast<int>(rawRenderingSetting())));

	auto* rawRenderingHint = new QLabel(tr("How raw photos open in the viewer and in photo compare. Thumbnails always show the embedded preview, "
		"and a raw without one opens at half size."), this);
	rawRenderingHint->setWordWrap(true);
	Style::applyThemedSheet(rawRenderingHint, [] {
		return QStringLiteral("color: %1;").arg(Theme::current().instructionText.name());
	});

	auto* form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->addRow(tr("Camera raw photos:"), _rawRendering);
	form->addRow(QString{}, rawRenderingHint);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
}

void ImagesSettingsPage::acceptSettings()
{
	saveRawRenderingSetting(static_cast<RawRendering>(_rawRendering->currentData().toInt()));
}
