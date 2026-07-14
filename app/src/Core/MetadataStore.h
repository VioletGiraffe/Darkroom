#pragma once

#include "Core/MediaId.h"

#include <QJsonObject>
#include <QString>

#include <vector>

// App-owned per-item metadata that isn't derivable from disk (e.g. playback loop intervals and labels).
// Persisted as a single JSON document in the root folder, keyed by MediaId, one
// record (a JSON object of named fields) per item. Field-granular get/set so independent features
// can share a record without clobbering each other's fields. Owned by Library; consumers borrow that
// instance explicitly. All writes go
// through a Writer obtained from beginBatch() (see below). GUI-thread only - no internal locking.
class MetadataStore
{
public:
	// The one write path, RAII-scoped: a write applies to the in-memory records immediately, but the disk
	// write is deferred until the *outermost* live Writer is destroyed (they nest freely - a Writer created
	// while another is alive just joins its batch), and happens only if something actually changed. So a
	// multi-field update made through one Writer reaches disk as a single atomic QSaveFile write - never as
	// a partially-updated record - and an unbatched write is impossible by construction. A one-off single
	// write can use a temporary: beginBatch().set(...) flushes at the end of the full expression.
	// Do not store a Writer beyond the mutation it batches: a long-lived one holds the batch open and defers
	// all persistence indefinitely.
	class Writer
	{
	public:
		~Writer();
		Writer(const Writer&) = delete;
		Writer& operator=(const Writer&) = delete;

		void set(const MediaId& id, QStringView field, const QJsonValue& value);

		// Drops an item's whole record. Used when an item is deleted outright so it doesn't linger as a ghost
		// entry once the catalog (not the disk walk) is the authoritative set of items. No-op if absent.
		void remove(const MediaId& id);

		// Moves an item's whole record from oldId to newId, preserving every field. Used when an in-app rename
		// changes the source file name (and thus the MediaId) so the metadata (loop intervals, labels, ...)
		// follows the rename instead of being orphaned. No-op if there is no record under oldId.
		void rekey(const MediaId& oldId, const MediaId& newId);

	private:
		friend class MetadataStore;  // Writers are created by beginBatch() only
		explicit Writer(MetadataStore& store);

		MetadataStore& _store;
	};

	QJsonValue get(const MediaId& id, QStringView field) const;

	// Every item that has a record, reconstructed from its key (size) and stored "name" (original case).
	// The catalog enumerates items through here instead of walking the filesystem.
	[[nodiscard]] std::vector<MediaId> allMediaIds() const;

	[[nodiscard]] Writer beginBatch();

	MetadataStore(const MetadataStore&) = delete;
	MetadataStore& operator=(const MetadataStore&) = delete;

private:
	friend class LibraryState;
	explicit MetadataStore(QString rootFolder);

	// The mutations behind Writer's public forwarders - private so every write is forced through a Writer's
	// batch. Each updates _records immediately and defers the disk write to the batch flush (scheduleSave).
	void set(const MediaId& id, QStringView field, const QJsonValue& value);
	void remove(const MediaId& id);
	void rekey(const MediaId& oldId, const MediaId& newId);

	void load();
	void save() const;
	void scheduleSave();  // marks dirty for the outermost Writer's flush (a Writer is always alive during a mutation)
	QString filePath() const;

	const QString _rootFolder;
	QJsonObject _records;             // MediaId::key() -> record object
	int         _batchDepth = 0;      // > 0 while any Writer is alive; only the outermost's destructor flushes
	bool        _dirty      = false;  // a save was deferred while batching and still needs to be flushed
};
