#include "Utils.h"
#include "Settings.h"

#include <QByteArray>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QWidget>

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

void openInExplorer(const QString& path)
{
	const QFileInfo fi{ path };
	if (fi.exists())
	{
		const QStringList param{ "/select,", QDir::toNativeSeparators(fi.canonicalFilePath()) };
		QProcess::startDetached("explorer.exe", param);
	}
}

void reportMissingFile(QWidget* parent, const QString& path)
{
	QMessageBox::warning(parent, QObject::tr("File not found"), QObject::tr("This file no longer exists:\n%1").arg(path));
}

QString ffmpegPath()
{
	const QString configured = QSettings{}.value(Settings::FfmpegPath).toString();
	return (configured.isEmpty() || !QFile::exists(configured)) ? "ffmpeg" : configured;
}

QString rootFolder()
{
	return QSettings{}.value(Settings::RootFolder, Defaults::RootFolder).toString();
}

const QString PHOTOS_DIR_NAME = "Photos";

QString photosRootFolder()
{
	return rootFolder() + "/" + PHOTOS_DIR_NAME;
}
