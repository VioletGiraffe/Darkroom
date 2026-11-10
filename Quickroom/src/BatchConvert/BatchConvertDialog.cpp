#include "BatchConvert/BatchConvertDialog.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "fileoperations/fileoperationtexts.h"
#include "fileoperations/newnamecheck.h"
#include "qtcore_helpers/displayformatting.hpp"
#include "widgets/cpersistenceenabler.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractButton>
#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

using namespace BatchConvert;

namespace {

const QString ResizeKey = QStringLiteral("batchConvert/resize");
const QString FitKey = QStringLiteral("batchConvert/fit");
const QString WidthKey = QStringLiteral("batchConvert/width");
const QString HeightKey = QStringLiteral("batchConvert/height");
const QString DirectionKey = QStringLiteral("batchConvert/direction");
const QString AdaptToOrientationKey = QStringLiteral("batchConvert/adaptToOrientation");
const QString FormatKey = QStringLiteral("batchConvert/format");
const QString QualityKey = QStringLiteral("batchConvert/quality");
const QString SubsamplingKey = QStringLiteral("batchConvert/jpegSubsampling");
const QString LocationKey = QStringLiteral("batchConvert/location");
const QString SubfolderKey = QStringLiteral("batchConvert/subfolder");
const QString FolderKey = QStringLiteral("batchConvert/folder");

// In the order of the controls' segments and items, which the settings store as indices
constexpr std::array FitModes{ FitMode::Inside, FitMode::Outside };
constexpr std::array ScaleDirections{ ScaleDirection::Any, ScaleDirection::OnlyEnlarge, ScaleDirection::OnlyShrink };
constexpr std::array Subsamplings{ ImageEncoding::JpegSubsampling::Chroma420, ImageEncoding::JpegSubsampling::Chroma444 };
constexpr std::array Locations{ OutputLocation::Subfolder, OutputLocation::Folder, OutputLocation::SourceFolder };

constexpr int MaxSide = 65535; // JPEG's limit

// A stored index, clamped to count entries
int storedIndex(const QSettings& settings, const QString& key, size_t count)
{
	return std::clamp(settings.value(key, 0).toInt(), 0, static_cast<int>(count) - 1);
}

template <typename Value, size_t Count>
int indexOf(const std::array<Value, Count>& values, Value value)
{
	return static_cast<int>(std::ranges::find(values, value) - values.begin());
}

// An invalid color restores the list's own text color
void styleImageItem(QListWidgetItem& item, const QColor& color, bool bold = false, bool italic = false)
{
	QFont font = item.font();
	font.setBold(bold);
	font.setItalic(italic);
	item.setFont(font);
	item.setForeground(color.isValid() ? QBrush{ color } : QBrush{});
}

} // namespace

