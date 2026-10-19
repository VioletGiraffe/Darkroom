#include "Utils.h"
#include "TestHelpers.h"

#include <QDir>
#include <QTemporaryDir>

TEST_CASE("listFrameImageFiles: matches every image suffix case-insensitively, excludes the rest", "[utils]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QDir d(dir.path());

	for (const QString& name : { "a.jpg", "B.JPG", "c.jpeg", "d.PNG", "e.tif", "f.TIFF" })
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
	REQUIRE(isPathAtOrUnder("c:\\lib\\photos\\cats\\a.gif", roots));
	REQUIRE_FALSE(isPathAtOrUnder("C:/Lib/Photos/Catsup/a.gif", roots)); // a sibling sharing the name prefix
	REQUIRE_FALSE(isPathAtOrUnder("C:/Lib/Photos", roots));
	REQUIRE_FALSE(isPathAtOrUnder("", roots));
	REQUIRE_FALSE(isPathAtOrUnder("/x", { "" }));
}

TEST_CASE("pathAfterMoves: rebases the moved path and its descendants, keeping the descendants' spelling", "[utils]")
{
	const std::vector<PathMove> moves{ { "C:/Lib/Photos/Cats", "C:/Lib/Photos/Kittens" }, { "C:/v/clip.mp4", "C:/v/renamed.mp4" } };
	REQUIRE(pathAfterMoves("C:/Lib/Photos/Cats", moves) == "C:/Lib/Photos/Kittens");
	REQUIRE(pathAfterMoves("c:/lib/photos/cats/Sub/A.gif", moves) == "C:/Lib/Photos/Kittens/Sub/A.gif");
	REQUIRE(pathAfterMoves("C:\\v\\clip.mp4", moves) == "C:/v/renamed.mp4");
	REQUIRE(pathAfterMoves("C:/Lib/Photos/Catsup/a.gif", moves) == "C:/Lib/Photos/Catsup/a.gif");
	REQUIRE(pathAfterMoves("", moves).isEmpty());
}
