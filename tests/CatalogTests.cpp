#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

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
		// so the disk-touching paths (relocation, label rename) can operate on it.
		MediaId addVideo(const QString& name, qint64 size, const QString& labelName)
		{
			const QString folder = root + '/' + labelName + '/' + QFileInfo(name).completeBaseName();
			REQUIRE(QDir{}.mkpath(folder));
			const MediaId id = MediaId::fromNameAndSize(name, size);
			REQUIRE(library.catalog().addMediaItem(id, "D:/videos/" + name, folder, true));
			return id;
		}

		QTemporaryDir tempDir;
		Library library;
		QString root;
		LabelId alpha = LabelId::None;
		LabelId beta = LabelId::None;
	};
}

TEST_CASE("addMediaItem: collision and re-registration rules", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaId id = f.addVideo("Clip.mp4", 1000, "Alpha");

	// Same id at a different folder: a name+size collision - refused, the existing entry untouched.
	REQUIRE_FALSE(catalog.addMediaItem(id, "D:/other/Clip.mp4", f.root + "/Beta/Clip", true));
	REQUIRE(catalog.folderForMediaItem(id) == f.root + "/Alpha/Clip");
	REQUIRE(catalog.sourcePathForMediaItem(id) == "D:/videos/Clip.mp4");

	// A case-variant of the same name is the same id - still a collision.
	REQUIRE_FALSE(catalog.addMediaItem(MediaId::fromNameAndSize("CLIP.MP4", 1000), "D:/other/CLIP.MP4", f.root + "/Beta/CLIP", true));

	// Re-registration at the same folder is not a collision: it updates the entry (the re-export /
	// on-demand split flow).
	REQUIRE(catalog.addMediaItem(id, "D:/videos/Clip.mp4", f.root + "/Alpha/Clip", false, 5000));
	REQUIRE_FALSE(catalog.isSplitIntoFrames(id));
	REQUIRE(catalog.durationMsForMediaItem(id) == 5000);

	// An unknown incoming duration must not erase the stored one.
	REQUIRE(catalog.addMediaItem(id, "D:/videos/Clip.mp4", f.root + "/Alpha/Clip", true, -1));
	REQUIRE(catalog.durationMsForMediaItem(id) == 5000);
	REQUIRE(catalog.isSplitIntoFrames(id));

	REQUIRE_FALSE(catalog.addMediaItem(MediaId{}, "D:/videos/x.mp4", f.root + "/Alpha/x", true));

	requireRebuildStable(catalog);
}