void BatchConvertDialog::showForImages(const QStringList& imagePaths)
{
	auto* dialog = new BatchConvertDialog(imagePaths);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

BatchConvertDialog::BatchConvertDialog(const QStringList& imagePaths)
{
	setAcceptDrops(true);

	auto* imagesGroup = new QGroupBox(tr("Images"));
	_imageList = new QListWidget;
	_imageList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	_imageList->setContextMenuPolicy(Qt::ActionsContextMenu);
	// Restyling a row lays out the whole list again: with uniform sizes that layout measures one row, not all of them
	_imageList->setUniformItemSizes(true);
	_imageList->setTextElideMode(Qt::ElideMiddle); // a path wider than the rows keeps its drive and file name
	_removeAction = new QAction(tr("Remove from the batch"), _imageList);
	_removeAction->setShortcut(QKeySequence::Delete);
	_removeAction->setShortcutContext(Qt::WidgetShortcut);
	_imageList->addAction(_removeAction);
	_dropHint = new QLabel(tr("Drop images or folders here to add them"));
	auto* imagesLayout = new QVBoxLayout(imagesGroup);
	imagesLayout->addWidget(_imageList);
	imagesLayout->addWidget(_dropHint);

	_resizeGroup = new QGroupBox(tr("Resize"));
	_resizeGroup->setCheckable(true);
	_widthBox = new QSpinBox;
	_heightBox = new QSpinBox;
	for (QSpinBox* side : { _widthBox, _heightBox })
	{
		side->setRange(1, MaxSide);
		side->setSuffix(tr(" px"));
	}
	auto* sizeRow = new QHBoxLayout;
	sizeRow->addWidget(_widthBox);
	sizeRow->addWidget(new QLabel(QStringLiteral("×")));
	sizeRow->addWidget(_heightBox);
	sizeRow->addStretch();
	_fitToggle = new SegmentedToggle({ tr("Inside"), tr("Outside") });
	_directionToggle = new SegmentedToggle({ tr("Both directions"), tr("Only enlarge"), tr("Only shrink") });
	_adaptToOrientationBox = new QCheckBox(tr("Adapt to orientation"));
	_adaptToOrientationBox->setToolTip(tr("Swaps width and height for an image whose orientation differs from the box's"));
	auto* resizeForm = new QFormLayout(_resizeGroup);
	resizeForm->addRow(tr("Box:"), sizeRow);
	resizeForm->addRow(tr("Fit:"), _fitToggle);
	resizeForm->addRow(tr("Scale:"), _directionToggle);
	resizeForm->addRow(QString{}, _adaptToOrientationBox);

	auto* formatGroup = new QGroupBox(tr("Format"));
	_formatBox = new QComboBox;
	_formatBox->addItem(tr("Same as the original"), QString{});
	_formatBox->setToolTip(tr("Camera raws, and originals in formats that cannot be written, are saved as JPEG"));
	const QMimeDatabase mimeDatabase;
	for (const QString& mimeType : ImageEncoding::offeredMimeTypes())
	{
		const QMimeType type = mimeDatabase.mimeTypeForName(mimeType);
		_formatBox->addItem(QStringLiteral("%1 (.%2)").arg(type.comment(), type.preferredSuffix()), mimeType);
	}
	_qualityBox = new QSpinBox;
	_qualityBox->setRange(1, 100);
	_subsamplingBox = new QComboBox;
	_subsamplingBox->addItems({ QStringLiteral("4:2:0"), QStringLiteral("4:4:4") });
	auto* formatForm = new QFormLayout(formatGroup);
	formatForm->addRow(tr("Format:"), _formatBox);
	formatForm->addRow(tr("Quality:"), _qualityBox);
	formatForm->addRow(tr("JPEG chroma subsampling:"), _subsamplingBox);

	auto* outputGroup = new QGroupBox(tr("Save to"));
	_subfolderButton = new QRadioButton(tr("A subfolder beside each original:"));
	_subfolderEdit = new QLineEdit;
	_folderButton = new QRadioButton(tr("One folder:"));
	_folderEdit = new QLineEdit;
	_browseButton = new QPushButton(tr("Browse..."));
	_sourceFolderButton = new QRadioButton(tr("The originals' own folders"));
	auto* outputGrid = new QGridLayout(outputGroup);
	outputGrid->addWidget(_subfolderButton, 0, 0);
	outputGrid->addWidget(_subfolderEdit, 0, 1, 1, 2);
	outputGrid->addWidget(_folderButton, 1, 0);
	outputGrid->addWidget(_folderEdit, 1, 1);
	outputGrid->addWidget(_browseButton, 1, 2);
	outputGrid->addWidget(_sourceFolderButton, 2, 0, 1, 3);
	outputGrid->setColumnStretch(1, 1);

	_optionsArea = new QWidget;
	auto* optionsLayout = new QVBoxLayout(_optionsArea);
	optionsLayout->setContentsMargins(0, 0, 0, 0);
	optionsLayout->addWidget(_resizeGroup);
	optionsLayout->addWidget(formatGroup);
	optionsLayout->addWidget(outputGroup);
	optionsLayout->addStretch();

	auto* setupRow = new QHBoxLayout;
	setupRow->addWidget(imagesGroup, 1);
	setupRow->addWidget(_optionsArea);

	_progressBar = new QProgressBar;
	_statusLabel = new QLabel;
	_failureList = new QPlainTextEdit;
	_failureList->setReadOnly(true);
	for (QWidget* progressWidget : std::initializer_list<QWidget*>{ _progressBar, _statusLabel, _failureList })
		progressWidget->hide();

	_startButton = new QPushButton(tr("Start"));
	_startButton->setDefault(true);
	_closeButton = new QPushButton(tr("Close"));
	auto* buttonRow = new QHBoxLayout;
	buttonRow->addStretch();
	buttonRow->addWidget(_startButton);
	buttonRow->addWidget(_closeButton);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(setupRow, 1);
	layout->addWidget(_progressBar);
	layout->addWidget(_statusLabel);
	layout->addWidget(_failureList, 1);
	layout->addLayout(buttonRow);

	loadOptions();
	addImages(imagePaths);

	connect(_removeAction, &QAction::triggered, this, &BatchConvertDialog::removeSelectedImages);
	connect(_formatBox, &QComboBox::currentIndexChanged, this, &BatchConvertDialog::updateControls);
	for (QRadioButton* location : { _subfolderButton, _folderButton, _sourceFolderButton })
		connect(location, &QRadioButton::toggled, this, &BatchConvertDialog::updateControls);
	for (QLineEdit* edit : { _subfolderEdit, _folderEdit })
		connect(edit, &QLineEdit::textChanged, this, &BatchConvertDialog::updateControls);
	connect(_browseButton, &QPushButton::clicked, this, [this] {
		if (const QString folder = QFileDialog::getExistingDirectory(this, tr("Save to folder"), _folderEdit->text()); !folder.isEmpty())
			_folderEdit->setText(QDir::toNativeSeparators(folder));
	});
	connect(_startButton, &QPushButton::clicked, this, &BatchConvertDialog::startConversion);
	connect(_closeButton, &QPushButton::clicked, this, &BatchConvertDialog::reject);

	enablePersistence(this, "batchConvert/window", CPersistenceEnabler::Delayed{ true }, CPersistenceEnabler::SetDefaultSize{ false });
}

void BatchConvertDialog::done(int result)
{
	saveOptions();
	QDialog::done(result);
}

void BatchConvertDialog::reject()
{
	if (!_job || !_job->isRunning())
	{
		QDialog::reject();
		return;
	}

	_closeOnFinish = true;
	_closeButton->setEnabled(false);
	_statusLabel->setText(tr("Cancelling"));
	_job->cancel();
}

void BatchConvertDialog::closeEvent(QCloseEvent* event)
{
	if (_job && _job->isRunning())
	{
		reject();
		event->ignore();
		return;
	}
	QDialog::closeEvent(event);
}

void BatchConvertDialog::dragEnterEvent(QDragEnterEvent* event)
{
	dragMoveEvent(event);
}

void BatchConvertDialog::dragMoveEvent(QDragMoveEvent* event)
{
	const QList<QUrl> urls = event->mimeData()->urls();
	if (!_started && std::ranges::any_of(urls, &QUrl::isLocalFile))
		acceptDropAsCopy(event);
	else
		event->ignore();
}

void BatchConvertDialog::dropEvent(QDropEvent* event)
{
	acceptDropAsCopy(event);
	QStringList paths = droppedFilesWithFolderImages(event->mimeData());
	paths.removeIf([](const QString& path) { return !isSupportedImageFile(path); });
	addImages(paths);
}

void BatchConvertDialog::addImages(const QStringList& paths)
{
	QSet<QString> listedKeys;
	for (const QString& listed : imagePaths())
		listedKeys.insert(pathComparisonKey(listed));
	for (const QString& path : paths)
	{
		const QString key = pathComparisonKey(path);
		if (listedKeys.contains(key))
			continue;
		listedKeys.insert(key);
		auto* item = new QListWidgetItem(QDir::toNativeSeparators(path), _imageList);
		item->setData(Qt::UserRole, path);
	}
	updateForImageCount();
}

void BatchConvertDialog::removeSelectedImages()
{
	qDeleteAll(_imageList->selectedItems());
	updateForImageCount();
}

QStringList BatchConvertDialog::imagePaths() const
{
	QStringList paths;
	paths.reserve(_imageList->count());
	for (int row = 0; row < _imageList->count(); ++row)
		paths.push_back(_imageList->item(row)->data(Qt::UserRole).toString());
	return paths;
}

void BatchConvertDialog::updateForImageCount()
{
	setWindowTitle(tr("Resize / convert %n image(s)", nullptr, _imageList->count()));
	updateControls();
}

void BatchConvertDialog::loadOptions()
{
	const Options defaults;
	const QSettings settings;
	_resizeGroup->setChecked(settings.value(ResizeKey, defaults.resize.enabled).toBool());
	_fitToggle->setCurrentIndex(storedIndex(settings, FitKey, FitModes.size()));
	_widthBox->setValue(settings.value(WidthKey, defaults.resize.box.width()).toInt());
	_heightBox->setValue(settings.value(HeightKey, defaults.resize.box.height()).toInt());
	_directionToggle->setCurrentIndex(storedIndex(settings, DirectionKey, ScaleDirections.size()));
	_adaptToOrientationBox->setChecked(settings.value(AdaptToOrientationKey, defaults.resize.adaptToOrientation).toBool());
	_formatBox->setCurrentIndex(std::max(_formatBox->findData(settings.value(FormatKey).toString()), 0));
	_qualityBox->setValue(settings.value(QualityKey, defaults.quality).toInt());
	_subsamplingBox->setCurrentIndex(storedIndex(settings, SubsamplingKey, Subsamplings.size()));

	const std::array<QRadioButton*, Locations.size()> locationButtons{ _subfolderButton, _folderButton, _sourceFolderButton };
	locationButtons[static_cast<size_t>(storedIndex(settings, LocationKey, Locations.size()))]->setChecked(true);
	_subfolderEdit->setText(settings.value(SubfolderKey, defaults.subfolderName).toString());
	_folderEdit->setText(QDir::toNativeSeparators(settings.value(FolderKey).toString()));
}

void BatchConvertDialog::saveOptions() const
{
	const Options current = options();
	QSettings settings;
	settings.setValue(ResizeKey, current.resize.enabled);
	settings.setValue(FitKey, indexOf(FitModes, current.resize.fit));
	settings.setValue(WidthKey, current.resize.box.width());
	settings.setValue(HeightKey, current.resize.box.height());
	settings.setValue(DirectionKey, indexOf(ScaleDirections, current.resize.direction));
	settings.setValue(AdaptToOrientationKey, current.resize.adaptToOrientation);
	settings.setValue(FormatKey, current.mimeType);
	settings.setValue(QualityKey, current.quality);
	settings.setValue(SubsamplingKey, indexOf(Subsamplings, current.jpegSubsampling));
	settings.setValue(LocationKey, indexOf(Locations, current.location));
	settings.setValue(SubfolderKey, current.subfolderName);
	settings.setValue(FolderKey, current.folder);
}

Options BatchConvertDialog::options() const
{
	Options current;
	current.resize.enabled = _resizeGroup->isChecked();
	current.resize.fit = FitModes[static_cast<size_t>(_fitToggle->currentIndex())];
	current.resize.box = QSize{ _widthBox->value(), _heightBox->value() };
	current.resize.direction = ScaleDirections[static_cast<size_t>(_directionToggle->currentIndex())];
	current.resize.adaptToOrientation = _adaptToOrientationBox->isChecked();
	current.mimeType = _formatBox->currentData().toString();
	current.quality = _qualityBox->value();
	current.jpegSubsampling = Subsamplings[static_cast<size_t>(_subsamplingBox->currentIndex())];
	current.location = _subfolderButton->isChecked() ? OutputLocation::Subfolder : _folderButton->isChecked() ? OutputLocation::Folder : OutputLocation::SourceFolder;
	current.subfolderName = _subfolderEdit->text().trimmed();
	current.folder = QDir::fromNativeSeparators(_folderEdit->text().trimmed());
	return current;
}

void BatchConvertDialog::updateControls()
{
	const QString format = _formatBox->currentData().toString();
	// "Same as the original" may write lossy formats and JPEG
	_qualityBox->setEnabled(format.isEmpty() || ImageEncoding::isLossy(format));
	_subsamplingBox->setEnabled(format.isEmpty() || format == QLatin1String("image/jpeg"));
	_subfolderEdit->setEnabled(_subfolderButton->isChecked());
	_folderEdit->setEnabled(_folderButton->isChecked());
	_browseButton->setEnabled(_folderButton->isChecked());

	// Portable: the output may be read from another platform.
	const NameRejection subfolderRejection = _subfolderButton->isChecked() ? checkNewEntryName(_subfolderEdit->text().trimmed(), NameRules::Portable) : NameRejection::None;
	QString startBlocker;
	if (_imageList->count() == 0)
		startBlocker = tr("Add images to convert");
	else if (subfolderRejection != NameRejection::None)
		startBlocker = tr("The subfolder name cannot be used. %1").arg(newNameRejectionText(subfolderRejection));
	else if (_folderButton->isChecked() && !QDir::isAbsolutePath(_folderEdit->text().trimmed()))
		startBlocker = tr("Enter the full path of the folder to save to");
	_startButton->setEnabled(startBlocker.isEmpty() && !_job);
	_startButton->setToolTip(startBlocker);
}

void BatchConvertDialog::startConversion()
{
	const Options conversion = options();
	const QStringList sources = imagePaths();
	Plan plan = planTargets(sources, conversion, ClashResolution::Replace);
	if (plan.existingCount > 0)
	{
		QString question = tr("A file of the same name already exists for %n image(s).", nullptr, plan.existingCount);
		if (plan.replacedSourceCount > 0)
			question += "\n\n" + tr("For %n of them it is the original itself: replacing moves the original to Trash.", nullptr, plan.replacedSourceCount);
		QMessageBox box{ QMessageBox::Question, windowTitle(), question, QMessageBox::Cancel, this };
		const QAbstractButton* replace = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
		QPushButton* keepBoth = box.addButton(tr("Keep both"), QMessageBox::AcceptRole);
		const QAbstractButton* skip = box.addButton(tr("Skip"), QMessageBox::AcceptRole);
		box.setDefaultButton(keepBoth);
		box.exec();

		if (box.clickedButton() == keepBoth)
			plan = planTargets(sources, conversion, ClashResolution::KeepBoth);
		else if (box.clickedButton() == skip)
			plan = planTargets(sources, conversion, ClashResolution::Skip);
		else if (box.clickedButton() != replace)
			return;
	}

	_started = true;
	_optionsArea->setEnabled(false);
	_removeAction->setEnabled(false);
	_dropHint->hide();
	_startButton->setEnabled(false);
	_closeButton->setText(tr("Cancel"));
	_targetCount = static_cast<int>(plan.targets.size());
	_progressBar->setRange(0, _targetCount);
	_progressBar->setValue(0);
	_progressBar->show();
	_statusLabel->setText(statusText(false));
	_statusLabel->show();

	_job = new Job(std::move(plan.targets), conversion, [this](size_t targetIndex) { showTargetStarted(targetIndex); },
		[this](const TargetResult& result) { showTargetEnded(result); }, [this] { showFinished(); }, this);
	_job->start();
}

void BatchConvertDialog::showTargetStarted(size_t targetIndex)
{
	QListWidgetItem* item = _imageList->item(static_cast<int>(targetIndex));
	assert_and_return_r(item, );
	styleImageItem(*item, Theme::current().palette.accentText, true);
	_imageList->scrollToItem(item);
}

void BatchConvertDialog::showTargetEnded(const TargetResult& result)
{
	QListWidgetItem* item = _imageList->item(static_cast<int>(result.targetIndex));
	assert_and_return_r(item, );
	const Theme::Theme& theme = Theme::current();

	++_endedCount;
	switch (result.ending)
	{
	case Ending::Written:
		++_writtenCount;
		_writtenSourceBytes += result.sourceBytes;
		_writtenOutputBytes += result.outputBytes;
		styleImageItem(*item, theme.palette.textDim);
		break;
	case Ending::Skipped:
		++_skippedCount;
		styleImageItem(*item, theme.palette.textDim, false, true);
		break;
	case Ending::Cancelled:
		styleImageItem(*item, {});
		break;
	case Ending::Failed:
		++_failedCount;
		styleImageItem(*item, theme.errorText);
		item->setToolTip(result.error);
		_failureList->appendPlainText(QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(result.target.sourcePath), result.error));
		_failureList->show();
		break;
	}

	_progressBar->setValue(_endedCount);
	if (!_closeOnFinish)
		_statusLabel->setText(statusText(false));
}

