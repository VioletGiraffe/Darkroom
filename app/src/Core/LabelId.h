#pragma once

#include <QString>

#include <cstdint>

// A label's stable identity. The reserved low range [0, 1000] is for special/virtual labels (currently just
// Best); Catalog mints real, folder-backed ids monotonically from 1001, seeding its counter from the largest
// persisted id so the next value is unused.
//
// Serialization boundaries (see Catalog / LabelSidebar / MainWindow):
//   - JSON (labels.json + each item's metadata membership array): an integer-valued JSON number.
//   - QSettings (the sidebar's saved filter): the native quint64 (toUInt64 / labelIdFromUInt64).
//   - QVariant item-data roles (LabelSidebar rows): stored/read directly via QVariant::fromValue / value<LabelId>().
//
// String-only UI boundaries (label drag MIME data and ImportDialog staging) use the decimal form below.
// ImportDialog layers provisional "new:<n>" placeholders over that string space; they are not LabelIds until
// the label is materialized in Catalog.
enum class LabelId : uint64_t
{
	None = 0,   // no / invalid label
	Best = 1,   // the one virtual (folderless) label
	// [2, 1000] reserved for future special labels; the first real label id is FirstRealLabelId.
};

// Lowest id a real (folder-backed, user-created) label may take; everything below is reserved.
inline constexpr uint64_t FirstRealLabelId = 1001;

[[nodiscard]] inline uint64_t toUInt64(LabelId id) noexcept { return static_cast<uint64_t>(id); }
[[nodiscard]] inline LabelId  labelIdFromUInt64(uint64_t value) noexcept { return static_cast<LabelId>(value); }

// Decimal-string form for string-only UI and MIME boundaries.
[[nodiscard]] inline QString toString(LabelId id) { return QString::number(toUInt64(id)); }

// Parses the decimal-string form; None on empty or non-numeric input.
[[nodiscard]] inline LabelId labelIdFromString(const QString& s)
{
	bool ok = false;
	const qulonglong value = s.toULongLong(&ok);
	return ok ? labelIdFromUInt64(value) : LabelId::None;
}
