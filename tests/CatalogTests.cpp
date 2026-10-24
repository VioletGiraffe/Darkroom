#include "Core/Catalog.h"
#include "Core/Library.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <vector>

namespace
{
	// A loaded library in a temp root with two folder-backed labels (Alpha, Beta) ready for items.
	struct LibraryFixture
	{
		LibraryFixture()
		{
			REQUIRE(tempDir.isValid());
			REQUIRE(library.setRoot(tempDir.path()));
			root = library.rootFolder();
			Catalog& catalog = library.catalog();
			QString error;
			alpha = catalog.createLabel("Alpha", {}, &error);
			REQUIRE(alpha != LabelId::None);
			beta = catalog.createLabel("Beta", {}, &error);
			REQUIRE(beta != LabelId::None);
		}

		// Registers a video whose frame folder physically exists under the given label's storage folder,
		// so the disk-touching paths (relocation, label rename) can operate on it. The source does not exist.
		MediaItemId addVideo(const QString& name, const QString& labelName, const QString& sourceFolder = "videos")
		{
			const QString folder = root + '/' + labelName + '/' + QFileInfo(name).completeBaseName();
			REQUIRE(QDir{}.mkpath(folder));
			const MediaItemId id = library.catalog().addMediaItem(outsideLibraryPath(sourceFolder + '/') + name, folder, true);
			REQUIRE(id != MediaItemId::None);
			return id;
		}

		QTemporaryDir tempDir;
		Library library;
		QString root;
		LabelId alpha = LabelId::None;
		LabelId beta = LabelId::None;
	};
}

TEST_CASE("frameFolderName: derived from the source's name, extension and size", "[catalog]")
{
	// Names folders on disk: these values pin the scheme
	CHECK(Catalog::frameFolderName("clip", "mp4", 734003200) == "clip_mp4_734003200");
	CHECK(Catalog::frameFolderName("clip", "", 5) == "clip__5");
	CHECK(Catalog::frameFolderName("", "mp4", 5) == "_mp4_5");

	// Sources differing in extension, size, or only where an underscore replaces the dot
	CHECK(Catalog::frameFolderName("clip", "mp4", 5) != Catalog::frameFolderName("clip", "mov", 5));
	CHECK(Catalog::frameFolderName("clip", "mp4", 5) != Catalog::frameFolderName("clip", "mp4", 6));
	CHECK(Catalog::frameFolderName("a_b", "", 5) != Catalog::frameFolderName("a", "b", 5));
}

TEST_CASE("Library forwards coalesced Catalog change notifications", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();
	int notificationCount = 0;
	QObject::connect(&f.library, &Library::catalogChanged, [&notificationCount] { ++notificationCount; });

	catalog.setColor(f.alpha, "#123456");
	REQUIRE(notificationCount == 1);
	catalog.setColor(f.alpha, "#123456");
	REQUIRE(notificationCount == 1);

	MediaItemId video = MediaItemId::None;
	{
		Catalog::ChangeBatchScope changes(catalog);
		catalog.setColor(f.beta, "#654321");
		video = f.addVideo("Clip.mp4", "Alpha");
		catalog.addLabel(video, f.beta);
	}
	REQUIRE(notificationCount == 2);
	{
		Catalog::BatchScope batch(catalog);
		catalog.setDurationMs(video, 5000);
		catalog.addLabel(video, Catalog::BestLabelId);
	}
	REQUIRE(notificationCount == 3);

	QTemporaryDir replacement;
	REQUIRE(replacement.isValid());
	REQUIRE(f.library.setRoot(replacement.path()));
	REQUIRE(notificationCount == 4);
}

