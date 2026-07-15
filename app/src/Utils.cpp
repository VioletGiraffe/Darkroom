#include "Utils.h"
#include "Settings.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
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

// QtDBus is linked on these platforms only (see app.pro), for revealInFileManager.
#if !defined Q_OS_WIN && !defined Q_OS_MACOS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
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
	// Importable photo formats - bounded by what Qt's image plugins decode (cards render via QImage).
	static const QStringList supportedExtensions { "jpg", "jpeg", "png", "tif", "tiff", "webp", "bmp" };
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
	// A non-local URL yields an empty local path, which isDirectoryOrSupportedFile rejects - so this also filters those out.
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

QString pathComparisonKey(const QString& path)
{
	const QString canonical = QFileInfo(path).canonicalFilePath();
	return (canonical.isEmpty() ? QDir::cleanPath(path) : canonical).toLower();
}

QChar invalidFilenameChar(const QString& name)
{
	static const QString invalidChars = R"(\/:*?"<>|)";
	for (const QChar c : name)
		if (invalidChars.contains(c))
			return c;
	return {};
}

const QStringList IMAGE_FILE_FILTERS { "*.jpg", "*.jpeg", "*.tif", "*.tiff", "*.png" };

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
	// The only interface that selects the item instead of merely opening its folder; Nautilus/Dolphin/Nemo/Caja/Thunar
	// implement it, and the name is D-Bus-activatable, so this also starts a file manager that isn't running yet.
	// Raw method call: QDBusInterface's constructor would spend a blocking introspection round-trip first.
	QDBusMessage showItems = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
	                                                        "org.freedesktop.FileManager1", "ShowItems");
	showItems.setArguments({ QStringList{ QUrl::fromLocalFile(canonicalPath).toString() }, QString{} });

	// Async: that activation can take seconds, which a blocking call() would spend with the UI frozen. qApp parents the
	// watcher (bounding its life if no reply comes) and is the connect context, putting the GUI-thread-only openUrl on
	// the main thread.
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

void reportMissingFile(QWidget* parent, const QString& path)
{
	QMessageBox::warning(parent, QObject::tr("File not found"), QObject::tr("This file or folder no longer exists:\n") + path);
}

QString autoDetectedFfmpegPath()
{
	const QString executableName = QStringLiteral("ffmpeg");

	// Beside the app first: that's the precedence Windows gives a local binary when handed a bare name. On macOS
	// applicationDirPath() points inside the bundle (Darkroom.app/Contents/MacOS), so climb out of it - a
	// hand-placed binary sits next to Darkroom.app, not within it.
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
	// A Finder-launched app inherits launchd's minimal PATH, so a Homebrew or MacPorts ffmpeg that runs fine from a
	// terminal is invisible to the PATH search above. Compiled out elsewhere rather than handed an empty directory
	// list, which findExecutable would answer by searching PATH a second time.
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
