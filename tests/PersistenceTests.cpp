#include "Core/Catalog.h"
#include "Core/JsonPersistence.h"
#include "Core/Library.h"
#include "Core/MediaItemId.h"
#include "Core/MetadataStore.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

namespace
{
	[[nodiscard]] QByteArray readFile(const QString& path)
	{
		QFile file(path);
		REQUIRE(file.open(QIODevice::ReadOnly));
		return file.readAll();
	}
}

TEST_CASE("MediaItemId: the persisted string form", "[persistence]")
{
	const MediaItemId id = static_cast<MediaItemId>(0x1234abcdULL);
	REQUIRE(toString(id) == "000000001234abcd");
	REQUIRE(mediaItemIdFromString(toString(id)) == id);
	REQUIRE(mediaItemIdFromString(QStringLiteral("ffffffffffffffff")) == static_cast<MediaItemId>(~0ULL));

	REQUIRE(mediaItemIdFromString(QStringLiteral("1234abcd")) == MediaItemId::None);          // not 16 digits
	REQUIRE(mediaItemIdFromString(QStringLiteral("000000001234abcg")) == MediaItemId::None);  // not hex
	REQUIRE(mediaItemIdFromString(QString()) == MediaItemId::None);
}

TEST_CASE("A new empty library seeds Best and persists the registry", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());

	Library library;
	REQUIRE_FALSE(library.isLoaded());
	QString error;
	REQUIRE(library.setRoot(dir.path(), Library::OpenMode::CreateIfAbsent, &error));
	REQUIRE(error.isEmpty());
	REQUIRE(library.isLoaded());

	const Catalog& catalog = library.catalog();
	REQUIRE(catalog.mediaItemCount() == 0);
	REQUIRE(catalog.allLabels().size() == 1u);
	REQUIRE(catalog.allLabels().front().id == Catalog::BestLabelId);
	REQUIRE(QFileInfo::exists(library.rootFolder() + "/labels.json"));
}

