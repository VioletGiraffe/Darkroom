#include "Utils.h"
#include "TestHelpers.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <vector>

TEST_CASE("listFrameImageFiles: matches every image suffix case-insensitively, excludes the rest", "[utils]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QDir d(dir.path());

	for (const char* name : { "a.jpg", "B.JPG", "c.jpeg", "d.PNG", "e.tif", "f.TIFF" })
		writeTextFile(d.filePath(name), "x");
	writeTextFile(d.filePath("notes.txt"), "x");   // wrong suffix
	writeTextFile(d.filePath("noext"), "x");        // no suffix at all
	REQUIRE(d.mkdir("sub"));                        // a directory is not a file
	writeTextFile(d.filePath("sub/inner.png"), "x"); // and a nested image is not a direct child

	// Sorted for the compare: the function returns QDir enumeration order, whose case handling varies by
	// platform - not our logic to pin. What is ours is that ".JPG"/".PNG"/".TIFF" are matched alongside
	// their lowercase spellings while the non-image entries are dropped.
	QStringList files = listFrameImageFiles(d);
	files.sort();
	REQUIRE(files == QStringList({ "B.JPG", "a.jpg", "c.jpeg", "d.PNG", "e.tif", "f.TIFF" }));
}

TEST_CASE("isPathAtOrUnder: the root itself and its descendants, case- and separator-insensitively", "[utils]")
{
	const QStringList roots{ "C:/Lib/Photos/Cats" };
	REQUIRE(isPathAtOrUnder("C:/Lib/Photos/Cats", roots));
	REQUIRE(isPathAtOrUnder("c:/lib/photos/cats/a.gif", roots));
#ifdef Q_OS_WIN
	REQUIRE(isPathAtOrUnder("c:\\lib\\photos\\cats\\a.gif", roots)); // a backslash separates on Windows only
#endif
	REQUIRE(isPathAtOrUnder("C:/Lib/Photos/Dogs/../Cats/a.gif", roots));
	REQUIRE(isPathAtOrUnder("C:/Lib/Photos/Cats/a.gif", { "C:/Lib/Photos/Cats/" }));
	REQUIRE(isPathAtOrUnder("C:/Lib/Photos/Dogs/a.gif", { "C:/Lib/Photos/Cats", "C:/Lib/Photos/Dogs" }));
	REQUIRE_FALSE(isPathAtOrUnder("C:/Lib/Photos/Catsup/a.gif", roots)); // a sibling sharing the name prefix
	REQUIRE_FALSE(isPathAtOrUnder("C:/Lib/Photos", roots));
	REQUIRE_FALSE(isPathAtOrUnder("", roots));
	REQUIRE_FALSE(isPathAtOrUnder("/x", { "" }));
}

TEST_CASE("pathRelativeTo: the remainder below root in the path's spelling; empty for root itself, nullopt outside it", "[utils]")
{
	REQUIRE(pathRelativeTo("C:/Lib/Photos/Trip/A.jpg", "C:/Lib") == "Photos/Trip/A.jpg");
	REQUIRE(pathRelativeTo("c:/lib/Photos/Trip/A.jpg", "C:/Lib/") == "Photos/Trip/A.jpg");
#ifdef Q_OS_WIN
	REQUIRE(pathRelativeTo("C:\\Lib\\Photos\\A.jpg", "C:/Lib") == "Photos/A.jpg");
#endif
	REQUIRE(pathRelativeTo("C:/Lib", "C:/Lib") == "");
	REQUIRE_FALSE(pathRelativeTo("C:/Library/A.jpg", "C:/Lib"));
	REQUIRE_FALSE(pathRelativeTo("D:/videos/Clip.mp4", "C:/Lib"));
	REQUIRE_FALSE(pathRelativeTo("", "C:/Lib"));
}

TEST_CASE("pathAfterMoves: rebases the moved path and its descendants, keeping the descendants' spelling", "[utils]")
{
	const std::vector<PathMove> moves{ { "C:/Lib/Photos/Cats", "C:/Lib/Photos/Kittens" }, { "C:/v/clip.mp4", "C:/v/renamed.mp4" } };
	REQUIRE(pathAfterMoves("C:/Lib/Photos/Cats", moves) == "C:/Lib/Photos/Kittens");
	REQUIRE(pathAfterMoves("c:/lib/photos/cats/Sub/A.gif", moves) == "C:/Lib/Photos/Kittens/Sub/A.gif");
	REQUIRE(pathAfterMoves("C:/V/Clip.mp4", moves) == "C:/v/renamed.mp4");
#ifdef Q_OS_WIN
	REQUIRE(pathAfterMoves("C:\\v\\clip.mp4", moves) == "C:/v/renamed.mp4");
#endif
	REQUIRE(pathAfterMoves("C:/Lib/Photos/Catsup/a.gif", moves) == "C:/Lib/Photos/Catsup/a.gif");
	REQUIRE(pathAfterMoves("", moves).isEmpty());
}

TEST_CASE("pathAfterMoves: the first applicable move wins", "[utils]")
{
	const std::vector<PathMove> moves{ { "C:/a/b", "C:/x" }, { "C:/a", "C:/y" } };
	REQUIRE(pathAfterMoves("C:/a/b/c.jpg", moves) == "C:/x/c.jpg");
	REQUIRE(pathAfterMoves("C:/a/d.jpg", moves) == "C:/y/d.jpg");
}

