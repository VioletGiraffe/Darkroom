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
	// platform - not our logic to pin. What is ours (and what a plain QDir name-filter would silently lose
	// on the case-sensitive Linux CI leg) is that ".JPG"/".PNG"/".TIFF" are matched alongside their
	// lowercase spellings while the non-image entries are dropped.
	QStringList files = listFrameImageFiles(d);
	files.sort();
	REQUIRE(files == QStringList({ "B.JPG", "a.jpg", "c.jpeg", "d.PNG", "e.tif", "f.TIFF" }));
}