// The golden backward-compatibility fixture for the first catalog.json format: every record shape it ever held.
// Loading converts it to the current format. Never weaken this fixture.
TEST_CASE("Every first-format record converts and loads correctly", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString root = dir.path();

	writeTextFile(root + "/labels.json", QStringLiteral(R"({
		"labels": [ { "id": 1001, "displayName": "Trip", "color": "#112233" } ]
	})").toUtf8());
	// In order: a pre-photos video record (no type/splitIntoFrames/durationMs), a newer video record
	// (with a legacy absolute folder), an owned photo with a legacy absolute source, a newer owned photo,
	// a referenced photo, then records that were never items: a folder-less video, a negative-size
	// (source-unavailable) key, a nameless record.
	const QByteArray firstFormat = QStringLiteral(R"({
		"1000:clip.mp4": { "name": "Clip.MP4", "folder": "Family/Clip", "sourceVideoPath": "%2/Clip.MP4" },
		"1500:new.mp4": { "name": "New.mp4", "folder": "%1/Family/New", "sourceVideoPath": "%2/New.mp4",
		                  "splitIntoFrames": false, "durationMs": 98765 },
		"2000:photo.jpg": { "name": "Photo.jpg", "folder": "Photos/Trip", "sourceVideoPath": "%1/Photos/Trip/Photo.jpg", "type": "photo" },
		"2500:current.jpg": { "name": "Current.jpg", "folder": "Photos/Trip", "sourceVideoPath": "Photos/Trip/Current.jpg", "type": "photo" },
		"3000:ref.png": { "name": "Ref.png", "sourceVideoPath": "%3/Ref.png", "type": "photo", "referenced": true, "labels": [1001] },
		"4000:orphan.mp4": { "name": "Orphan.mp4", "labels": [1001] },
		"-1:invalid.mp4": { "name": "invalid.mp4", "folder": "Family/invalid" },
		"5000:nameless.mp4": { "folder": "Family/nameless" }
	})").arg(root, outsideLibraryPath("videos"), outsideLibraryPath("elsewhere")).toUtf8();
	writeTextFile(root + "/catalog.json", firstFormat);

	Library library;
	QString error;
	REQUIRE(library.setRoot(root, Library::OpenMode::ExistingOnly, &error));
	const Catalog& catalog = library.catalog();
	const QString rootFolder = library.rootFolder();

	REQUIRE(catalog.mediaItemCount() == 5);

	// The original is kept beside the converted file, which is saved on load.
	REQUIRE(readFile(root + "/catalog.v1-backup.json") == firstFormat);
	const JsonPersistence::ObjectReadResult converted = JsonPersistence::readObject(root + "/catalog.json");
	REQUIRE(converted.success);
	REQUIRE(converted.object.value(MetadataStore::VersionField).toInteger() == MetadataStore::FormatVersion);
	REQUIRE(converted.object.value(MetadataStore::ItemsField).toObject().size() == 5);

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

	// The pre-photos video: absent fields take their documented defaults; the size comes from the key.
	const MediaItemId video = catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.MP4"));
	REQUIRE(video != MediaItemId::None);
	REQUIRE(catalog.mediaType(video) == Catalog::MediaType::Video);
	REQUIRE(catalog.isSplitIntoFrames(video));
	REQUIRE(catalog.durationMsForMediaItem(video) == -1);
	REQUIRE(catalog.mediaItems().value(video).sourceSize == 1000);
	REQUIRE_FALSE(catalog.isReferenced(video));
	REQUIRE(catalog.folderForMediaItem(video) == rootFolder + "/Family/Clip");
	REQUIRE(catalog.labelsForMediaItem(video) == QList<LabelId>{ family });
	REQUIRE(catalog.mediaItemsForLabel(family).contains(video));

	// The newer video record: explicit fields honored; a legacy absolute folder value is tolerated.
	const MediaItemId newVideo = catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/New.mp4"));
	REQUIRE_FALSE(catalog.isSplitIntoFrames(newVideo));
	REQUIRE(catalog.durationMsForMediaItem(newVideo) == 98765);
	REQUIRE(catalog.folderForMediaItem(newVideo) == root + "/Family/New");

	const MediaItemId photo = catalog.mediaItemAtSourcePath(root + "/Photos/Trip/Photo.jpg");
	REQUIRE(catalog.mediaType(photo) == Catalog::MediaType::Photo);
	REQUIRE_FALSE(catalog.isReferenced(photo));
	REQUIRE(catalog.folderForMediaItem(photo) == rootFolder + "/Photos/Trip");
	REQUIRE(catalog.labelsForMediaItem(photo) == QList<LabelId>{ trip });

	// The newer photo record: its source is root-relative.
	const MediaItemId currentPhoto = catalog.mediaItemAtSourcePath(rootFolder + "/Photos/Trip/Current.jpg");
	REQUIRE(catalog.folderForMediaItem(currentPhoto) == rootFolder + "/Photos/Trip");

	// A referenced photo has no folder; all its labels are stored ids.
	const MediaItemId referenced = catalog.mediaItemAtSourcePath(outsideLibraryPath("elsewhere/Ref.png"));
	REQUIRE(catalog.mediaType(referenced) == Catalog::MediaType::Photo);
	REQUIRE(catalog.isReferenced(referenced));
	REQUIRE(catalog.folderForMediaItem(referenced).isEmpty());
	REQUIRE(catalog.labelsForMediaItem(referenced) == QList<LabelId>{ trip });

	REQUIRE(QSet<MediaItemId>{ video, newVideo, photo, currentPhoto, referenced }.size() == 5);

	// The converted library reloads to the same state, ids included.
	Library reloaded;
	REQUIRE(reloaded.setRoot(root, Library::OpenMode::ExistingOnly));
	REQUIRE(dumpCatalog(reloaded.catalog()) == dumpCatalog(catalog));
}

