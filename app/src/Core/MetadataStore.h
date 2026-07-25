#pragma once

#include "Core/MediaId.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <utility>
#include <vector>

// GUI-thread store for non-derivable per-item metadata. One root JSON document maps MediaIds to named-field
// records, allowing features to share an item without clobbering one another. All writes use Writer.
class MetadataStore
{
public:
	// Writes update memory immediately and flush atomically when the outermost nested Writer dies. Failed
	// flushes remain dirty for retry. Do not retain a Writer beyond the mutation it batches.
	class Writer
	{
	public:
		~Writer();
		Writer(const Writer&) = delete;
		Writer& operator=(const Writer&) = delete;

		void set(const MediaId& id, QStringView field, const QJsonValue& value);

		// Drops the record too when only its reserved "name" field remains.
		void removeField(const MediaId& id, QStringView field);

		void remove(const MediaId& id);

		// Moves the complete record so metadata follows a source rename. No-op if oldId has no record.
		void rekey(const MediaId& oldId, const MediaId& newId);

	private:
		friend class MetadataStore;
		explicit Writer(MetadataStore& store);

		MetadataStore& _store;
	};

	QJsonValue get(const MediaId& id, QStringView field) const;

	// Reconstructs ids from each key's size and reserved original-case "name".
	[[nodiscard]] std::vector<MediaId> allMediaIds() const;

	[[nodiscard]] Writer beginBatch();

	MetadataStore(const MetadataStore&) = delete;
	MetadataStore& operator=(const MetadataStore&) = delete;

private:
	friend class LibraryState;
	explicit MetadataStore(QString rootFolder, QJsonObject records);

	void set(const MediaId& id, QStringView field, const QJsonValue& value);
	void removeField(const MediaId& id, QStringView field);
	void remove(const MediaId& id);
	void rekey(const MediaId& oldId, const MediaId& newId);

	[[nodiscard]] QString writeRecords() const;
	[[nodiscard]] bool flushPendingSave(QString* error = nullptr);
	[[nodiscard]] const QString& pendingSaveError() const { return _pendingSaveError; }
	void setPersistenceFailureHandler(std::function<void()> handler) { _persistenceFailureHandler = std::move(handler); }
	void scheduleSave();
	QString filePath() const;

	const QString _rootFolder;
	QJsonObject _records;
	int         _batchDepth = 0;
	bool        _dirty      = false;
	QString               _pendingSaveError;
	std::function<void()> _persistenceFailureHandler;
};
