#pragma once

#include "Core/VideoId.h"

#include <QJsonObject>
#include <QList>

// App-owned per-video metadata that isn't derivable from disk (e.g. playback loop intervals, and
// later: labels). Persisted as a single JSON document in the collection root, keyed by VideoId, one
// record (a JSON object of named fields) per video. Field-granular get/set so independent features
// can share a record without clobbering each other's fields. Single shared instance; writes persist
// immediately, unless deferred by a batch (see beginBatch/endBatch). GUI-thread only - no internal locking.
class MetadataStore
{
public:
	static MetadataStore& instance();

	QJsonValue get(const VideoId& id, QStringView field) const;
	void set(const VideoId& id, QStringView field, const QJsonValue& value);

	// Drops a video's whole record. Used when a video is deleted outright so it doesn't linger as a ghost
	// entry once the catalog (not the disk walk) is the authoritative set of videos. No-op if absent.
	void remove(const VideoId& id);

	// Every video that has a record, reconstructed from its key (size) and stored "name" (original case).
	// The catalog enumerates videos through here instead of walking the filesystem. Includes source-unavailable
	// placeholders (negative size), so a video whose source went missing still surfaces.
	[[nodiscard]] QList<VideoId> allVideoIds() const;

	// Moves a video's whole record from oldId to newId, preserving every field. Used when an in-app rename
	// changes the source file name (and thus the VideoId) so the metadata (loop intervals, labels, ...)
	// follows the rename instead of being orphaned. No-op if there is no record under oldId.
	void rekey(const VideoId& oldId, const VideoId& newId);

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

	QJsonObject _records;             // VideoId::key() -> record object
	int         _batchDepth = 0;      // > 0 while any BatchScope is alive; only the outermost's destructor flushes
	bool        _dirty      = false;  // a save was deferred while batching and still needs to be flushed
};
