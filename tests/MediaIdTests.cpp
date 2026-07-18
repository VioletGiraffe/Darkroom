#include "Core/MediaId.h"
#include "TestHelpers.h"

#include <QTemporaryDir>

TEST_CASE("MediaId: identity semantics", "[mediaid]")
{
	SECTION("default-constructed is invalid")
	{
		REQUIRE_FALSE(MediaId{}.isValid());
	}

	SECTION("fromNameAndSize round-trips name and size; key() folds case")
	{
		const MediaId id = MediaId::fromNameAndSize("MyClip.MP4", 12345);
		REQUIRE(id.isValid());
		REQUIRE(id.name() == "MyClip.MP4");
		REQUIRE(id.size() == 12345);
		// The persisted catalog key - changing this format orphans every existing record.
		REQUIRE(id.key() == "12345:myclip.mp4");
	}

	SECTION("equality and hash are case-insensitive and agree with each other")
	{
		const MediaId a = MediaId::fromNameAndSize("Clip.mp4", 100);
		const MediaId b = MediaId::fromNameAndSize("CLIP.MP4", 100);
		REQUIRE(a == b);
		REQUIRE(qHash(a) == qHash(b));
		REQUIRE(qHash(a, 7) == qHash(b, 7));
		REQUIRE(a.key() == b.key());

		REQUIRE_FALSE(a == MediaId::fromNameAndSize("Clip.mp4", 101));
		REQUIRE_FALSE(a == MediaId::fromNameAndSize("Other.mp4", 100));
	}

	SECTION("fromFile: a real file yields its name and size, anything else is invalid")
	{
		QTemporaryDir dir;
		REQUIRE(dir.isValid());
		const QString path = dir.path() + "/Frame.PNG";
		writeTextFile(path, "0123456789");

		const MediaId id = MediaId::fromFile(path);
		REQUIRE(id.isValid());
		REQUIRE(id.name() == "Frame.PNG");
		REQUIRE(id.size() == 10);

		REQUIRE_FALSE(MediaId::fromFile(dir.path() + "/missing.png").isValid());
		REQUIRE_FALSE(MediaId::fromFile(dir.path()).isValid());  // a directory is not a file
	}
}
