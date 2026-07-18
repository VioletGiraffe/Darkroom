#pragma once

#include "Core/Catalog.h"

#include "3rdparty/catch2/catch.hpp"

#include <QFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <string>

namespace Catch {
	template <> struct StringMaker<QString> {
		static std::string convert(const QString& s) { return '"' + s.toStdString() + '"'; }
	};
}

[[nodiscard]] inline LabelId labelIdByName(const Catalog& catalog, const QString& displayName)
{
	for (const Catalog::Label& l : catalog.allLabels())
		if (l.displayName == displayName)
			return l.id;
	return LabelId::None;
}

// Deterministic textual snapshot of the registry + item model, for whole-state comparison across
// rebuildIndex() and save/reload boundaries. Labels keep registry (display) order; items are sorted
// by key because QHash order is unspecified.
[[nodiscard]] inline QString dumpCatalog(const Catalog& catalog)
{
	QString out;
	for (const Catalog::Label& l : catalog.allLabels())
		out += QStringLiteral("label %1 name='%2' color='%3'\n").arg(toUInt64(l.id)).arg(l.displayName, l.color);

	QStringList items;
	const auto& mediaItems = catalog.mediaItems();
	for (auto it = mediaItems.cbegin(); it != mediaItems.cend(); ++it)
	{
		QStringList labelIds;
		for (const LabelId labelId : it->labelIds)
			labelIds.push_back(QString::number(toUInt64(labelId)));
		items.push_back(QStringLiteral("item %1 name='%2' folder='%3' source='%4' labels=[%5] split=%6 duration=%7 type=%8 referenced=%9")
			.arg(it.key().key(), it.key().name(), it->folder, it->sourcePath, labelIds.join(','))
			.arg(it->splitIntoFrames).arg(it->durationMs)
			.arg(it->type == Catalog::MediaType::Photo ? QStringLiteral("photo") : QStringLiteral("video"))
			.arg(it->referenced));
	}
	std::sort(items.begin(), items.end());
	out += items.join('\n');
	return out;
}

// The mutation-vs-load consistency check: every mutation must leave the in-memory model exactly as a
// fresh re-derivation from the persisted store would rebuild it. Catches the two code paths drifting.
inline void requireRebuildStable(Catalog& catalog)
{
	const QString before = dumpCatalog(catalog);
	catalog.rebuildIndex();
	REQUIRE(dumpCatalog(catalog) == before);
}

inline void writeTextFile(const QString& path, const QByteArray& utf8Content)
{
	QFile file(path);
	REQUIRE(file.open(QIODevice::WriteOnly));
	REQUIRE(file.write(utf8Content) == utf8Content.size());
}