// The golden fixture for the current format.
TEST_CASE("Current-format records load under their stored ids", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString root = dir.path();

	writeTextFile(root + "/catalog.json", QStringLiteral(R"({
		"version": 2,
		"items": {
			"00000000000004d2": { "folder": "Family/Clip", "sourceVideoPath": "%1/Clip.mp4", "sourceSize": 1000,
			                      "splitIntoFrames": false, "durationMs": 5000 },
			"fedcba9876543210": { "folder": "Photos/Trip", "sourceVideoPath": "Photos/Trip/Photo.jpg", "type": "photo" }
		}
	})").arg(outsideLibraryPath("videos")).toUtf8());

	Library library;
	REQUIRE(library.setRoot(root, Library::OpenMode::ExistingOnly));
	const Catalog& catalog = library.catalog();
	const QString rootFolder = library.rootFolder();
	REQUIRE(catalog.mediaItemCount() == 2);
	REQUIRE_FALSE(QFileInfo::exists(root + "/catalog.v1-backup.json"));

	const MediaItemId video = mediaItemIdFromString(QStringLiteral("00000000000004d2"));
	REQUIRE(catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.mp4")) == video);
	REQUIRE(catalog.folderForMediaItem(video) == rootFolder + "/Family/Clip");
	REQUIRE(catalog.mediaItems().value(video).sourceSize == 1000);
	REQUIRE_FALSE(catalog.isSplitIntoFrames(video));
	REQUIRE(catalog.durationMsForMediaItem(video) == 5000);

	const MediaItemId photo = mediaItemIdFromString(QStringLiteral("fedcba9876543210"));
	REQUIRE(catalog.mediaType(photo) == Catalog::MediaType::Photo);
	REQUIRE(catalog.sourcePathForMediaItem(photo) == rootFolder + "/Photos/Trip/Photo.jpg");
	REQUIRE(catalog.mediaItems().value(photo).sourceSize == -1);  // none recorded
}

TEST_CASE("An API-built library round-trips through disk unchanged", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());

	QString firstDump;
	{
		Library library;
		REQUIRE(library.setRoot(dir.path(), Library::OpenMode::CreateIfAbsent));
		Catalog& catalog = library.catalog();
		const QString root = library.rootFolder();

		const MediaItemId video = catalog.addMediaItem(outsideLibraryPath("videos/Clip.mp4"), root + "/Alpha/Clip", false, 5000);
		REQUIRE(video != MediaItemId::None);
		QString error;
		const LabelId beta = catalog.createLabel("Beta", "#334455", &error);
		REQUIRE(beta != LabelId::None);
		catalog.addLabel(video, beta);
		catalog.addLabel(video, Catalog::BestLabelId);
		catalog.markSplitComplete(video);
		catalog.setDurationMs(video, 6000);

		const QString photoPath = root + "/Photos/Beta/Photo.jpg";
		REQUIRE(QDir{}.mkpath(root + "/Photos/Beta"));
		writeTextFile(photoPath, "photo");
		REQUIRE(catalog.addPhoto(photoPath, root + "/Photos/Beta", false) != MediaItemId::None);

		const MediaItemId referenced = catalog.addPhoto(outsideLibraryPath("elsewhere/Ref.png"), {}, true);
		REQUIRE(referenced != MediaItemId::None);
		catalog.addLabel(referenced, beta);

		requireRebuildStable(catalog);
		firstDump = dumpCatalog(catalog);
		REQUIRE(library.flushPendingWrites());
	}

	Library reloaded;
	REQUIRE(reloaded.setRoot(dir.path(), Library::OpenMode::ExistingOnly));
	REQUIRE(dumpCatalog(reloaded.catalog()) == firstDump);
}

TEST_CASE("A library opens intact after its folder moves", "[persistence]")
{
	QTemporaryDir original, moved;
	REQUIRE(original.isValid());
	REQUIRE(moved.isValid());

	// A first-format record holding absolute paths under the root; loading must convert it.
	writeTextFile(original.path() + "/catalog.json", QStringLiteral(R"({
		"2000:old.jpg": { "name": "Old.jpg", "folder": "%1/Photos/Trip", "sourceVideoPath": "%1/Photos/Trip/Old.jpg", "type": "photo" }
	})").arg(original.path()).toUtf8());

	{
		Library library;
		REQUIRE(library.setRoot(original.path(), Library::OpenMode::ExistingOnly));
		Catalog& catalog = library.catalog();
		const QString root = library.rootFolder();
		REQUIRE(catalog.addPhoto(root + "/Photos/Trip/New.jpg", root + "/Photos/Trip", false) != MediaItemId::None);
		REQUIRE(catalog.addMediaItem(outsideLibraryPath("videos/Clip.mp4"), root + "/Family/Clip", true) != MediaItemId::None);
		REQUIRE(library.flushPendingWrites());
	}
	for (const char* file : { "catalog.json", "labels.json" })
		REQUIRE(QFile::copy(original.path() + "/" + file, moved.path() + "/" + file));

	Library library;
	REQUIRE(library.setRoot(moved.path(), Library::OpenMode::ExistingOnly));
	const Catalog& catalog = library.catalog();
	const QString root = library.rootFolder();
	const MediaItemId oldPhoto = catalog.mediaItemAtSourcePath(root + "/Photos/Trip/Old.jpg");
	REQUIRE(oldPhoto != MediaItemId::None);
	REQUIRE(catalog.folderForMediaItem(oldPhoto) == root + "/Photos/Trip");
	REQUIRE(catalog.mediaItemAtSourcePath(root + "/Photos/Trip/New.jpg") != MediaItemId::None);
	const MediaItemId video = catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.mp4"));  // outside the root, so stored as-is
	REQUIRE(catalog.folderForMediaItem(video) == root + "/Family/Clip");
}

