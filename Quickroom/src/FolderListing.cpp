#include "FolderListing.h"
#include "Utils.h"
#include "directoryscan/thinioqstring.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QStringView>
#include <QTimeZone>
RESTORE_COMPILER_WARNINGS

#include <utility>

namespace {

QDateTime toDateTime(const thin_io::timestamp& time)
{
	return QDateTime::fromMSecsSinceEpoch(time.seconds * 1000 + time.nanoseconds / 1'000'000, QTimeZone::UTC);
}

// Empty without a dot.
QStringView extension(const QString& name)
{
	const qsizetype dot = name.lastIndexOf(u'.');
	return dot < 0 ? QStringView{} : QStringView{ name }.sliced(dot + 1);
}

bool precedesAscending(const FolderEntry& a, const FolderEntry& b, int sortBy)
{
	const bool aIsFolder = a.kind == EntryKind::Folder;
	if (aIsFolder != (b.kind == EntryKind::Folder))
		return aIsFolder;
	if (sortBy == SortBy::Date && a.lastModified != b.lastModified)
		return a.lastModified < b.lastModified;
	if (sortBy == SortBy::Type && !aIsFolder)
	{
		if (const int byExtension = NaturalSort::compare(extension(a.name), extension(b.name), /* caseSensitive */ false); byExtension != 0)
			return byExtension < 0;
	}
	return NaturalSort::lessCaseInsensitive(a.name, b.name);
}

} // namespace

std::optional<std::vector<FolderEntry>> listFolder(const QString& folder)
{
	const auto listing = listDirectoryWithDetails(folder);
	if (!listing)
		return std::nullopt;

	const QDir dir(folder);
	std::vector<FolderEntry> entries;
	for (const thin_io::directory_entry& listed : *listing)
	{
		QString name = nativeNameToQString(listed.name);
		bool hidden = listed.attributes.hidden;
#ifndef _WIN32
		hidden = hidden || name.startsWith(u'.'); // thin_io never infers hidden from the name
#endif
		if (hidden || (listed.attributes.is_link && !listed.link_target))
			continue;

		const thin_io::entry_status& status = listed.attributes.is_link ? *listed.link_target : listed;
		EntryKind kind;
		if (status.attributes.kind == thin_io::entry_kind::directory)
			kind = EntryKind::Folder;
		else if (status.attributes.kind != thin_io::entry_kind::regular_file)
			continue;
		else if (isSupportedImageFile(name))
			kind = EntryKind::Image;
		else if (isSupportedVideoFile(name))
			kind = EntryKind::Video;
		else
			continue;

		QString path = dir.absoluteFilePath(name);
		entries.push_back({
			.path = std::move(path),
			.name = std::move(name),
			.kind = kind,
			.size = static_cast<qint64>(status.logical_size.value_or(0)),
			.lastModified = toDateTime(status.times.last_write),
		});
	}
	return entries;
}

bool precedesInOrder(const FolderEntry& a, const FolderEntry& b, const SortOrder& order)
{
	return order.descending ? precedesAscending(b, a, order.sortBy) : precedesAscending(a, b, order.sortBy);
}