TEST_CASE("addMediaItem: a new id per item; a tracked source or frame folder is refused", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId id = f.addVideo("Clip.mp4", "Alpha");
	REQUIRE(catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.mp4")) == id);
	REQUIRE(catalog.videoAtFrameFolder(f.root + "/Alpha/Clip") == id);

	// The same source, under any spelling, is refused; the existing entry is untouched.
	REQUIRE(catalog.addMediaItem(outsideLibraryPath("videos/Clip.mp4"), f.root + "/Beta/Clip", true) == MediaItemId::None);
	REQUIRE(catalog.addMediaItem(outsideLibraryPath("VIDEOS/CLIP.MP4"), f.root + "/Beta/CLIP", true) == MediaItemId::None);
	REQUIRE(catalog.folderForMediaItem(id) == f.root + "/Alpha/Clip");
	REQUIRE(catalog.mediaItemCount() == 1);

	// So is a second source for the same frame folder.
	REQUIRE(catalog.addMediaItem(outsideLibraryPath("other/Clip.mp4"), f.root + "/Alpha/Clip", true) == MediaItemId::None);

	// Another source with the same name is a different item with its own id.
	const MediaItemId other = f.addVideo("Clip.mp4", "Beta", "other");
	REQUIRE(other != id);
	REQUIRE(catalog.mediaItemCount() == 2);

	// A missing source records no size; a non-positive duration records none.
	REQUIRE(catalog.mediaItems().value(id).sourceSize == -1);
	REQUIRE(catalog.durationMsForMediaItem(id) == -1);

	requireRebuildStable(catalog);
}

TEST_CASE("addMediaItem and addPhoto record the source's size", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photoPath = f.root + "/Ref.png";
	writeTextFile(photoPath, "0123456789");
	const MediaItemId photo = catalog.addPhoto(photoPath, {}, true);
	REQUIRE(catalog.mediaItems().value(photo).sourceSize == 10);

	const QString videoPath = f.root + "/clip.mp4";
	writeTextFile(videoPath, "0123");
	REQUIRE(QDir{}.mkpath(f.root + "/Alpha/clip"));
	const MediaItemId video = catalog.addMediaItem(videoPath, f.root + "/Alpha/clip", true, 5000);
	REQUIRE(catalog.mediaItems().value(video).sourceSize == 4);
	REQUIRE(catalog.durationMsForMediaItem(video) == 5000);

	requireRebuildStable(catalog);
}

TEST_CASE("frameFolderForNewVideo: numbered when another video owns the folder", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	// Two different files with the same name and size.
	const QString first = f.root + "/a/Clip.mp4";
	const QString second = f.root + "/b/Clip.mp4";
	REQUIRE(QDir{}.mkpath(f.root + "/a"));
	REQUIRE(QDir{}.mkpath(f.root + "/b"));
	writeTextFile(first, "1111");
	writeTextFile(second, "2222");

	const QString storage = f.root + "/Alpha";
	const QString firstFolder = catalog.frameFolderForNewVideo(storage, first);
	REQUIRE(firstFolder == storage + "/Clip_mp4_4");
	REQUIRE(catalog.addMediaItem(first, firstFolder, false) != MediaItemId::None);

	REQUIRE(catalog.frameFolderForNewVideo(storage, second) == storage + "/Clip_mp4_4_2");
}

TEST_CASE("removeLabel: storage relocation and the last-ordinary-label invariant", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId video = f.addVideo("Clip.mp4", "Alpha");
	catalog.addLabel(video, f.beta);
	catalog.addLabel(video, Catalog::BestLabelId);

	// Removing the storage label relocates the frame folder to the remaining ordinary label.
	catalog.removeLabel(video, f.alpha);
	REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Beta/Clip");
	REQUIRE(catalog.videoAtFrameFolder(f.root + "/Beta/Clip") == video);
	REQUIRE(catalog.videoAtFrameFolder(f.root + "/Alpha/Clip") == MediaItemId::None);
	REQUIRE(QDir(f.root + "/Beta/Clip").exists());
	REQUIRE_FALSE(QDir(f.root + "/Alpha/Clip").exists());
	REQUIRE_FALSE(catalog.mediaItemHasLabel(video, f.alpha));
	REQUIRE(catalog.mediaItemHasLabel(video, f.beta));
	REQUIRE(catalog.mediaItemHasLabel(video, Catalog::BestLabelId));
	requireRebuildStable(catalog);

	// Beta is now the last ordinary label: removing it is refused (virtual Best does not count).
	catalog.removeLabel(video, f.beta);
	REQUIRE(catalog.mediaItemHasLabel(video, f.beta));
	REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Beta/Clip");

	// Best itself is removable freely.
	catalog.removeLabel(video, Catalog::BestLabelId);
	REQUIRE_FALSE(catalog.mediaItemHasLabel(video, Catalog::BestLabelId));
	requireRebuildStable(catalog);
}

TEST_CASE("removeLabel: a relocated owned photo is found at its new path", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photosAlpha = f.root + "/Photos/Alpha";
	REQUIRE(QDir{}.mkpath(photosAlpha));
	const QString oldPath = photosAlpha + "/pic.png";
	writeTextFile(oldPath, "x");
	const MediaItemId photo = catalog.addPhoto(oldPath, photosAlpha, false);
	catalog.addLabel(photo, f.beta);

	catalog.removeLabel(photo, f.alpha);
	const QString newPath = f.root + "/Photos/Beta/pic.png";
	REQUIRE(catalog.sourcePathForMediaItem(photo) == newPath);
	REQUIRE(catalog.mediaItemAtSourcePath(newPath) == photo);
	REQUIRE(catalog.mediaItemAtSourcePath(oldPath) == MediaItemId::None);
	requireRebuildStable(catalog);
}

TEST_CASE("A referenced photo keeps its last ordinary label", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photoPath = f.root + "/Ref.png";
	writeTextFile(photoPath, "0123456789");
	const MediaItemId photo = catalog.addPhoto(photoPath, {}, true);
	REQUIRE(photo != MediaItemId::None);
	catalog.addLabel(photo, f.alpha);

	catalog.removeLabel(photo, f.alpha);  // the last ordinary label - refused
	REQUIRE(catalog.mediaItemHasLabel(photo, f.alpha));

	catalog.addLabel(photo, f.beta);
	catalog.removeLabel(photo, f.alpha);  // now another one remains - allowed
	REQUIRE_FALSE(catalog.mediaItemHasLabel(photo, f.alpha));
	REQUIRE(catalog.mediaItemHasLabel(photo, f.beta));
	REQUIRE(QFileInfo::exists(photoPath));  // referenced: label operations never touch the file
	requireRebuildStable(catalog);
}

TEST_CASE("updateMediaItemPaths: the id and metadata stay, another item's source is refused", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId id = f.addVideo("Clip.mp4", "Alpha");
	catalog.addLabel(id, Catalog::BestLabelId);
	catalog.setDurationMs(id, 5000);
	const MediaItemId other = f.addVideo("Other.mp4", "Alpha");

	// Moving onto another tracked item's source is refused, both entries untouched.
	REQUIRE_FALSE(catalog.updateMediaItemPaths(id, outsideLibraryPath("videos/Other.mp4"), f.root + "/Alpha/Other"));
	REQUIRE(catalog.folderForMediaItem(id) == f.root + "/Alpha/Clip");
	REQUIRE(catalog.folderForMediaItem(other) == f.root + "/Alpha/Other");

	// A rename: new paths under the same id; labels and duration stay.
	REQUIRE(QFile::rename(f.root + "/Alpha/Clip", f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.updateMediaItemPaths(id, outsideLibraryPath("videos/Renamed.mp4"), f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.durationMsForMediaItem(id) == 5000);
	REQUIRE(catalog.mediaItemHasLabel(id, Catalog::BestLabelId));
	REQUIRE(catalog.folderForMediaItem(id) == f.root + "/Alpha/Renamed");
	REQUIRE(catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Renamed.mp4")) == id);
	REQUIRE(catalog.mediaItemAtSourcePath(outsideLibraryPath("videos/Clip.mp4")) == MediaItemId::None);
	REQUIRE(catalog.videoAtFrameFolder(f.root + "/Alpha/Renamed") == id);
	requireRebuildStable(catalog);

	// A located source that exists has its size recorded; a missing one keeps the recorded size.
	const QString located = f.root + "/Located.mp4";
	writeTextFile(located, "012345");
	REQUIRE(catalog.updateMediaItemPaths(id, located, f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.mediaItems().value(id).sourceSize == 6);
	REQUIRE(catalog.updateMediaItemPaths(id, outsideLibraryPath("videos/Gone.mp4"), f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.mediaItems().value(id).sourceSize == 6);
	requireRebuildStable(catalog);
}

TEST_CASE("refreshSourceSize records the current size", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photoPath = f.root + "/Ref.png";
	writeTextFile(photoPath, "0123");
	const MediaItemId photo = catalog.addPhoto(photoPath, {}, true);
	writeTextFile(photoPath, "0123456789");
	REQUIRE(catalog.mediaItems().value(photo).sourceSize == 4);

	REQUIRE(catalog.refreshSourceSize(photo));
	REQUIRE(catalog.mediaItems().value(photo).sourceSize == 10);
	requireRebuildStable(catalog);

	REQUIRE(QFile::remove(photoPath));
	REQUIRE_FALSE(catalog.refreshSourceSize(photo));
	REQUIRE(catalog.mediaItems().value(photo).sourceSize == 10);
}

TEST_CASE("renameLabel: folders and stored paths follow, associations survive", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId video = f.addVideo("Clip.mp4", "Alpha");
	catalog.addLabel(video, Catalog::BestLabelId);
	QString error;

	SECTION("a valid rename")
	{
		REQUIRE(catalog.renameLabel(f.alpha, "Gamma", &error));
		REQUIRE(error.isEmpty());
		REQUIRE(labelIdByName(catalog, "Alpha") == LabelId::None);
		REQUIRE(labelIdByName(catalog, "Gamma") == f.alpha);  // same id - associations preserved
		REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Gamma/Clip");
		REQUIRE(catalog.videoAtFrameFolder(f.root + "/Gamma/Clip") == video);
		REQUIRE(QDir(f.root + "/Gamma/Clip").exists());
		REQUIRE_FALSE(QDir(f.root + "/Alpha").exists());
		REQUIRE(catalog.mediaItemHasLabel(video, f.alpha));
		REQUIRE(catalog.mediaItemHasLabel(video, Catalog::BestLabelId));
		requireRebuildStable(catalog);
	}
	SECTION("renaming to an existing label's name is refused, including a case variant")
	{
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "Beta", &error));
		REQUIRE_FALSE(error.isEmpty());
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "BETA", &error));
		REQUIRE(labelIdByName(catalog, "Alpha") == f.alpha);
		REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Alpha/Clip");
	}
	SECTION("Best cannot be renamed")
	{
		REQUIRE_FALSE(catalog.renameLabel(Catalog::BestLabelId, "Gamma", &error));
	}
	SECTION("unsafe or reserved names are refused")
	{
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "..", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "a/b", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "a\\b", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "CON", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "Photos", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "best", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "trailing.", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, " padded", &error));
		REQUIRE_FALSE(catalog.renameLabel(f.alpha, "", &error));
		REQUIRE(labelIdByName(catalog, "Alpha") == f.alpha);
	}
}