TEST_CASE("A legacy library moved before its first load rebases its paths onto the new root", "[persistence]")
{
	QTemporaryDir moved;
	REQUIRE(moved.isValid());
	const QString previousRoot = outsideLibraryPath("old-library");

	// Absolute paths under a root the library no longer lives at; sources outside it must stay as they are.
	writeTextFile(moved.path() + "/catalog.json", QStringLiteral(R"({
		"1000:clip.mp4": { "name": "Clip.mp4", "folder": "%1/Family/Clip", "sourceVideoPath": "%2/Clip.mp4", "splitIntoFrames": false },
		"1500:inside.mp4": { "name": "Inside.mp4", "folder": "%1/Family/Inside", "sourceVideoPath": "%1/Sources/Inside.mp4" },
		"2000:old.jpg": { "name": "Old.jpg", "folder": "%1/Photos/Trip", "sourceVideoPath": "%1/Photos/Trip/Old.jpg", "type": "photo" },
		"3000:ref.png": { "name": "Ref.png", "sourceVideoPath": "%3/Ref.png", "type": "photo", "referenced": true }
	})").arg(previousRoot, outsideLibraryPath("videos"), outsideLibraryPath("elsewhere")).toUtf8());

	Library library;
	REQUIRE(library.setRoot(moved.path(), Library::OpenMode::ExistingOnly));
	const Catalog& catalog = library.catalog();
	const QString root = library.rootFolder();

	const MediaItemId video = catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.mp4"));
	REQUIRE(catalog.folderForMediaItem(video) == root + "/Family/Clip");
	REQUIRE(catalog.labelsForMediaItem(video) == QList<LabelId>{ labelIdByName(catalog, "Family") });

	const MediaItemId inside = catalog.mediaItemAtSourcePath(root + "/Sources/Inside.mp4");
	REQUIRE(catalog.folderForMediaItem(inside) == root + "/Family/Inside");

	const MediaItemId photo = catalog.mediaItemAtSourcePath(root + "/Photos/Trip/Old.jpg");
	REQUIRE(catalog.folderForMediaItem(photo) == root + "/Photos/Trip");

	REQUIRE(catalog.mediaItemAtSourcePath(outsideLibraryPath("elsewhere/Ref.png")) != MediaItemId::None);
	REQUIRE(catalog.mediaItemCount() == 4);

	// The rebased paths are persisted root-relative.
	REQUIRE(library.flushPendingWrites());
	REQUIRE_FALSE(readFile(moved.path() + "/catalog.json").contains(previousRoot.toUtf8()));
}

TEST_CASE("setRoot on a fresh Library fails cleanly on corrupt data", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	writeTextFile(dir.path() + "/catalog.json", "{ not json");

	Library library;
	QString error;
	REQUIRE_FALSE(library.setRoot(dir.path(), Library::OpenMode::CreateIfAbsent, &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE_FALSE(library.isLoaded());
}

TEST_CASE("setRoot(ExistingOnly) opens only a folder that holds a library", "[persistence]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString missingRoot = dir.path() + "/missing";

	Library library;
	QString error;
	REQUIRE_FALSE(library.setRoot(missingRoot, Library::OpenMode::ExistingOnly, &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE_FALSE(QFileInfo::exists(missingRoot));

	REQUIRE_FALSE(library.setRoot(dir.path(), Library::OpenMode::ExistingOnly, &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE_FALSE(Library::holdsLibrary(dir.path()));
	REQUIRE_FALSE(library.isLoaded());

	{
		Library creator;
		REQUIRE(creator.setRoot(dir.path(), Library::OpenMode::CreateIfAbsent));
	}
	REQUIRE(library.setRoot(dir.path(), Library::OpenMode::ExistingOnly, &error));
	REQUIRE(error.isEmpty());
}

TEST_CASE("A failed setRoot leaves the current library untouched", "[persistence]")
{
	QTemporaryDir goodDir, badDir;
	REQUIRE(goodDir.isValid());
	REQUIRE(badDir.isValid());

	Library library;
	REQUIRE(library.setRoot(goodDir.path(), Library::OpenMode::CreateIfAbsent));
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
	SECTION("catalog.json in an unknown format version")
	{
		writeTextFile(badDir.path() + "/catalog.json", R"({ "version": 3, "items": {} })");
	}
	SECTION("catalog.json whose items are not an object")
	{
		writeTextFile(badDir.path() + "/catalog.json", R"({ "version": 2, "items": [] })");
	}
	SECTION("catalog.json with an invalid item id")
	{
		writeTextFile(badDir.path() + "/catalog.json", R"({ "version": 2, "items": { "1000:clip.mp4": {} } })");
	}
	SECTION("catalog.json with a non-object item record")
	{
		writeTextFile(badDir.path() + "/catalog.json", R"({ "version": 2, "items": { "00000000000004d2": 42 } })");
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
	REQUIRE_FALSE(library.setRoot(badDir.path(), Library::OpenMode::CreateIfAbsent, &error));
	REQUIRE_FALSE(error.isEmpty());
	REQUIRE(library.isLoaded());
	REQUIRE(library.rootFolder() == goodRoot);
	REQUIRE(library.generation() == generation);
}
