#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <optional>
#include <utility>

// Video verdicts are independent: a completed split needs real frames, every video needs a preview, and real
// frames behind a preview-only flag make that flag stale. Source absence overlays any combination. A photo has
// only its source file. Every item is also checked for label ids absent from the registry, and a present source
// for a size other than the recorded one. The final disk walk compares the tree under the root with the layout
// the catalog and label registry define.

namespace CatalogIntegrity {

namespace {

// The OS or a sync client recreates these after deletion.
[[nodiscard]] bool isIgnoredSystemEntry(const QString& name, bool atRoot)
{
	static const QStringList anywhere{ "Thumbs.db", "desktop.ini", ".DS_Store", ".directory", "Icon\r" };
	// Exist only at a volume root or a sync root.
	static const QStringList atRootOnly{ "$RECYCLE.BIN", "System Volume Information", ".Spotlight-V100", ".fseventsd", ".Trashes",
		".TemporaryItems", ".stfolder", ".stversions", ".dropbox", ".dropbox.attr", ".dropbox.cache", ".sync" };

	if (anywhere.contains(name, Qt::CaseInsensitive) || name.startsWith(QLatin1String("._")))  // ._*: macOS AppleDouble files
		return true;
	return atRoot && (atRootOnly.contains(name, Qt::CaseInsensitive) || name.startsWith(QLatin1String(".Trash-")));
}

// Links are reported, never followed.
[[nodiscard]] bool isRealFolder(const QFileInfo& info)
{
	return info.isDir() && !info.isSymLink() && !info.isJunction();
}

// Empty unless the folder holds a preview of frame images, or real frames named the way extraction names them.
[[nodiscard]] std::optional<UntrackedFolder> asDarkroomFrameFolder(const QString& folder)
{
	static const QRegularExpression extractedFrameName(QStringLiteral(R"(^\d{4,}_)"));
	const bool realFramesPresent = std::ranges::any_of(listFrameImageFiles(QDir(folder)),
		[](const QString& name) { return extractedFrameName.match(name).hasMatch(); });
	const bool previewPresent = !listFrameImageFiles(QDir(Catalog::previewDirFor(folder))).isEmpty();
	if (!realFramesPresent && !previewPresent)
		return std::nullopt;
	return UntrackedFolder{ folder, realFramesPresent, previewPresent };
}

// Reports every file and folder under the root that the catalog's layout does not account for.
class LayoutWalk
{
public:
	LayoutWalk(const Catalog& catalog, const QString& rootFolder, IntegrityReport& report, const ScanProgress& progress);
	void run() { walkFolder(_root, Role::Root); }

private:
	enum class Role { Root, Storage, PhotosRoot, PhotoLabel, FrameFolder, Preview, Known, Unknown };

	void walkFolder(const QString& folder, Role role);
	[[nodiscard]] Role roleOfChildFolder(const QString& folder, Role role, const QFileInfo& child) const;
	[[nodiscard]] QFileInfoList entriesOf(const QString& folder, bool atRoot);
	// Adds folder and every folder between it and the root; folders outside the root are skipped.
	void addKnownFolder(const QString& folder);
	[[nodiscard]] size_t untrackedMediaCount() const
	{
		return _report.untracked.size() + _report.untrackedPhotos.size() + _report.untrackedVideos.size();
	}

	const QString& _root;
	IntegrityReport& _report;
	const ScanProgress& _progress;
	int _walkedFolders = 0;

