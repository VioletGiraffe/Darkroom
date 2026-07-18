#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "TestHelpers.h"

#include <QFileInfo>
#include <QTemporaryDir>

TEST_CASE("A new empty library seeds Best and persists the registry", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());

	Library library;
	REQUIRE_FALSE(library.isLoaded());
	QString error;
	REQUIRE(library.setRoot(dir.path(), &error));
	REQUIRE(error.isEmpty());
	REQUIRE(library.isLoaded());

	const Catalog& catalog = library.catalog();
	REQUIRE(catalog.mediaItemCount() == 0);
	REQUIRE(catalog.allLabels().size() == 1u);
	REQUIRE(catalog.allLabels().front().id == Catalog::BestLabelId);
	REQUIRE(QFileInfo::exists(library.rootFolder() + "/labels.json"));
}

// The golden backward-compatibility fixture: every record shape catalog.json has ever held. If a schema
// change breaks reading any of these, existing libraries break with it - extend this fixture when the
// schema evolves, never weaken it.
TEST_CASE("Every stored record format loads correctly", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString root = dir.path();

	writeTextFile(root + "/labels.json", QStringLiteral(R"({
		"labels": [ { "id": 1001, "displayName": "Trip", "color": "#112233" } ]
	})").toUtf8());
	// In order: a pre-photos video record (no type/splitIntoFrames/durationMs), a current video record
	// (with a legacy absolute folder), an owned photo, a referenced photo, then records the loader must
	// skip: a folder-less video, a negative-size (source-unavailable) key, a nameless record.
	writeTextFile(root + "/catalog.json", QStringLiteral(R"({
		"1000:clip.mp4": { "name": "Clip.MP4", "folder": "Family/Clip", "sourceVideoPath": "D:/videos/Clip.MP4" },
		"1500:new.mp4": { "name": "New.mp4", "folder": "%1/Family/New", "sourceVideoPath": "D:/videos/New.mp4",
		                  "splitIntoFrames": false, "durationMs": 98765 },
		"2000:photo.jpg": { "name": "Photo.jpg", "folder": "Photos/Trip", "sourceVideoPath": "%1/Photos/Trip/Photo.jpg", "type": "photo" },
		"3000:ref.png": { "name": "Ref.png", "sourceVideoPath": "D:/elsewhere/Ref.png", "type": "photo", "referenced": true, "labels": [1001] },
		"4000:orphan.mp4": { "name": "Orphan.mp4", "labels": [1001] },
		"-1:invalid.mp4": { "name": "invalid.mp4", "folder": "Family/invalid" },
		"5000:nameless.mp4": { "folder": "Family/nameless" }
	})").arg(root).toUtf8());

	Library library;
	QString error;
	REQUIRE(library.setRoot(root, &error));
	const Catalog& catalog = library.catalog();
	const QString rootFolder = library.rootFolder();

	REQUIRE(catalog.mediaItemCount() == 4);

	// Registry: Best pinned first, the persisted label kept as-is, "Family" seeded from the items'
	// storage folders, "Trip" resolved to the persisted entry rather than duplicated.
	REQUIRE(catalog.allLabels().front().id == Catalog::BestLabelId);
	const LabelId trip = labelIdByName(catalog, "Trip");
	REQUIRE(trip == labelIdFromUInt64(1001));
	REQUIRE(catalog.labelById(trip)->color == "#112233");
	const LabelId family = labelIdByName(catalog, "Family");
	REQUIRE(family != LabelId::None);
	REQUIRE(toUInt64(family) >= FirstRealLabelId);
	REQUIRE(catalog.allLabels().size() == 3u);

	// The pre-photos video: absent fields take their documented defaults.
	const MediaId video = MediaId::fromNameAndSize("Clip.MP4", 1000);
	REQUIRE(catalog.containsMediaItem(video));
	REQUIRE(catalog.mediaType(video) == Catalog::MediaType::Video);
	REQUIRE(catalog.isSplitIntoFrames(video));
	REQUIRE(catalog.durationMsForMediaItem(video) == -1);
	REQUIRE_FALSE(catalog.isReferenced(video));
	REQUIRE(catalog.folderForMediaItem(video) == rootFolder + "/Family/Clip");
	REQUIRE(catalog.sourcePathForMediaItem(video) == "D:/videos/Clip.MP4");
	REQUIRE(catalog.labelsForMediaItem(video) == QList<LabelId>{ family });
	REQUIRE(catalog.mediaItemsForLabel(family).contains(video));

	// Identity is case-insensitive: a case-variant spelling addresses the same item.
	REQUIRE(catalog.containsMediaItem(MediaId::fromNameAndSize("CLIP.mp4", 1000)));

	// The current-format video: explicit fields honored; a legacy absolute folder value is tolerated.
	const MediaId newVideo = MediaId::fromNameAndSize("New.mp4", 1500);
	REQUIRE_FALSE(catalog.isSplitIntoFrames(newVideo));
	REQUIRE(catalog.durationMsForMediaItem(newVideo) == 98765);
	REQUIRE(catalog.folderForMediaItem(newVideo) == root + "/Family/New");

	const MediaId photo = MediaId::fromNameAndSize("Photo.jpg", 2000);
	REQUIRE(catalog.mediaType(photo) == Catalog::MediaType::Photo);
	REQUIRE_FALSE(catalog.isReferenced(photo));
	REQUIRE(catalog.folderForMediaItem(photo) == rootFolder + "/Photos/Trip");
	REQUIRE(catalog.labelsForMediaItem(photo) == QList<LabelId>{ trip });

	// A referenced photo has no folder; all its labels are stored ids.
	const MediaId referenced = MediaId::fromNameAndSize("Ref.png", 3000);
	REQUIRE(catalog.mediaType(referenced) == Catalog::MediaType::Photo);
	REQUIRE(catalog.isReferenced(referenced));
	REQUIRE(catalog.folderForMediaItem(referenced).isEmpty());
	REQUIRE(catalog.labelsForMediaItem(referenced) == QList<LabelId>{ trip });

	REQUIRE_FALSE(catalog.containsMediaItem(MediaId::fromNameAndSize("Orphan.mp4", 4000)));
	REQUIRE_FALSE(catalog.containsMediaItem(MediaId::fromNameAndSize("invalid.mp4", -1)));
	REQUIRE_FALSE(catalog.containsMediaItem(MediaId::fromNameAndSize("nameless.mp4", 5000)));
}

