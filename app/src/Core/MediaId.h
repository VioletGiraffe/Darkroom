#pragma once

#include <QString>

// Stable source identity: case-insensitive filename plus byte size. Derived on demand, with no id registry.
class MediaId
{
public:
	MediaId() = default;
	static MediaId fromFile(const QString& path);

	// Reconstructs a persisted identity without touching the file.
	static MediaId fromNameAndSize(const QString& name, qint64 size);

	bool isValid() const { return _size >= 0; }
	const QString& name() const { return _name; }
	qint64 size() const { return _size; }

	// Persisted canonical key; changing its case fold would orphan existing records.
	QString key() const;
	uint64_t hash() const;

	bool operator==(const MediaId& other) const;

private:
	QString _name;       // original file name, kept for display; identity compares it case-insensitively
	qint64  _size = -1;  // -1 == invalid (no such file)
};

inline size_t qHash(const MediaId& id, size_t /*seed*/ = 0) {
	return static_cast<size_t>(id.hash());
}
