#pragma once

#include <QString>

#include <cstdint>

// A label's stable identity - a 64-bit value, never a string: label ids are only ever compared and stored,
// never parsed or human-authored. The reserved low range [0, 1000] is for special/virtual labels (currently
// just Best); real, folder-backed labels are minted at random across [1001, UINT64_MAX] (Catalog::generateLabelId).
//
// Serialization boundaries (see Catalog / LabelSidebar / MainWindow):
//   - JSON (labels.json + each item's metadata membership array): the id's *decimal-string* form (toString /
//     labelIdFromString). Storing it as a JSON *number* would be lossy - QJsonValue's integer is qint64, so the
//     top half of the u64 range wouldn't round-trip; a decimal string is exact for the whole range.
//   - QSettings (the sidebar's saved filter): the native quint64 (toUInt64 / labelIdFromUInt64).
//   - QVariant item-data roles (LabelSidebar rows): stored/read directly via QVariant::fromValue / value<LabelId>().
//
// The string-based UI layer (the ImportDialog staging list) carries ids in their decimal-string form and
// layers its own namespacing over that string space - the provisional "new:<n>" placeholders for not-yet-created
// labels. Those are never a LabelId until Import materializes them in the Catalog.
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

// Decimal-string form for JSON storage and for handing an id to the string-based UI layer.
[[nodiscard]] inline QString toString(LabelId id) { return QString::number(toUInt64(id)); }

// Parses the decimal-string form; None on empty or non-numeric input.
[[nodiscard]] inline LabelId labelIdFromString(const QString& s)
{
	bool ok = false;
	const qulonglong value = s.toULongLong(&ok);
	return ok ? labelIdFromUInt64(value) : LabelId::None;
}
