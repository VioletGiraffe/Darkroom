#pragma once

#include <QString>

// Stable, derived identity for a source video: its file name (matched case-insensitively) plus its
// byte size. Because it is derived from the file on demand it never has to be assigned or kept in a
// registry - recompute it from the file whenever needed, which sidesteps the file<->id de-sync that
// a synthetic, stored id would suffer. A collision (same name and size) is, in practice, a genuine
// duplicate worth flagging to the user.
class VideoId
{
public:
	VideoId() = default;
	static VideoId fromFile(const QString& path);

	// Reconstructs an identity from values already known (a persisted catalog record), without touching the
	// file. The catalog stores each record under key() (which carries the size) plus the original-case name,
	// so a record round-trips back to its VideoId through here. A negative size marks a "source unavailable"
	// placeholder: a video whose frames exist but whose source file's size couldn't be read (missing/unmounted
	// when the catalog was seeded). Such an id is intentionally !isValid() - it still has a usable key() and
	// name() for storage and display, but the rest of the app treats it as having no live source.
	static VideoId fromNameAndSize(const QString& name, qint64 size);

	bool isValid() const { return _size >= 0; }
	const QString& name() const { return _name; }
	qint64 size() const { return _size; }

	// Canonical key for use as a map / JSON key. Folds the name's case so matching is case-insensitive.
	QString key() const;

	bool operator==(const VideoId& other) const
	{
		return _size == other._size && _name.compare(other._name, Qt::CaseInsensitive) == 0;
	}

private:
	QString _name;       // original file name, kept for display; identity compares it case-insensitively
	qint64  _size = -1;  // -1 == invalid (no such file)
};

size_t qHash(const VideoId& id, size_t seed = 0);