	// pathComparisonKey sets.
	QSet<QString> _knownFiles;
	QSet<QString> _knownFolders;  // label folders and every folder above a tracked path
	QSet<QString> _frameFolders;
};

LayoutWalk::LayoutWalk(const Catalog& catalog, const QString& rootFolder, IntegrityReport& report, const ScanProgress& progress)
	: _root(rootFolder), _report(report), _progress(progress)
{
	for (const QString& file : Library::persistenceFilePaths(rootFolder))
		_knownFiles.insert(pathComparisonKey(file));

	for (const Catalog::Label& label : catalog.allLabels())
	{
		addKnownFolder(catalog.storageFolderForLabel(label.id));
		addKnownFolder(catalog.photoFolderForLabel(label.id));
	}

	for (const Catalog::Entry& entry : catalog.mediaItems())
	{
		if (entry.type == Catalog::MediaType::Video)
		{
			_frameFolders.insert(pathComparisonKey(entry.folder));
			addKnownFolder(QFileInfo(entry.folder).path());
		}
		else
			addKnownFolder(entry.folder);

		if (!entry.sourcePath.isEmpty())
		{
			_knownFiles.insert(pathComparisonKey(entry.sourcePath));
			addKnownFolder(QFileInfo(entry.sourcePath).path());
		}
	}
}

void LayoutWalk::addKnownFolder(const QString& folder)
{
	const std::optional<QString> relative = pathRelativeTo(folder, _root);
	if (!relative || relative->isEmpty())
		return;

	QString path = _root;
	for (const QString& part : relative->split('/'))
	{
		path += '/' + part;
		_knownFolders.insert(pathComparisonKey(path));
	}
}

QFileInfoList LayoutWalk::entriesOf(const QString& folder, bool atRoot)
{
	if (_progress)
		_progress(ScanPhase::Folders, _walkedFolders++, 0);

	QFileInfoList entries = QDir(folder).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
	entries.removeIf([atRoot](const QFileInfo& info) { return isIgnoredSystemEntry(info.fileName(), atRoot); });
	return entries;
}

LayoutWalk::Role LayoutWalk::roleOfChildFolder(const QString& folder, Role role, const QFileInfo& child) const
{
	const QString key = pathComparisonKey(child.filePath());
	switch (role)
	{
	case Role::Root:
		return child.fileName().compare(Catalog::PhotosDirectoryName.toString(), Qt::CaseInsensitive) == 0 ? Role::PhotosRoot : Role::Storage;
	case Role::PhotosRoot:
		return Role::PhotoLabel;
	case Role::Storage:
		if (_frameFolders.contains(key))
			return Role::FrameFolder;
		break;
	case Role::FrameFolder:
		if (key == pathComparisonKey(Catalog::previewDirFor(folder)))
			return Role::Preview;
		break;
	case Role::PhotoLabel:
	case Role::Preview:
	case Role::Known:
	case Role::Unknown:
		break;
	}
	return _knownFolders.contains(key) ? Role::Known : Role::Unknown;
}

void LayoutWalk::walkFolder(const QString& folder, Role role)
{
	const size_t mediaBefore = untrackedMediaCount();
	const size_t unknownBefore = _report.unknownItems.size();

	for (const QFileInfo& info : entriesOf(folder, role == Role::Root))
	{
		const QString path = info.filePath();
		if (_knownFiles.contains(pathComparisonKey(path)))
			continue;

		if (!info.isDir())
		{
			if ((role == Role::FrameFolder || role == Role::Preview) && isFrameImageFile(info.fileName()))
				continue;
			if (isSupportedImageFile(path))
				_report.untrackedPhotos.push_back({ path, role == Role::PhotoLabel ? QFileInfo(folder).fileName() : QString{} });
			else if (isSupportedVideoFile(path))
				_report.untrackedVideos.push_back({ path });
			else
				_report.unknownItems.push_back({ path, false });
			continue;
		}
		if (!isRealFolder(info))
		{
			_report.unknownItems.push_back({ path, true });
			continue;
		}

		const Role childRole = roleOfChildFolder(folder, role, info);
		// Frame folders live only directly in a storage folder.
		if (childRole == Role::Unknown && role == Role::Storage)
		{
			if (const std::optional<UntrackedFolder> untracked = asDarkroomFrameFolder(path))
			{
				_report.untracked.push_back(*untracked);
				continue;
			}
		}
		walkFolder(path, childRole);
	}

	// An unaccounted-for folder holding no untracked media is reported whole.
	const bool unaccounted = role == Role::Unknown
		|| ((role == Role::Storage || role == Role::PhotoLabel) && !_knownFolders.contains(pathComparisonKey(folder)));
	if (unaccounted && untrackedMediaCount() == mediaBefore)
	{
		_report.unknownItems.resize(unknownBefore);
		_report.unknownItems.push_back({ folder, true });
	}
}

} // namespace

IntegrityReport scan(const Catalog& catalog, const QString& rootFolder, const ScanProgress& progress)
{
	IntegrityReport report;

	QSet<LabelId> knownLabelIds;
	for (const Catalog::Label& label : catalog.allLabels())
		knownLabelIds.insert(label.id);

	const int itemCount = catalog.mediaItemCount();
	int checkedItems = 0;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		if (progress)
			progress(ScanPhase::Items, checkedItems++, itemCount);

		DanglingLabelIssue labelIssue{ .id = id };
		for (const LabelId labelId : entry.labelIds)
			if (!knownLabelIds.contains(labelId))
				labelIssue.missingLabelIds.push_back(labelId);
		if (!labelIssue.missingLabelIds.empty())
			report.danglingLabelIssues.push_back(std::move(labelIssue));

		if (const QFileInfo source{ entry.sourcePath }; source.isFile() && source.size() != entry.sourceSize)
			report.changedSizeIssues.push_back({ id, entry.sourcePath, entry.sourceSize, source.size() });

		if (entry.type == Catalog::MediaType::Photo)
		{
			if (entry.sourcePath.isEmpty() || !QFileInfo::exists(entry.sourcePath))
				report.photoIssues.push_back({ id, entry.sourcePath, entry.referenced });
			continue;
		}

		MediaIssue issue = inspectVideo(catalog, id);
		if (!issue.healthy())
			report.issues.push_back(std::move(issue));
	}

	LayoutWalk(catalog, rootFolder, report, progress).run();
	return report;
}

MediaIssue inspectVideo(const Catalog& catalog, MediaItemId id)
{
	const auto it = catalog.mediaItems().constFind(id);
	assert_and_return_r(it != catalog.mediaItems().cend() && it->type == Catalog::MediaType::Video, {});

	MediaIssue issue;
	issue.id                = id;
	issue.folder            = it->folder;
	issue.sourcePath        = it->sourcePath;
	issue.sourcePresent     = !it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath);
	issue.splitComplete     = it->splitIntoFrames;
	issue.realFramesPresent = !listFrameImageFiles(QDir(it->folder)).isEmpty();
	issue.previewPresent    = !listFrameImageFiles(QDir(Catalog::previewDirFor(it->folder))).isEmpty();
	return issue;
}

} // namespace CatalogIntegrity
