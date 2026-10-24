#pragma once

#include "Core/MediaItemId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QJsonObject>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <utility>
#include <vector>

// GUI-thread store for non-derivable per-item metadata. One root JSON document maps MediaItemIds to named-field
// records, allowing features to share an item without clobbering one another. All writes use Writer.
class MetadataStore
{
public:
	// The document is { VersionField: FormatVersion, ItemsField: { "<MediaItemId>": { field: value } } }.
	static constexpr int FormatVersion = 2;
	static constexpr QStringView VersionField = u"version";
	static constexpr QStringView ItemsField = u"items";

	// Writes update memory immediately and flush atomically when the outermost nested Writer dies. Failed
	// flushes remain dirty for retry. Do not retain a Writer beyond the mutation it batches.
	class Writer
	{
	public:
		~Writer();
		Writer(const Writer&) = delete;
		Writer& operator=(const Writer&) = delete;

		void set(MediaItemId id, QStringView field, const QJsonValue& value);

		// Drops the record too when no field remains.
		void removeField(MediaItemId id, QStringView field);

		void remove(MediaItemId id);

	private:
		friend class MetadataStore;
		explicit Writer(MetadataStore& store);

		MetadataStore& _store;
	};

	QJsonValue get(MediaItemId id, QStringView field) const;

	[[nodiscard]] std::vector<MediaItemId> allItemIds() const;

	[[nodiscard]] Writer beginBatch();

	// The backing JSON document of a store rooted at rootFolder.
	[[nodiscard]] static QString filePathFor(const QString& rootFolder);

	MetadataStore(const MetadataStore&) = delete;
	MetadataStore& operator=(const MetadataStore&) = delete;

private:
	friend class LibraryState;
	// records is the ItemsField object.
	explicit MetadataStore(QString rootFolder, QJsonObject records);

	void set(MediaItemId id, QStringView field, const QJsonValue& value);
	void removeField(MediaItemId id, QStringView field);
	void remove(MediaItemId id);

	[[nodiscard]] QString writeRecords() const;
	[[nodiscard]] bool flushPendingSave(QString* error = nullptr);
	[[nodiscard]] const QString& pendingSaveError() const { return _pendingSaveError; }
	void setPersistenceFailureHandler(std::function<void()> handler) { _persistenceFailureHandler = std::move(handler); }
	void scheduleSave();

	const QString _rootFolder;
	QJsonObject _records;
	int         _batchDepth = 0;
	bool        _dirty      = false;
	QString               _pendingSaveError;
	std::function<void()> _persistenceFailureHandler;
};
