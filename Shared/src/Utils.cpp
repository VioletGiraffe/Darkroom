#include "Utils.h"
#include "Ffmpeg.h"
#include "Settings.h"
#include "SharedSettings.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"

DISABLE_COMPILER_WARNINGS
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <functional>

void clearStuckHoverIfCursorLeft(QWidget* w)
{
	if (!w->rect().contains(w->mapFromGlobal(QCursor::pos())))
	{
		w->setAttribute(Qt::WA_UnderMouse, false);
		w->update();
	}
}

QStringList pickEvenlySpacedFrames(const QDir& dir, const QStringList& files, int maxFrames)
{
	const int count = static_cast<int>(files.size());
	const int n = qMin(count, maxFrames);

	int startIdx = static_cast<int>(count * 0.1f);
	int endIdx   = static_cast<int>(count * 0.9f);
	if (endIdx - startIdx + 1 < n)
	{
		startIdx = 0;
		endIdx = count - 1;
	}

	QStringList out;
	out.reserve(n);
	for (int i = 0; i < n; ++i)
	{
		const int idx = (n == 1) ? startIdx : startIdx + i * (endIdx - startIdx) / (n - 1);
		out << dir.filePath(files[idx]);
	}
	return out;
}

static const QStringList SUPPORTED_VIDEO_SUFFIXES {
	"mp4", "m4v", "mov", "avi", "mkv", "flv", "webm", "wmv", "mpg", "mpeg", "m2ts", "mts", "ts"
};

// Keep this to formats the deployed Qt image plugins decode.
static const QStringList SUPPORTED_IMAGE_SUFFIXES { "jpg", "jpeg", "jfif", "png", "tif", "tiff", "webp", "gif", "bmp", "svg" };

static QString suffixesToGlobs(const QStringList& suffixes)
{
	return QStringLiteral("*.") + suffixes.join(QStringLiteral(" *."));
}

bool isSupportedVideoFile(const QString& filePath)
{
	return SUPPORTED_VIDEO_SUFFIXES.contains(QFileInfo(filePath).suffix().toLower());
}

bool isSupportedImageFile(const QString& filePath)
{
	return SUPPORTED_IMAGE_SUFFIXES.contains(QFileInfo(filePath).suffix().toLower());
}

QString supportedVideoFileGlobs()
{
	static const QString globs = suffixesToGlobs(SUPPORTED_VIDEO_SUFFIXES);
	return globs;
}

QString supportedImageFileGlobs()
{
	static const QString globs = suffixesToGlobs(SUPPORTED_IMAGE_SUFFIXES);
	return globs;
}

bool isSupportedMediaFile(const QString& filePath)
{
	return isSupportedVideoFile(filePath) || isSupportedImageFile(filePath);
}

bool isDirectoryOrSupportedFile(const QString& path)
{
	return QFileInfo(path).isDir() || isSupportedMediaFile(path);
}

