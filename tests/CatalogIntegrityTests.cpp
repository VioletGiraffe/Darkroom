#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "Utils.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

namespace
{
	// A loaded library plus the disk-staging helpers the scan needs: it reasons purely over the catalog model
	// and the on-disk layout, so every test here builds a concrete <root>/<label>/<frameFolder> + preview tree.
	struct IntegrityFixture
	{
		IntegrityFixture()
		{
			REQUIRE(tempDir.isValid());
			REQUIRE(library.setRoot(tempDir.path()));
			root = library.rootFolder();
		}

		[[nodiscard]] Catalog& catalog() { return library.catalog(); }

		[[nodiscard]] QString frameFolder(const QString& name, const QString& label) const
		{
			return root + '/' + label + '/' + QFileInfo(name).completeBaseName();
		}

		// A dummy frame image inside dir (created if needed). Only its presence and suffix matter to the scan.
		void putFrame(const QString& dir, const QString& name = "frame_0001.png")
		{
			REQUIRE(QDir{}.mkpath(dir));
			writeTextFile(dir + '/' + name, "x");
		}

		// Registers a video with frame folder <root>/<label>/<base>. Its source lives under <root>/sources
		// (a files-only dir the two-level untracked walk never descends into), created iff sourcePresent.
		MediaId addVideo(const QString& name, qint64 size, const QString& label, bool splitComplete, bool sourcePresent)
		{
			REQUIRE(QDir{}.mkpath(frameFolder(name, label)));
			const QString sourcePath = root + "/sources/" + name;
			if (sourcePresent)
			{
				REQUIRE(QDir{}.mkpath(root + "/sources"));
				writeTextFile(sourcePath, "src");
			}
			const MediaId id = MediaId::fromNameAndSize(name, size);
			REQUIRE(catalog().addMediaItem(id, sourcePath, frameFolder(name, label), splitComplete));
			return id;
		}

		QTemporaryDir tempDir;
		Library library;
		QString root;
	};

	[[nodiscard]] const CatalogIntegrity::MediaIssue* videoIssueFor(const CatalogIntegrity::IntegrityReport& r, const MediaId& id)
	{
		for (const CatalogIntegrity::MediaIssue& i : r.issues)
			if (i.id == id)
				return &i;
		return nullptr;
	}

	[[nodiscard]] const CatalogIntegrity::PhotoIssue* photoIssueFor(const CatalogIntegrity::IntegrityReport& r, const MediaId& id)
	{
		for (const CatalogIntegrity::PhotoIssue& i : r.photoIssues)
			if (i.id == id)
				return &i;
		return nullptr;
	}
}

TEST_CASE("scan: each video verdict is reported, a healthy video is not", "[integrity]")
{
	IntegrityFixture f;

	// Healthy: split done, real frames + preview + source all present.
	const MediaId healthy = f.addVideo("healthy.mp4", 1, "L", true, true);
	f.putFrame(f.frameFolder("healthy.mp4", "L"));
	f.putFrame(Catalog::previewDirFor(f.frameFolder("healthy.mp4", "L")));

	// Extracted frames missing: split done but the real frames (the deliverable) are gone; preview still renders the card.
	const MediaId noFrames = f.addVideo("noframes.mp4", 2, "L", true, true);
	f.putFrame(Catalog::previewDirFor(f.frameFolder("noframes.mp4", "L")));

	// Preview missing: the card falls back to the real frames, which are still there.
	const MediaId noPreview = f.addVideo("nopreview.mp4", 3, "L", true, true);
	f.putFrame(f.frameFolder("nopreview.mp4", "L"));

	// Stale split flag: real frames already exist but the entry is still flagged preview-only.
	const MediaId staleFlag = f.addVideo("staleflag.mp4", 4, "L", false, true);
	f.putFrame(f.frameFolder("staleflag.mp4", "L"));
	f.putFrame(Catalog::previewDirFor(f.frameFolder("staleflag.mp4", "L")));

	// Source missing: backing intact, but the source file is gone (overlays any cell; here the only fault).
	const MediaId noSource = f.addVideo("nosource.mp4", 5, "L", true, false);
	f.putFrame(f.frameFolder("nosource.mp4", "L"));
	f.putFrame(Catalog::previewDirFor(f.frameFolder("nosource.mp4", "L")));

	const CatalogIntegrity::IntegrityReport report = CatalogIntegrity::scan(f.catalog(), f.root);

	REQUIRE(videoIssueFor(report, healthy) == nullptr);

	// Each verdict holds in isolation - the predicates are orthogonal, so exactly one fires per entry here.
	const CatalogIntegrity::MediaIssue* nf = videoIssueFor(report, noFrames);
	REQUIRE(nf);
	REQUIRE(nf->extractedFramesMissing());
	REQUIRE_FALSE(nf->previewMissing());
	REQUIRE_FALSE(nf->splitFlagStale());
	REQUIRE_FALSE(nf->sourceMissing());

	const CatalogIntegrity::MediaIssue* np = videoIssueFor(report, noPreview);
	REQUIRE(np);
	REQUIRE(np->previewMissing());
	REQUIRE_FALSE(np->extractedFramesMissing());
	REQUIRE_FALSE(np->splitFlagStale());
	REQUIRE_FALSE(np->sourceMissing());

	const CatalogIntegrity::MediaIssue* sf = videoIssueFor(report, staleFlag);
	REQUIRE(sf);
	REQUIRE(sf->splitFlagStale());
	REQUIRE_FALSE(sf->extractedFramesMissing());
	REQUIRE_FALSE(sf->previewMissing());
	REQUIRE_FALSE(sf->sourceMissing());

	const CatalogIntegrity::MediaIssue* ns = videoIssueFor(report, noSource);
	REQUIRE(ns);
	REQUIRE(ns->sourceMissing());
	REQUIRE_FALSE(ns->extractedFramesMissing());
	REQUIRE_FALSE(ns->previewMissing());
	REQUIRE_FALSE(ns->splitFlagStale());

	REQUIRE(report.issues.size() == 4u);
	REQUIRE(report.photoIssues.empty());
	REQUIRE(report.untracked.empty());
	REQUIRE(report.untrackedPhotos.empty());
}