TEST_CASE("deleteLabel: relocates, untags, refuses orphaning", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId stored1 = f.addVideo("One.mp4", "Alpha");
	catalog.addLabel(stored1, f.beta);
	const MediaItemId stored2 = f.addVideo("Two.mp4", "Alpha");  // Alpha is its only label so far
	const MediaItemId tagger = f.addVideo("Three.mp4", "Beta");
	catalog.addLabel(tagger, f.alpha);

	// stored2 would be orphaned: the impact reports it, and the delete is refused wholesale.
	{
		const Catalog::DeleteImpact impact = catalog.deleteLabelImpact(f.alpha);
		REQUIRE(impact.relocateCount == 2);
		REQUIRE(impact.untagCount == 1);
		REQUIRE(impact.wouldOrphan);
	}
	REQUIRE_FALSE(catalog.deleteLabel(f.alpha));
	REQUIRE(labelIdByName(catalog, "Alpha") == f.alpha);
	REQUIRE(catalog.folderForMediaItem(stored1) == f.root + "/Alpha/One");

	catalog.addLabel(stored2, f.beta);
	{
		const Catalog::DeleteImpact impact = catalog.deleteLabelImpact(f.alpha);
		REQUIRE(impact.relocateCount == 2);
		REQUIRE(impact.untagCount == 1);
		REQUIRE_FALSE(impact.wouldOrphan);
	}
	REQUIRE(catalog.deleteLabel(f.alpha));
	REQUIRE(labelIdByName(catalog, "Alpha") == LabelId::None);
	REQUIRE(catalog.folderForMediaItem(stored1) == f.root + "/Beta/One");
	REQUIRE(catalog.folderForMediaItem(stored2) == f.root + "/Beta/Two");
	REQUIRE(QDir(f.root + "/Beta/One").exists());
	REQUIRE(QDir(f.root + "/Beta/Two").exists());
	REQUIRE_FALSE(catalog.mediaItemHasLabel(tagger, f.alpha));
	REQUIRE_FALSE(QDir(f.root + "/Alpha").exists());  // the emptied storage folder is removed
	requireRebuildStable(catalog);
}

