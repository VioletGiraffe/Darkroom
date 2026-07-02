#pragma once

#include "Core/MediaId.h"

#include <QJsonObject>
#include <QList>

// App-owned per-video metadata that isn't derivable from disk (e.g. playback loop intervals, and
// later: labels). Persisted as a single JSON document in the collection root, keyed by MediaId, one
// record (a JSON object of named fields) per item. Field-granular get/set so independent features
// can share a record without clobbering each other's fields. Single shared instance; writes persist
// immediately, unless deferred by a batch (see beginBatch/endBatch). GUI-thread only - no internal locking.
class MetadataStore
{
public:
	static MetadataStore& instance();

	QJsonValue get(const MediaId& id, QStringView field) const;
	void set(const MediaId& id, QStringView field, const QJsonValue& value);

	// Drops an item's whole record. Used when an item is deleted outright so it doesn't linger as a ghost
	// entry once the catalog (not the disk walk) is the authoritative set of items. No-op if absent.
	void remove(const MediaId& id);

	// Every item that has a record, reconstructed from its key (size) and stored "name" (original case).
	// The catalog enumerates items through here instead of walking the filesystem. Includes source-unavailable
	// placeholders (negative size), so an item whose source went missing still surfaces.
	[[nodiscard]] QList<MediaId> allMediaIds() const;

	// Moves an item's whole record from oldId to newId, preserving every field. Used when an in-app rename
	// changes the source file name (and thus the MediaId) so the metadata (loop intervals, labels, ...)
	// follows the rename instead of being orphaned. No-op if there is no record under oldId.
	void rekey(const MediaId& oldId, const MediaId& newId);

	// Paired calls backing Catalog::BatchScope: while a batch is open (depth > 0), set()/remove()/rekey() still
	// update the in-memory record immediately but defer the on-disk write; endBatch() writes once, only when
	// the outermost batch closes and only if anything actually changed. Nest freely. Not meant to be called
	// directly - construct a Catalog::BatchScope instead, which pairs these correctly via RAII.
	void beginBatch();
	void endBatch();

	MetadataStore(const MetadataStore&) = delete;
	MetadataStore& operator=(const MetadataStore&) = delete;

private:
	MetadataStore();

	void load();
	void save() const;
	void scheduleSave();  // saves now, unless a BatchScope is active - then just marks dirty for its flush
	QString filePath() const;

	QJsonObject _records;             // MediaId::key() -> record object
	int         _batchDepth = 0;      // > 0 while any BatchScope is alive; only the outermost's destructor flushes
	bool        _dirty      = false;  // a save was deferred while batching and still needs to be flushed
};
