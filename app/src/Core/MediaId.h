#pragma once

#include <QString>

// Stable, derived identity for a media item's source file: its file name (matched case-insensitively) plus its
// byte size. Because it is derived from the file on demand it never has to be assigned or kept in a
// registry - recompute it from the file whenever needed, which sidesteps the file<->id de-sync that
// a synthetic, stored id would suffer. A collision (same name and size) is, in practice, a genuine
// duplicate worth flagging to the user.
class MediaId
{
public:
	MediaId() = default;
	static MediaId fromFile(const QString& path);

	// Reconstructs an identity from values already known (a persisted catalog record), without touching the
	// file. The catalog stores each record under key() (which carries the size) plus the original-case name,
	// so a record round-trips back to its MediaId through here.
	static MediaId fromNameAndSize(const QString& name, qint64 size);

	bool isValid() const { return _size >= 0; }
	const QString& name() const { return _name; }
	qint64 size() const { return _size; }

	// Canonical key for use as a map / JSON key. Folds the name's case so matching is case-insensitive.
	QString key() const;

	bool operator==(const MediaId& other) const;

private:
	QString _name;       // original file name, kept for display; identity compares it case-insensitively
	qint64  _size = -1;  // -1 == invalid (no such file)
};

size_t qHash(const MediaId& id, size_t seed = 0);