QStringList collectFilesInDirectory(const QString& directory, bool recursive, const std::function<bool(const QString&)>& filterPredicate)
{
	QStringList files;
	QDirIterator it(directory, QDir::Files, recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
	while (it.hasNext())
	{
		it.next();
		if (filterPredicate(it.filePath()))
			files.push_back(it.filePath());
	}
	return files;
}

bool hasSupportedPaths(const QMimeData* mime)
{
	for (const QUrl& url : mime->urls())
	{
		if (const QString path = url.toLocalFile(); isDirectoryOrSupportedFile(path))
			return true;
	}
	return false;
}

QStringList supportedPaths(const QMimeData* mime)
{
	QStringList paths;
	for (const QUrl& url : mime->urls())
	{
		if (const QString path = url.toLocalFile(); isDirectoryOrSupportedFile(path))
			paths.push_back(path);
	}
	return paths;
}

bool filesAreIdentical(const QString& pathA, const QString& pathB)
{
	if (QFileInfo(pathA).size() != QFileInfo(pathB).size())
		return false;

	QFile fileA(pathA);
	QFile fileB(pathB);
	if (!fileA.open(QIODevice::ReadOnly) || !fileB.open(QIODevice::ReadOnly))
		return false;

	// Files of equal size but different content usually differ early.
	constexpr qint64 chunkSize = 512 * 1024;
	while (!fileA.atEnd())
	{
		if (fileA.read(chunkSize) != fileB.read(chunkSize))
			return false;
	}
	return true;
}

bool deleteFolderRecursivelyIfPresent(const QString& folderPath)
{
	if (folderPath.isEmpty())
		return false;

	const QFileInfo info(folderPath);
	if (!info.exists() && !info.isSymLink())
		return true;
	if (!info.isDir())
		return false;
	return QDir(folderPath).removeRecursively();
}

QString pathComparisonKey(const QString& path)
{
	// Callers that must see through aliases canonicalize their shared root before using this lexical key.
	return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

bool isPathAtOrUnder(const QString& path, const QStringList& roots)
{
	if (path.isEmpty())
		return false;

	const QString key = pathComparisonKey(path);
	return std::any_of(roots.cbegin(), roots.cend(), [&key](const QString& root) {
		if (root.isEmpty())
			return false;
		const QString rootKey = pathComparisonKey(root);
		return key == rootKey || key.startsWith(rootKey + '/');
	});
}

std::optional<QString> pathRelativeTo(const QString& path, const QString& root)
{
	if (!isPathAtOrUnder(path, { root }))
		return std::nullopt;

	// By segment count: lowercasing can change a key's length.
	const QStringList segments = QDir::cleanPath(QDir::fromNativeSeparators(path)).split('/');
	const qsizetype rootDepth = pathComparisonKey(root).count('/') + 1;
	return segments.mid(rootDepth).join('/');
}

QString pathAfterMoves(const QString& path, const std::vector<PathMove>& moves)
{
	for (const PathMove& move : moves)
		if (const std::optional<QString> relative = pathRelativeTo(path, move.from))
			return relative->isEmpty() ? move.to : move.to + '/' + *relative;
	return path;
}

QChar invalidFilenameChar(const QString& name)
{
	static const QString invalidChars = R"(\/:*?"<>|)";
	for (const QChar c : name)
		if (invalidChars.contains(c))
			return c;
	return {};
}

static const QStringList FRAME_IMAGE_SUFFIXES { "jpg", "jpeg", "tif", "tiff", "png" };

bool isFrameImageFile(const QString& fileName)
{
	return FRAME_IMAGE_SUFFIXES.contains(QFileInfo(fileName).suffix(), Qt::CaseInsensitive);
}

QStringList listFrameImageFiles(const QDir& dir)
{
	static const QStringList frameImageGlobs = suffixesToGlobs(FRAME_IMAGE_SUFFIXES).split(' ');
	return dir.entryList(frameImageGlobs, QDir::Files, QDir::Name);
}

QDateTime parseTrailingTimestamp(const QString& text)
{
	static const QRegularExpression re(R"((\d{14})\d*$)");
	const QRegularExpressionMatch match = re.match(text);
	if (!match.hasMatch())
		return {};

	const QDateTime dt = QDateTime::fromString(match.captured(1), "yyyyMMddHHmmss");
	if (!dt.isValid())
		return {};

	// Guards against an unrelated 14+ digit trailing run (e.g. a device serial) that
	// happens to parse as a syntactically valid date.
	const int year = dt.date().year();
	if (year < 1990 || year > QDate::currentDate().year() + 1)
		return {};

	return dt;
}

QDateTime getSourceFileDate(const QString& sourcePath, const QString& folderPath)
{
	if (!sourcePath.isEmpty())
	{
		const QDateTime fromName = parseTrailingTimestamp(QFileInfo(sourcePath).completeBaseName());
		if (fromName.isValid())
			return fromName;

		const QFileInfo sourceInfo(sourcePath);
		if (sourceInfo.exists())
		{
			const QDateTime birth = sourceInfo.birthTime();
			return birth.isValid() ? birth : sourceInfo.lastModified();
		}
	}

	const QFileInfo folderInfo(folderPath);
	const QDateTime birth = folderInfo.birthTime();
	return birth.isValid() ? birth : folderInfo.lastModified();
}

void reportMissingFile(QWidget* parent, const QString& path)
{
	QMessageBox::warning(parent, QObject::tr("File not found"), QObject::tr("This file or folder no longer exists:\n") + path);
}

void reportFfmpegFailure(QWidget* parent, const Ffmpeg::SplitResult& result, const QString& videoFilePath, const QString& outputTarget)
{
	using Status = Ffmpeg::SplitResult::Status;
	switch (result.status)
	{
	case Status::Ok:
		break;
	case Status::SourceMissing:
		reportMissingFile(parent, videoFilePath);
		break;
	case Status::FolderCreateFailed:
		QMessageBox::critical(parent, QObject::tr("Error"), QObject::tr("Failed to create output folder:\n%1").arg(outputTarget));
		break;
	case Status::StartFailed:
	{
		const QString ffmpeg = ffmpegPath();
		QMessageBox::critical(parent, QObject::tr("Error"), ffmpeg.isEmpty()
			? QObject::tr("FFMPEG was not found. Install it, or set the path to the binary in Settings.")
			: QObject::tr("Failed to start the FFMPEG process at:\n%1").arg(ffmpeg));
		break;
	}
	case Status::TimedOut:
		QMessageBox::critical(parent, QObject::tr("Error"), QObject::tr("The FFMPEG process timed out and was terminated."));
		break;
	case Status::ExtractionFailed:
		// ffmpeg's own output goes in the scrollable body - it runs to hundreds of lines for some failures.
		MessageDialog::notice(parent, QObject::tr("Error"),
			QObject::tr("FFMPEG failed with exit code %1.").arg(result.exitCode),
			result.errorOutput.trimmed(), QMessageBox::Critical);
		break;
	case Status::NoFrames:
		QMessageBox::warning(parent, QObject::tr("Warning"), QObject::tr("No frames were extracted from:\n%1").arg(videoFilePath));
		break;
	case Status::Cancelled:
		break;
	}
}

QString autoDetectedFfmpegPath()
{
	const QString executableName = QStringLiteral("ffmpeg");

	// Search beside the app first. On macOS, climb out of Contents/MacOS to the directory beside the bundle.
#ifdef Q_OS_MACOS
	const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../..");
#else
	const QString appDir = QCoreApplication::applicationDirPath();
#endif
	if (const QString besideApp = QStandardPaths::findExecutable(executableName, { appDir }); !besideApp.isEmpty())
		return besideApp;

	if (const QString onPath = QStandardPaths::findExecutable(executableName); !onPath.isEmpty())
		return onPath;

#ifdef Q_OS_MACOS
	// Finder-launched apps do not inherit the shell PATH containing Homebrew or MacPorts.
	return QStandardPaths::findExecutable(executableName, { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin" });
#else
	return {};
#endif
}

QString ffmpegPath()
{
	const QString configured = readSharedSetting(Settings::FfmpegPath).toString();
	return (!configured.isEmpty() && QFile::exists(configured)) ? configured : autoDetectedFfmpegPath();
}