TEST_CASE("deleteLabel: a blocked relocation keeps the label and reports what stayed", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId blocked = f.addVideo("One.mp4", "Alpha");
	catalog.addLabel(blocked, f.beta);
	const MediaItemId movable = f.addVideo("Two.mp4", "Alpha");
	catalog.addLabel(movable, f.beta);

	// An occupied destination is what blocks the move; the rest of the deletion still runs.
	REQUIRE(QDir{}.mkpath(f.root + "/Beta/One"));

	std::vector<MediaItemId> itemsLeftBehind;
	REQUIRE_FALSE(catalog.deleteLabel(f.alpha, &itemsLeftBehind));
	REQUIRE(itemsLeftBehind.size() == 1);
	REQUIRE(itemsLeftBehind.front() == blocked);

	REQUIRE(labelIdByName(catalog, "Alpha") == f.alpha);
	REQUIRE(catalog.folderForMediaItem(blocked) == f.root + "/Alpha/One");
	REQUIRE(catalog.folderForMediaItem(movable) == f.root + "/Beta/Two");
	requireRebuildStable(catalog);
}

TEST_CASE("A mutated library reloads to an identical state", "[catalog][persistence]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaItemId video = f.addVideo("Clip.mp4", "Alpha");
	catalog.addLabel(video, f.beta);
	catalog.addLabel(video, Catalog::BestLabelId);
	catalog.setColor(f.beta, "#445566");
	catalog.removeLabel(video, f.alpha);  // relocates Alpha/Clip -> Beta/Clip
	REQUIRE(QFile::rename(f.root + "/Beta/Clip", f.root + "/Beta/Renamed"));
	REQUIRE(catalog.updateMediaItemPaths(video, outsideLibraryPath("videos/Renamed.mp4"), f.root + "/Beta/Renamed"));
	catalog.removeMediaItem(f.addVideo("Doomed.mp4", "Beta"));
	REQUIRE(f.library.flushPendingWrites());

	Library reloaded;
	REQUIRE(reloaded.setRoot(f.tempDir.path()));
	REQUIRE(dumpCatalog(reloaded.catalog()) == dumpCatalog(catalog));
}