TEST_CASE("removeLabel: storage relocation and the last-ordinary-label invariant", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaId video = f.addVideo("Clip.mp4", 1000, "Alpha");
	catalog.addLabel(video, f.beta);
	catalog.addLabel(video, Catalog::BestLabelId);

	// Removing the storage label relocates the frame folder to the remaining ordinary label.
	catalog.removeLabel(video, f.alpha);
	REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Beta/Clip");
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

TEST_CASE("A referenced photo keeps its last ordinary label", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photoPath = f.root + "/Ref.png";
	writeTextFile(photoPath, "0123456789");
	const MediaId photo = MediaId::fromFile(photoPath);
	REQUIRE(catalog.addPhoto(photo, photoPath, {}, true));
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

TEST_CASE("applyRename: metadata follows the new identity, collisions are refused", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaId oldId = f.addVideo("Clip.mp4", 1000, "Alpha");
	catalog.addLabel(oldId, Catalog::BestLabelId);
	catalog.setDurationMs(oldId, 5000);
	const MediaId other = f.addVideo("Other.mp4", 2000, "Alpha");

	// Renaming onto another tracked item's identity is refused, both entries untouched.
	REQUIRE_FALSE(catalog.applyRename(oldId, other, "D:/videos/Other.mp4", f.root + "/Alpha/Other"));
	REQUIRE(catalog.folderForMediaItem(oldId) == f.root + "/Alpha/Clip");
	REQUIRE(catalog.folderForMediaItem(other) == f.root + "/Alpha/Other");

	// A real rename: new name, new folder; labels and duration ride along to the new identity.
	const MediaId newId = MediaId::fromNameAndSize("Renamed.mp4", 1000);
	REQUIRE(QFile::rename(f.root + "/Alpha/Clip", f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.applyRename(oldId, newId, "D:/videos/Renamed.mp4", f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.containsMediaItem(newId));
	REQUIRE(catalog.durationMsForMediaItem(newId) == 5000);
	REQUIRE(catalog.mediaItemHasLabel(newId, Catalog::BestLabelId));
	REQUIRE(catalog.folderForMediaItem(newId) == f.root + "/Alpha/Renamed");
	requireRebuildStable(catalog);

	// A case-only rename is the same identity, but the stored original-case name must still update.
	const MediaId casedId = MediaId::fromNameAndSize("RENAMED.MP4", 1000);
	REQUIRE(catalog.applyRename(newId, casedId, "D:/videos/RENAMED.MP4", f.root + "/Alpha/Renamed"));
	REQUIRE(catalog.containsMediaItem(newId));  // same identity under either spelling
	bool nameUpdated = false;
	for (auto it = catalog.mediaItems().cbegin(); it != catalog.mediaItems().cend(); ++it)
		if (it.key() == casedId)
			nameUpdated = (it.key().name() == "RENAMED.MP4");
	REQUIRE(nameUpdated);
	REQUIRE(catalog.mediaItemHasLabel(casedId, Catalog::BestLabelId));
	requireRebuildStable(catalog);
}

TEST_CASE("renameLabel: folders and stored paths follow, associations survive", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaId video = f.addVideo("Clip.mp4", 1000, "Alpha");
	catalog.addLabel(video, Catalog::BestLabelId);
	QString error;

	SECTION("a valid rename")
	{
		REQUIRE(catalog.renameLabel(f.alpha, "Gamma", &error));
		REQUIRE(error.isEmpty());
		REQUIRE(labelIdByName(catalog, "Alpha") == LabelId::None);
		REQUIRE(labelIdByName(catalog, "Gamma") == f.alpha);  // same id - associations preserved
		REQUIRE(catalog.folderForMediaItem(video) == f.root + "/Gamma/Clip");
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

	const MediaId stored1 = f.addVideo("One.mp4", 1000, "Alpha");
	catalog.addLabel(stored1, f.beta);
	const MediaId stored2 = f.addVideo("Two.mp4", 2000, "Alpha");  // Alpha is its only label so far
	const MediaId tagger = f.addVideo("Three.mp4", 3000, "Beta");
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

TEST_CASE("A mutated library reloads to an identical state", "[catalog][persistence]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const MediaId video = f.addVideo("Clip.mp4", 1000, "Alpha");
	catalog.addLabel(video, f.beta);
	catalog.addLabel(video, Catalog::BestLabelId);
	catalog.setColor(f.beta, "#445566");
	catalog.removeLabel(video, f.alpha);  // relocates Alpha/Clip -> Beta/Clip
	const MediaId newId = MediaId::fromNameAndSize("Renamed.mp4", 1000);
	REQUIRE(QFile::rename(f.root + "/Beta/Clip", f.root + "/Beta/Renamed"));
	REQUIRE(catalog.applyRename(video, newId, "D:/videos/Renamed.mp4", f.root + "/Beta/Renamed"));
	catalog.removeMediaItem(f.addVideo("Doomed.mp4", 9000, "Beta"));
	REQUIRE(f.library.flushPendingWrites());

	Library reloaded;
	REQUIRE(reloaded.setRoot(f.tempDir.path()));
	REQUIRE(dumpCatalog(reloaded.catalog()) == dumpCatalog(catalog));
}

TEST_CASE("findPhotoBySameContent: same-size photos are byte-compared, the rest skipped", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	const QString photosBeta = f.root + "/Photos/Beta";
	REQUIRE(QDir{}.mkpath(photosBeta));
	const QString storedPath = photosBeta + "/Stored.png";
	writeTextFile(storedPath, "0123456789");  // 10 bytes
	const MediaId photo = MediaId::fromFile(storedPath);
	REQUIRE(catalog.addPhoto(photo, storedPath, photosBeta, false));

	// A byte-identical file under a different name is the same content re-encountered.
	const QString identical = f.root + "/copy.png";
	writeTextFile(identical, "0123456789");
	REQUIRE(catalog.findPhotoBySameContent(identical) == storedPath);

	// Same size clears the gate but the byte compare rejects it - the case that catches a broken gate or
	// a broken compare masking each other.
	const QString sameSizeOtherBytes = f.root + "/other.png";
	writeTextFile(sameSizeOtherBytes, "9876543210");  // 10 bytes, different content
	REQUIRE(catalog.findPhotoBySameContent(sameSizeOtherBytes).isEmpty());

	// A different size never reaches the byte compare.
	const QString differentSize = f.root + "/short.png";
	writeTextFile(differentSize, "012");
	REQUIRE(catalog.findPhotoBySameContent(differentSize).isEmpty());
}

TEST_CASE("removeLabel: a relocation blocked by a name collision leaves the item untouched", "[catalog]")
{
	LibraryFixture f;
	Catalog& catalog = f.library.catalog();

	SECTION("a video's frame folder")
	{
		const MediaId video = f.addVideo("Clip.mp4", 1000, "Alpha");  // <root>/Alpha/Clip
		catalog.addLabel(video, f.beta);
		// A different item (same name, different size -> different id) already occupies the destination basename.
		f.addVideo("Clip.mp4", 2000, "Beta");  // <root>/Beta/Clip

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
		writeTextFile(bPath, "bbbbbb");  // same name, different size -> different id
		const MediaId photo = MediaId::fromFile(aPath);
		REQUIRE(catalog.addPhoto(photo, aPath, photosAlpha, false));
		catalog.addLabel(photo, f.beta);
		REQUIRE(catalog.addPhoto(MediaId::fromFile(bPath), bPath, photosBeta, false));

		catalog.removeLabel(photo, f.alpha);  // would relocate to Photos/Beta/pic.png, which exists -> refused

		REQUIRE(catalog.folderForMediaItem(photo) == photosAlpha);
		REQUIRE(catalog.sourcePathForMediaItem(photo) == aPath);
		REQUIRE(catalog.mediaItemHasLabel(photo, f.alpha));
		REQUIRE(QFileInfo::exists(aPath));
		REQUIRE(QFileInfo::exists(bPath));
		requireRebuildStable(catalog);
	}
}