TEST_CASE("scan: a photo with a missing source is flagged, owned vs referenced", "[integrity]")
{
	IntegrityFixture f;
	Catalog& catalog = f.catalog();

	const QString photosAlpha = f.root + "/Photos/Alpha";
	REQUIRE(QDir{}.mkpath(photosAlpha));

	// Present owned photo -> healthy, no issue.
	const QString presentPath = photosAlpha + "/present.png";
	writeTextFile(presentPath, "x");
	const MediaId present = MediaId::fromFile(presentPath);
	REQUIRE(catalog.addPhoto(present, presentPath, photosAlpha, false));

	// Owned photo whose library file is gone -> LOST (referenced == false).
	const MediaId lost = MediaId::fromNameAndSize("lost.png", 10);
	REQUIRE(catalog.addPhoto(lost, photosAlpha + "/lost.png", photosAlpha, false));

	// Referenced photo whose external file moved/unmounted -> GONE (referenced == true).
	const MediaId gone = MediaId::fromNameAndSize("gone.png", 20);
	REQUIRE(catalog.addPhoto(gone, f.root + "/nonexistent/gone.png", {}, true));

	const CatalogIntegrity::IntegrityReport report = CatalogIntegrity::scan(catalog, f.root);

	REQUIRE(photoIssueFor(report, present) == nullptr);

	const CatalogIntegrity::PhotoIssue* lostIssue = photoIssueFor(report, lost);
	REQUIRE(lostIssue);
	REQUIRE_FALSE(lostIssue->referenced);

	const CatalogIntegrity::PhotoIssue* goneIssue = photoIssueFor(report, gone);
	REQUIRE(goneIssue);
	REQUIRE(goneIssue->referenced);

	REQUIRE(report.photoIssues.size() == 2u);
	REQUIRE(report.issues.empty());
	REQUIRE(report.untrackedPhotos.empty());  // the present photo is claimed; the missing ones aren't on disk
}

TEST_CASE("scan: untracked frame folders and loose photos surface; claimed and frame-less ones don't", "[integrity]")
{
	IntegrityFixture f;
	Catalog& catalog = f.catalog();

	// A claimed, healthy video -> not untracked.
	f.addVideo("kept.mp4", 1, "Kept", true, true);
	f.putFrame(f.frameFolder("kept.mp4", "Kept"));
	f.putFrame(Catalog::previewDirFor(f.frameFolder("kept.mp4", "Kept")));

	// An unclaimed, frame-bearing folder -> untracked (a video the catalog is missing).
	const QString orphan = f.root + "/Stray/Orphan";
	f.putFrame(orphan);

	// An unclaimed folder with no frames -> junk, not a missing video.
	const QString frameless = f.root + "/Stray/Frameless";
	REQUIRE(QDir{}.mkpath(frameless));
	writeTextFile(frameless + "/notes.txt", "x");

	// Under Photos/<label>: one claimed owned photo and one loose image file.
	const QString photosAlpha = f.root + "/Photos/Alpha";
	REQUIRE(QDir{}.mkpath(photosAlpha));
	const QString trackedPhoto = photosAlpha + "/tracked.png";
	writeTextFile(trackedPhoto, "x");
	REQUIRE(catalog.addPhoto(MediaId::fromFile(trackedPhoto), trackedPhoto, photosAlpha, false));
	const QString loose = photosAlpha + "/loose.png";
	writeTextFile(loose, "x");

	const CatalogIntegrity::IntegrityReport report = CatalogIntegrity::scan(catalog, f.root);

	REQUIRE(report.untracked.size() == 1u);
	REQUIRE(pathComparisonKey(report.untracked.front().folderPath) == pathComparisonKey(orphan));

	REQUIRE(report.untrackedPhotos.size() == 1u);
	REQUIRE(pathComparisonKey(report.untrackedPhotos.front().filePath) == pathComparisonKey(loose));
	REQUIRE(report.untrackedPhotos.front().labelName == "Alpha");
}