TEST_CASE("An API-built library round-trips through disk unchanged", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());

	QString firstDump;
	{
		Library library;
		REQUIRE(library.setRoot(dir.path()));
		Catalog& catalog = library.catalog();
		const QString root = library.rootFolder();

		const MediaId video = MediaId::fromNameAndSize("Clip.mp4", 1000);
		REQUIRE(catalog.addMediaItem(video, "D:/videos/Clip.mp4", root + "/Alpha/Clip", false, 5000));
		QString error;
		const LabelId beta = catalog.createLabel("Beta", "#334455", &error);
		REQUIRE(beta != LabelId::None);
		catalog.addLabel(video, beta);
		catalog.addLabel(video, Catalog::BestLabelId);
		catalog.markSplitComplete(video);
		catalog.setDurationMs(video, 6000);

		const MediaId photo = MediaId::fromNameAndSize("Photo.jpg", 2000);
		REQUIRE(catalog.addPhoto(photo, root + "/Photos/Beta/Photo.jpg", root + "/Photos/Beta", false));

		const MediaId referenced = MediaId::fromNameAndSize("Ref.png", 3000);
		REQUIRE(catalog.addPhoto(referenced, "D:/elsewhere/Ref.png", {}, true));
		catalog.addLabel(referenced, beta);

		requireRebuildStable(catalog);
		firstDump = dumpCatalog(catalog);
		REQUIRE(library.flushPendingWrites());
	}

	Library reloaded;
	REQUIRE(reloaded.setRoot(dir.path()));
	REQUIRE(dumpCatalog(reloaded.catalog()) == firstDump);
}

TEST_CASE("setRoot on a fresh Library fails cleanly on corrupt data", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	writeTextFile(dir.path() + "/catalog.json", "{ not json");

	Library library;
	QString error;
	REQUIRE_FALSE(library.setRoot(dir.path(), &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE_FALSE(library.isLoaded());
}

TEST_CASE("A failed setRoot leaves the current library untouched", "[persistence]")
{
	QTemporaryDir goodDir, badDir;
	REQUIRE(goodDir.isValid());
	REQUIRE(badDir.isValid());

	Library library;
	REQUIRE(library.setRoot(goodDir.path()));
	const QString goodRoot = library.rootFolder();
	const uint64_t generation = library.generation();

	SECTION("corrupt catalog.json")
	{
		writeTextFile(badDir.path() + "/catalog.json", "{ not json");
	}
	SECTION("catalog.json with a non-object root")
	{
		writeTextFile(badDir.path() + "/catalog.json", "[1, 2, 3]");
	}
	SECTION("labels.json without a 'labels' array")
	{
		writeTextFile(badDir.path() + "/labels.json", R"({ "labels": 42 })");
	}
	SECTION("labels.json with a duplicate label id")
	{
		writeTextFile(badDir.path() + "/labels.json",
			R"({ "labels": [ { "id": 1001, "displayName": "A" }, { "id": 1001, "displayName": "B" } ] })");
	}
	SECTION("labels.json with a non-integer label id")
	{
		writeTextFile(badDir.path() + "/labels.json", R"({ "labels": [ { "id": 1001.5, "displayName": "A" } ] })");
	}

	QString error;
	REQUIRE_FALSE(library.setRoot(badDir.path(), &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE(library.isLoaded());
	REQUIRE(library.rootFolder() == goodRoot);
	REQUIRE(library.generation() == generation);
}