TEST_CASE("findMediaItemWithSameContent: same-size items are byte-compared, the rest skipped", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photosBeta = f.root + "/Photos/Beta";
	REQUIRE(QDir{}.mkpath(photosBeta));
	const QString storedPath = photosBeta + "/Stored.png";
	writeTextFile(storedPath, "0123456789");  // 10 bytes
	const MediaItemId photo = catalog.addPhoto(storedPath, photosBeta, false);

	// A byte-identical file under a different name is the same content re-encountered.
	const QString identical = f.root + "/copy.png";
	writeTextFile(identical, "0123456789");
	REQUIRE(catalog.findMediaItemWithSameContent(identical) == photo);

	// Same size clears the gate but the byte compare rejects it - the case that catches a broken gate or
	// a broken compare masking each other.
	const QString sameSizeOtherBytes = f.root + "/other.png";
	writeTextFile(sameSizeOtherBytes, "9876543210");  // 10 bytes, different content
	REQUIRE(catalog.findMediaItemWithSameContent(sameSizeOtherBytes) == MediaItemId::None);

	// A different size never reaches the byte compare.
	const QString differentSize = f.root + "/short.png";
	writeTextFile(differentSize, "012");
	REQUIRE(catalog.findMediaItemWithSameContent(differentSize) == MediaItemId::None);

	// The gate is the recorded size: a tracked file grown since is not compared.
	writeTextFile(storedPath, "0123456789ab");
	writeTextFile(identical, "0123456789ab");
	REQUIRE(catalog.findMediaItemWithSameContent(identical) == MediaItemId::None);
	REQUIRE(catalog.refreshSourceSize(photo));
	REQUIRE(catalog.findMediaItemWithSameContent(identical) == photo);
}

