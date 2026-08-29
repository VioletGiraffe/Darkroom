#include "Utils.h"
#include "Ffmpeg.h"
#include "Settings.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagebox.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

// QtDBus is linked on these platforms only (see app.pro), for revealInFileManager.
#if !defined Q_OS_WIN && !defined Q_OS_MACOS
DISABLE_COMPILER_WARNINGS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
RESTORE_COMPILER_WARNINGS
#endif

#include <functional>

void saveWindowGeometry(QWidget* w, const QString& key)
{
	QSettings{}.setValue(key + "/geometry", w->saveGeometry());
}

bool restoreWindowGeometry(QWidget* w, const QString& key)
{
	const QByteArray ba = QSettings{}.value(key + "/geometry").toByteArray();
	return !ba.isEmpty() ? w->restoreGeometry(ba) : false;
}

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

	const int startIdx = static_cast<int>(count * 0.1f);
	const int endIdx   = static_cast<int>(count * 0.9f);

	QStringList out;
	out.reserve(n);
	for (int i = 0; i < n; ++i)
	{
		const int idx = (n == 1) ? startIdx : startIdx + i * (endIdx - startIdx) / (n - 1);
		out << dir.filePath(files[idx]);
	}
	return out;
}

bool isSupportedVideoFile(const QString& filePath)
{
	static const QStringList supportedExtensions { "mp4", "mov", "avi", "mkv", "flv" };
	const QString extension = QFileInfo(filePath).suffix().toLower();
	return supportedExtensions.contains(extension);
}

bool isSupportedImageFile(const QString& filePath)
{
	// Keep this to formats the deployed Qt image plugins decode.
	static const QStringList supportedExtensions { "jpg", "jpeg", "jfif", "png", "tif", "tiff", "webp", "bmp" };
	const QString extension = QFileInfo(filePath).suffix().toLower();
	return supportedExtensions.contains(extension);
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

	constexpr qint64 chunkSize = 4 * 1024 * 1024;
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

QChar invalidFilenameChar(const QString& name)
{
	static const QString invalidChars = R"(\/:*?"<>|)";
	for (const QChar c : name)
		if (invalidChars.contains(c))
			return c;
	return {};
}

static const QStringList FRAME_IMAGE_SUFFIXES { "jpg", "jpeg", "tif", "tiff", "png" };

const QStringList IMAGE_FILE_FILTERS = [] {
	QStringList globs;
	for (const QString& suffix : FRAME_IMAGE_SUFFIXES)
		globs << "*." + suffix;
	return globs;
}();

QStringList listFrameImageFiles(const QDir& dir)
{
	// Not entryList(IMAGE_FILE_FILTERS): QDir name filters match case-sensitively on a case-sensitive
	// filesystem and would miss e.g. ".JPG" files there.
	QStringList files = dir.entryList(QDir::Files, QDir::Name);
	files.removeIf([](const QString& fileName) { return !FRAME_IMAGE_SUFFIXES.contains(QFileInfo(fileName).suffix().toLower()); });
	return files;
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

bool revealInFileManager(const QString& path)
{
	const QFileInfo fi{ path };
	if (!fi.exists())
		return false;

	const QString canonicalPath = fi.canonicalFilePath();

#if defined Q_OS_WIN
	QProcess::startDetached("explorer.exe", { "/select,", QDir::toNativeSeparators(canonicalPath) });
#elif defined Q_OS_MACOS
	QProcess::startDetached("open", { "-R", canonicalPath });
#else
	// This D-Bus interface selects the item and activates a supported file manager if necessary. Construct the
	// call directly to avoid QDBusInterface's blocking introspection.
	QDBusMessage showItems = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
	                                                        "org.freedesktop.FileManager1", "ShowItems");
	showItems.setArguments({ QStringList{ QUrl::fromLocalFile(canonicalPath).toString() }, QString{} });

	// Activation can take seconds. qApp bounds the async watcher's lifetime and keeps the fallback on the GUI thread.
	const QUrl folderUrl = QUrl::fromLocalFile(fi.isDir() ? canonicalPath : fi.absolutePath());
	auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(showItems), qApp);
	QObject::connect(watcher, &QDBusPendingCallWatcher::finished, qApp, [folderUrl](QDBusPendingCallWatcher* self) {
		if (self->isError())
			QDesktopServices::openUrl(folderUrl);
		self->deleteLater();
	});
#endif

	return true;
}

QString revealInFileManagerActionText()
{
#if defined Q_OS_WIN
	return QObject::tr("Open in Explorer");
#elif defined Q_OS_MACOS
	return QObject::tr("Reveal in Finder");
#else
	return QObject::tr("Show in file manager");
#endif
}

bool openFolderInFileManager(const QString& folderPath)
{
	if (!QFileInfo(folderPath).isDir())
		return false;

	QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
	return true;
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
		MessageBox::notice(parent, QObject::tr("Error"),
			QObject::tr("FFMPEG failed with exit code %1.").arg(result.exitCode),
			result.errorOutput.trimmed(), QMessageBox::Critical);
		break;
	case Status::NoFrames:
		QMessageBox::warning(parent, QObject::tr("Warning"), QObject::tr("No frames were extracted from:\n%1").arg(videoFilePath));
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
	const QString configured = QSettings{}.value(Settings::FfmpegPath).toString();
	return (!configured.isEmpty() && QFile::exists(configured)) ? configured : autoDetectedFfmpegPath();
}
