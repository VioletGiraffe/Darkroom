#pragma once

#include "BatchConvert/BatchConvertJob.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class SegmentedToggle;

// The resize and format conversion of a set of images: the images and the options, then the progress. The options and the window geometry persist.
// Dropped images join the set until the conversion starts.
// While the conversion runs, closing the dialog cancels it; the dialog closes once the targets in flight end.
class BatchConvertDialog final : public QDialog
{
public:
	// Opens a self-deleting dialog for imagePaths. Parentless: closing the browser must not end a running conversion.
	static void showForImages(const QStringList& imagePaths);

	void done(int result) override;
	void reject() override;

protected:
	void closeEvent(QCloseEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	explicit BatchConvertDialog(const QStringList& imagePaths);

	// Skips the images already listed
	void addImages(const QStringList& paths);
	void removeSelectedImages();
	[[nodiscard]] QStringList imagePaths() const;
	void updateForImageCount();

	void loadOptions();
	void saveOptions() const;
	[[nodiscard]] BatchConvert::Options options() const;
	void updateControls();

	void startConversion();
	void showTargetEnded(const BatchConvert::TargetResult& result);
	void showFinished();

private:
	QWidget* _setupArea = nullptr; // the images and the options
	QListWidget* _imageList = nullptr;
	QGroupBox* _resizeGroup = nullptr;
	SegmentedToggle* _fitToggle = nullptr;
	QSpinBox* _widthBox = nullptr;
	QSpinBox* _heightBox = nullptr;
	SegmentedToggle* _directionToggle = nullptr;
	QCheckBox* _adaptToOrientationBox = nullptr;
	QComboBox* _formatBox = nullptr;
	QSpinBox* _qualityBox = nullptr;
	QComboBox* _subsamplingBox = nullptr;
	QRadioButton* _subfolderButton = nullptr;
	QRadioButton* _folderButton = nullptr;
	QRadioButton* _sourceFolderButton = nullptr;
	QLineEdit* _subfolderEdit = nullptr;
	QLineEdit* _folderEdit = nullptr;
	QPushButton* _browseButton = nullptr;

	QProgressBar* _progressBar = nullptr;
	QLabel* _statusLabel = nullptr;
	QPlainTextEdit* _failureList = nullptr;
	QPushButton* _startButton = nullptr;
	QPushButton* _closeButton = nullptr;

	BatchConvert::Job* _job = nullptr;
	int _targetCount = 0;
	int _endedCount = 0;
	int _writtenCount = 0;
	int _skippedCount = 0;
	int _failedCount = 0;
	bool _closeOnFinish = false;
};