TEST_CASE("pathAfterMoves: a moved folder whose lowercase key is longer than its name", "[utils]")
{
	// U+0130 lowercases to two UTF-16 units, so the key and the path disagree on where the folder ends.
	REQUIRE(pathComparisonKey("C:/İstanbul").size() > QString("C:/İstanbul").size());
	REQUIRE(pathAfterMoves("C:/İstanbul/Day 1/a.jpg", { { "C:/İstanbul", "C:/Trips" } }) == "C:/Trips/Day 1/a.jpg");
}

TEST_CASE("pickEvenlySpacedFrames: distinct, ordered picks from the 10%-90% window, or from all files when it is too small", "[utils]")
{
	const QDir dir("/f");
	const auto pickedIndices = [&dir](int fileCount, int maxFrames) {
		QStringList files;
		for (int i = 0; i < fileCount; ++i)
			files.push_back(QString::number(i));
		std::vector<int> indices;
		for (const QString& path : pickEvenlySpacedFrames(dir, files, maxFrames))
			indices.push_back(QFileInfo(path).fileName().toInt());
		return indices;
	};

	REQUIRE(pickedIndices(21, 4) == std::vector<int>{ 2, 7, 12, 18 });
	REQUIRE(pickedIndices(21, 1) == std::vector<int>{ 2 });
	REQUIRE(pickedIndices(3, 10) == std::vector<int>{ 0, 1, 2 });
	REQUIRE(pickedIndices(10, 10) == std::vector<int>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }); // the window holds 9
	REQUIRE(pickedIndices(20, 0).empty());
	REQUIRE(pickedIndices(0, 4).empty());

	for (int fileCount = 1; fileCount <= 40; ++fileCount)
	{
		for (int maxFrames = 1; maxFrames <= 10; ++maxFrames)
		{
			const std::vector<int> indices = pickedIndices(fileCount, maxFrames);
			CAPTURE(fileCount, maxFrames);
			REQUIRE(static_cast<int>(indices.size()) == std::min(fileCount, maxFrames));
			REQUIRE(std::ranges::adjacent_find(indices, std::greater_equal<>{}) == indices.end());
		}
	}
}

TEST_CASE("parseTrailingTimestamp: a plausible yyyyMMddHHmmss at the end of the text", "[utils]")
{
	const QDateTime expected{ QDate{ 2024, 3, 15 }, QTime{ 12, 30, 45 } };
	REQUIRE(parseTrailingTimestamp("VID_20240315123045") == expected);
	REQUIRE(parseTrailingTimestamp("VID_20240315123045123") == expected); // sub-second digits
	REQUIRE_FALSE(parseTrailingTimestamp("VID_20241315123045").isValid()); // month 13
	REQUIRE_FALSE(parseTrailingTimestamp("VID_19850315123045").isValid()); // before 1990
	REQUIRE_FALSE(parseTrailingTimestamp("VID_" + QString::number(QDate::currentDate().year() + 2) + "0315123045").isValid());
	REQUIRE_FALSE(parseTrailingTimestamp("VID_20240315123045_edit").isValid());
	REQUIRE_FALSE(parseTrailingTimestamp("VID_2024031512304").isValid()); // 13 digits
	REQUIRE_FALSE(parseTrailingTimestamp("").isValid());
}

TEST_CASE("invalidFilenameChar: the first Windows-illegal character, null when there is none", "[utils]")
{
	REQUIRE(invalidFilenameChar("Summer trip (1).jpg").isNull());
	REQUIRE(invalidFilenameChar("a?b*c") == '?');
	for (const QChar c : QString(R"(\/:*?"<>|)"))
		REQUIRE(invalidFilenameChar(QString("name") + c) == c);
}

TEST_CASE("filesAreIdentical: every byte across chunk boundaries", "[utils]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QDir d(dir.path());

	// Spans several comparison chunks.
	QByteArray content(1536 * 1024 + 7, 'a');
	writeTextFile(d.filePath("a.bin"), content);
	writeTextFile(d.filePath("same.bin"), content);
	REQUIRE(filesAreIdentical(d.filePath("a.bin"), d.filePath("same.bin")));

	content.back() = 'b';
	writeTextFile(d.filePath("lastByte.bin"), content);
	REQUIRE_FALSE(filesAreIdentical(d.filePath("a.bin"), d.filePath("lastByte.bin")));

	content.chop(1);
	writeTextFile(d.filePath("shorter.bin"), content);
	REQUIRE_FALSE(filesAreIdentical(d.filePath("a.bin"), d.filePath("shorter.bin")));
}

TEST_CASE("isSupportedVideoFile / isSupportedImageFile: by suffix, case-insensitively", "[utils]")
{
	REQUIRE(isSupportedVideoFile("C:/v/clip.MP4"));
	REQUIRE(isSupportedImageFile("C:/p/photo.JPEG"));
	REQUIRE(isSupportedImageFile("C:/p/raw.NEF"));
	REQUIRE_FALSE(isSupportedImageFile("C:/v/clip.mp4"));
	REQUIRE_FALSE(isSupportedVideoFile("C:/p/photo.jpg"));
	REQUIRE_FALSE(isSupportedMediaFile("C:/a/archive.mp4.zip"));
	REQUIRE_FALSE(isSupportedMediaFile("C:/a/noext"));
}
