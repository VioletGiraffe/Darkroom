#include "Windows/FrameCapture.h"

#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Ffmpeg.h"
#include "Import.h"
#include "Settings.h"
#include "Utils.h"

#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTime>
#include <QToolTip>

namespace Settings {
	constexpr const char* LastFrameExtractionMode   = "VideoPlayer/LastFrameExtractionMode";
	constexpr const char* LastFrameExtractionFolder = "VideoPlayer/LastFrameExtractionFolder";
}

namespace {

constexpr const char* ExtractToLibraryMode = "library";
constexpr const char* ExtractToFolderMode  = "folder";

[[nodiscard]] QString extractFrameInto(const QString& videoPath, qint64 timestampMs, const QString& destinationFolder,
	const std::function<void()>& extractionFinished, QWidget* dialogParent)
{
	const bool tiff       = QSettings{}.value(Settings::UseTiff, Defaults::UseTiff).toBool();
	const int jpegQuality = QSettings{}.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();

	// Windows filenames cannot use colon-separated timestamps.
	const QString timestampText = QTime::fromMSecsSinceStartOfDay(static_cast<int>(timestampMs)).toString("h.mm.ss.zzz");
	const QString filePath = destinationFolder + '/' + QFileInfo{ videoPath }.completeBaseName() + ' ' + timestampText
		+ (tiff ? ".tif" : ".jpg");

	const Ffmpeg::SplitResult result = Ffmpeg::extractFrame(videoPath, timestampMs, filePath, jpegQuality);
	if (extractionFinished)
		extractionFinished();
	if (!result.ok())
	{
		reportFfmpegFailure(dialogParent, result, videoPath, destinationFolder);
		return {};
	}
	return filePath;
}

} // namespace

FrameCapture::LastDestination FrameCapture::lastDestination()
{
	const QString mode = QSettings{}.value(Settings::LastFrameExtractionMode).toString();
	if (mode == ExtractToLibraryMode)
		return LastDestination::Library;
	if (mode == ExtractToFolderMode)
		return LastDestination::Folder;
	return LastDestination::None;
}

QString FrameCapture::lastFolder()
{
	return QSettings{}.value(Settings::LastFrameExtractionFolder).toString();
}

void FrameCapture::extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder,
	const std::function<void()>& extractionFinished, QWidget* dialogParent)
{
	const QString filePath = extractFrameInto(videoPath, timestampMs, folder, extractionFinished, dialogParent);
	if (filePath.isEmpty())
		return;

	QSettings settings;
	settings.setValue(Settings::LastFrameExtractionMode, ExtractToFolderMode);
	settings.setValue(Settings::LastFrameExtractionFolder, folder);
	QToolTip::showText(QCursor::pos(), QObject::tr("Frame saved:\n%1").arg(QDir::toNativeSeparators(filePath)), dialogParent);
}

void FrameCapture::extractToLibraryInteractive(Library& library, const QString& videoPath, qint64 timestampMs,
	const std::function<void()>& extractionFinished, QWidget* dialogParent)
{
	Catalog& catalog = library.catalog();

	const QString labelName = QSettings{}.value(Settings::ExtractedLabelName, Defaults::ExtractedLabelName).toString();
	QString error;
	const LabelId labelId = catalog.createLabel(labelName, {}, &error);
	if (labelId == LabelId::None)
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to create the \"%1\" label:\n%2").arg(labelName, error));
		return;
	}

	const QString photoFolder = catalog.photoFolderForLabel(labelId);
	if (photoFolder.isEmpty())
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("The \"%1\" label has no usable photo folder.").arg(labelName));
		return;
	}

	// Keep the final Move on the library's filesystem.
	QTemporaryDir tempDir(library.rootFolder() + "/.extract-XXXXXX");
	if (!tempDir.isValid())
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to create a temporary folder in the library:\n%1").arg(tempDir.errorString()));
		return;
	}

	const QString extractedPath = extractFrameInto(videoPath, timestampMs, tempDir.path(), extractionFinished, dialogParent);
	if (extractedPath.isEmpty())
		return;

	const Import::PhotoResult result = Import::importPhoto(catalog, photoFolder, extractedPath, Import::PhotoImportMode::Move);
	if (result.status != Import::PhotoStatus::Success)
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to import the extracted frame:\n%1").arg(result.errorMessage));
		return;
	}

	QSettings{}.setValue(Settings::LastFrameExtractionMode, ExtractToLibraryMode);
	QToolTip::showText(QCursor::pos(), QObject::tr("Frame imported into the library under \"%1\"").arg(labelName), dialogParent);
}