TEST_CASE("removeLabel: a relocation blocked by a name collision leaves the item untouched", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	SECTION("a video's frame folder")
	{
		const MediaItemId video = f.addVideo("Clip.mp4", "Alpha");  // <root>/Alpha/Clip
		catalog.addLabel(video, f.beta);
		// A different item already occupies the destination basename.
		f.addVideo("Clip.mp4", "Beta", "other");  // <root>/Beta/Clip

		catalog.removeLabel(video, f.alpha);  // would relocate Alpha/Clip -> Beta/Clip, which exists -> refused

		REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Alpha/Clip");
		REQUIRE(catalog.mediaItemHasLabel(video, f.alpha));
		REQUIRE(catalog.mediaItemHasLabel(video, f.beta));
		REQUIRE(QDir(f.root + "/Alpha/Clip").exists());
		REQUIRE(QDir(f.root + "/Beta/Clip").exists());
		requireRebuildStable(catalog);
	}

	SECTION("an owned photo's file")
	{
		const QString photosAlpha = f.root + "/Photos/Alpha";
		const QString photosBeta  = f.root + "/Photos/Beta";
		REQUIRE(QDir{}.mkpath(photosAlpha));
		REQUIRE(QDir{}.mkpath(photosBeta));
		const QString aPath = photosAlpha + "/pic.png";
		const QString bPath = photosBeta + "/pic.png";
		writeTextFile(aPath, "aaaa");
		writeTextFile(bPath, "bbbbbb");
		const MediaItemId photo = catalog.addPhoto(aPath, photosAlpha, false);
		catalog.addLabel(photo, f.beta);
		REQUIRE(catalog.addPhoto(bPath, photosBeta, false) != MediaItemId::None);

		catalog.removeLabel(photo, f.alpha);  // would relocate to Photos/Beta/pic.png, which exists -> refused

		REQUIRE(catalog.folderForMediaItem(photo) == photosAlpha);
		REQUIRE(catalog.sourcePathForMediaItem(photo) == aPath);
		REQUIRE(catalog.mediaItemHasLabel(photo, f.alpha));
		REQUIRE(QFileInfo::exists(aPath));
		REQUIRE(QFileInfo::exists(bPath));
		requireRebuildStable(catalog);
	}
}
