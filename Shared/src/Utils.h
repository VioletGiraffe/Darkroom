#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
// Several declarations take QDir; QDir also supplies QString and QStringList.
#include <QDir>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <optional>
#include <vector>

class QDateTime;
class QMimeData;
class QWidget;

// Clears a stale QSS :hover after a popup's mouse grab bypasses normal leave dispatch. A synthetic Leave is
// insufficient because :hover follows WA_UnderMouse; a real hover is preserved when the cursor has not moved.
void clearStuckHoverIfCursorLeft(QWidget* w);

// Returns min(maxFrames, files.size()) distinct full paths, sampled evenly over the middle 10%-90% of the sorted input,
// or over all of it when the window is smaller than that.
[[nodiscard]] QStringList pickEvenlySpacedFrames(const QDir& dir, const QStringList& files, int maxFrames);

[[nodiscard]] bool isSupportedVideoFile(const QString& filePath);
[[nodiscard]] bool isSupportedImageFile(const QString& filePath);
// Space-separated globs of the same suffixes, for a file dialog name filter.
[[nodiscard]] QString supportedVideoFileGlobs();
[[nodiscard]] QString supportedImageFileGlobs();
[[nodiscard]] bool isSupportedMediaFile(const QString& filePath);

[[nodiscard]] bool isDirectoryOrSupportedFile(const QString& path);

[[nodiscard]] bool hasSupportedPaths(const QMimeData* mime);
[[nodiscard]] QStringList supportedPaths(const QMimeData* mime);

// Byte-for-byte comparison with a size short-circuit.
[[nodiscard]] bool filesAreIdentical(const QString& pathA, const QString& pathB);

// An empty path fails; an already-absent path succeeds.
[[nodiscard]] bool deleteFolderRecursivelyIfPresent(const QString& folderPath);

// Lexical, case/separator-insensitive path key. Does not resolve symlinks or junctions.
[[nodiscard]] QString pathComparisonKey(const QString& path);

// Whether path is one of roots or lies under one, by pathComparisonKey. Empty paths and roots never match.
[[nodiscard]] bool isPathAtOrUnder(const QString& path, const QStringList& roots);

// path below root, '/'-separated and spelled as in path, by pathComparisonKey. Empty for root itself; nullopt when path is not at or under root.
[[nodiscard]] std::optional<QString> pathRelativeTo(const QString& path, const QString& root);

// A completed move of a file or folder.
struct PathMove
{
	QString from;
	QString to;
};
// path rebased by the first move from it or from a folder above it; path itself when no move applies.
[[nodiscard]] QString pathAfterMoves(const QString& path, const std::vector<PathMove>& moves);

// Returns the first Windows-illegal filename character, or a null QChar.
[[nodiscard]] QChar invalidFilenameChar(const QString& name);

// Name-sorted frame image filenames directly in dir. Suffix matching is case-insensitive.
[[nodiscard]] QStringList listFrameImageFiles(const QDir& dir);
// Whether listFrameImageFiles would list a file of this name.
[[nodiscard]] bool isFrameImageFile(const QString& fileName);

// Full paths accepted by filterPredicate, recursively or among immediate children. Order is filesystem-defined.
[[nodiscard]] QStringList collectFilesInDirectory(const QString& directory, bool recursive,
                                                  const std::function<bool(const QString&)>& filterPredicate);

// Parses the first yyyyMMddHHmmss of a trailing digit run; extra sub-second digits are ignored. The caller
// removes any extension. Returns an invalid QDateTime when the suffix is absent or invalid.
[[nodiscard]] QDateTime parseTrailingTimestamp(const QString& text);

// Best-effort recording date: filename timestamp, then source creation time, then frame-folder creation time.
[[nodiscard]] QDateTime getSourceFileDate(const QString& sourcePath, const QString& folderPath);

constexpr const char* IssueTrackerUrl = "https://github.com/VioletGiraffe/Darkroom/issues";

void reportMissingFile(QWidget* parent, const QString& path);

namespace Ffmpeg { struct SplitResult; }

// Presents a failed split/extract result; no-op on Ok and Cancelled.
void reportFfmpegFailure(QWidget* parent, const Ffmpeg::SplitResult& result, const QString& videoFilePath, const QString& outputTarget);

// Configured ffmpeg path when valid, otherwise autoDetectedFfmpegPath(). Empty is safe to pass to QProcess.
[[nodiscard]] QString ffmpegPath();

// Auto-detects beside the app, on PATH, or in common macOS package-manager locations.
[[nodiscard]] QString autoDetectedFfmpegPath();

namespace ImageDecoding { enum class RawRendering; }

// The shared Settings::RawRendering: how camera raws open in the viewer and in photo compare
[[nodiscard]] ImageDecoding::RawRendering rawRenderingSetting();
void saveRawRenderingSetting(ImageDecoding::RawRendering rawRendering);