void BatchConvertDialog::showFinished()
{
	_statusLabel->setText(statusText(true));
	_closeButton->setText(tr("Close"));
	_closeButton->setEnabled(true);
	_job->deleteLater();
	_job = nullptr;

	if (_closeOnFinish)
		QDialog::reject();
}

QString BatchConvertDialog::statusText(bool finished) const
{
	QStringList items;
	if (!finished)
		items.push_back(tr("Converting: %1/%2").arg(_endedCount).arg(_targetCount));
	else
	{
		items.push_back(_endedCount < _targetCount || _closeOnFinish ? tr("Cancelled") : tr("Done"));
		items.push_back(tr("Written: %1/%2").arg(_writtenCount).arg(_targetCount));
		if (_skippedCount > 0)
			items.push_back(tr("Skipped: %1").arg(_skippedCount));
		if (_failedCount > 0)
			items.push_back(tr("Failed: %1").arg(_failedCount));
	}

	if (_writtenSourceBytes > 0)
	{
		const int64_t saved = _writtenSourceBytes - _writtenOutputBytes;
		const QString sign = saved < 0 ? QStringLiteral("−") : QString{};
		items.push_back(tr("Saved: %1").arg(sign + fileSizeToString(static_cast<uint64_t>(std::abs(saved)))));
	}
	return items.join(QStringLiteral("    "));
}
