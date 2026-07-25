#pragma once

// forEachFolder uses QDir in this header; QDir also supplies QString and QStringList.
#include <QDir>

#include <functional>

class QDateTime;
class QMimeData;
class QWidget;

void saveWindowGeometry(QWidget* w, const QString& key);
bool restoreWindowGeometry(QWidget* w, const QString& key);

// Clears a stale QSS :hover after a popup's mouse grab bypasses normal leave dispatch. A synthetic Leave is
// insufficient because :hover follows WA_UnderMouse; a real hover is preserved when the cursor has not moved.
void clearStuckHoverIfCursorLeft(QWidget* w);

// Returns up to maxFrames full paths, sampled evenly over the middle 10%-90% of the sorted input.
[[nodiscard]] QStringList pickEvenlySpacedFrames(const QDir& dir, const QStringList& files, int maxFrames);

[[nodiscard]] bool isSupportedVideoFile(const QString& filePath);
[[nodiscard]] bool isSupportedImageFile(const QString& filePath);
[[nodiscard]] bool isSupportedMediaFile(const QString& filePath);

[[nodiscard]] bool isDirectoryOrSupportedFile(const QString& path);

[[nodiscard]] bool hasSupportedPaths(const QMimeData* mime);
[[nodiscard]] QStringList supportedPaths(const QMimeData* mime);

// Byte-for-byte comparison with a size short-circuit.
[[nodiscard]] bool filesAreIdentical(const QString& pathA, const QString& pathB);

// Lexical, case/separator-insensitive path key. Does not resolve symlinks or junctions.
[[nodiscard]] QString pathComparisonKey(const QString& path);

// Returns the first Windows-illegal filename character, or a null QChar.
[[nodiscard]] QChar invalidFilenameChar(const QString& name);

extern const QStringList IMAGE_FILE_FILTERS;

// Name-sorted frame image filenames directly in dir. Suffix matching remains case-insensitive on Linux.
[[nodiscard]] QStringList listFrameImageFiles(const QDir& dir);

// Full paths accepted by filterPredicate, recursively or among immediate children. Order is filesystem-defined.
[[nodiscard]] QStringList collectFilesInDirectory(const QString& directory, bool recursive,
                                                  const std::function<bool(const QString&)>& filterPredicate);

// Visits every (storageFolder, folderPath) pair under root, in storage-folder-name order.
template <typename F>
void forEachFolder(const QString& root, F&& callback)
{
	const QDir rootDir(root);
	for (const QString& storageFolder : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
	{
		const QDir storageDir(rootDir.filePath(storageFolder));
		for (const QString& folder : storageDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
			callback(storageFolder, storageDir.filePath(folder));
	}
}

// Parses the first yyyyMMddHHmmss of a trailing digit run; extra sub-second digits are ignored. The caller
// removes any extension. Returns an invalid QDateTime when the suffix is absent or invalid.
[[nodiscard]] QDateTime parseTrailingTimestamp(const QString& text);

// Best-effort recording date: filename timestamp, then source creation time, then frame-folder creation time.
[[nodiscard]] QDateTime getSourceFileDate(const QString& sourcePath, const QString& folderPath);

// Selects path in the platform file manager. False means only that path did not exist; the handoff itself is best-effort.
[[nodiscard]] bool revealInFileManager(const QString& path);

// Platform-native menu wording for revealInFileManager.
[[nodiscard]] QString revealInFileManagerActionText();

void reportMissingFile(QWidget* parent, const QString& path);

namespace Ffmpeg { struct SplitResult; }

// Presents a failed split/extract result; no-op on Ok.
void reportFfmpegFailure(QWidget* parent, const Ffmpeg::SplitResult& result, const QString& videoFilePath, const QString& outputTarget);

// Configured ffmpeg path when valid, otherwise autoDetectedFfmpegPath(). Empty is safe to pass to QProcess.
[[nodiscard]] QString ffmpegPath();

// Auto-detects beside the app, on PATH, or in common macOS package-manager locations.
[[nodiscard]] QString autoDetectedFfmpegPath();
