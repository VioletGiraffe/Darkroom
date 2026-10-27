#include "Windows/SingleFrameExtraction.h"

#include "Ffmpeg.h"
#include "SharedSettings.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QSettings>
#include <QTime>
#include <QToolTip>
RESTORE_COMPILER_WARNINGS

namespace Settings {
	constexpr const char* LastFrameExtractionMode   = "VideoPlayer/LastFrameExtractionMode";
	constexpr const char* LastFrameExtractionFolder = "VideoPlayer/LastFrameExtractionFolder";
}

namespace {

constexpr const char* ExtractToTargetMode = "library"; // value written when the library was the only target
constexpr const char* ExtractToFolderMode = "folder";

[[nodiscard]] QString firstAvailableFramePath(
	const QString& videoPath, qint64 timestampMs, const QString& destinationFolder, bool tiff)
{
	// Windows filenames cannot use colon-separated timestamps.
	const QString timestampText = QTime::fromMSecsSinceStartOfDay(static_cast<int>(timestampMs)).toString("h.mm.ss.zzz");
	const QString stem = QFileInfo{ videoPath }.completeBaseName() + ' ' + timestampText;
	const QString extension = tiff ? ".tif" : ".jpg";
	for (int copyIndex = 0; ; ++copyIndex)
	{
		const QString numberedSuffix = copyIndex == 0 ? QString{} : QString(" (%1)").arg(copyIndex);
		const QString candidatePath = QDir{ destinationFolder }.filePath(stem + numberedSuffix + extension);
		if (!QFileInfo::exists(candidatePath))
			return candidatePath;
	}
}

} // namespace

SingleFrameExtraction::LastDestination SingleFrameExtraction::lastDestination()
{
	const QString mode = QSettings{}.value(Settings::LastFrameExtractionMode).toString();
	if (mode == ExtractToTargetMode)
		return LastDestination::Target;
	if (mode == ExtractToFolderMode)
		return LastDestination::Folder;
	return LastDestination::None;
}

QString SingleFrameExtraction::lastFolder()
{
	return QSettings{}.value(Settings::LastFrameExtractionFolder).toString();
}

void SingleFrameExtraction::rememberTargetAsLastDestination()
{
	QSettings{}.setValue(Settings::LastFrameExtractionMode, ExtractToTargetMode);
}

QString SingleFrameExtraction::extractFrame(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent)
{
	if (!QDir{}.mkpath(folder))
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"), QObject::tr("Failed to create the folder:\n%1").arg(QDir::toNativeSeparators(folder)));
		return {};
	}

	const QSettings settings = sharedSettings();
	const bool tiff       = settings.value(Settings::UseTiff, Defaults::UseTiff).toBool();
	const int jpegQuality = settings.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();
	const QString filePath = firstAvailableFramePath(videoPath, timestampMs, folder, tiff);

	const Ffmpeg::SplitResult result = Ffmpeg::extractSingleFrame(videoPath, timestampMs, filePath, jpegQuality);
	if (!result.ok())
	{
		reportFfmpegFailure(dialogParent, result, videoPath, folder);
		return {};
	}
	return filePath;
}

void SingleFrameExtraction::extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent)
{
	const QString filePath = extractFrame(videoPath, timestampMs, folder, dialogParent);
	if (filePath.isEmpty())
		return;

	QSettings settings;
	settings.setValue(Settings::LastFrameExtractionMode, ExtractToFolderMode);
	settings.setValue(Settings::LastFrameExtractionFolder, folder);
	QToolTip::showText(QCursor::pos(), QObject::tr("Frame saved:\n%1").arg(QDir::toNativeSeparators(filePath)), dialogParent);
}
