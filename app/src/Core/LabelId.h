#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <cstdint>

// Stable label identity. [0, 1000] is reserved for special labels; Catalog mints ordinary ids monotonically
// from 1001. JSON stores numeric ids, QSettings uses uint64, and string-only UI/MIME boundaries use the decimal
// helpers below. ImportDialog's "new:<n>" values are provisional strings, not LabelIds.
enum class LabelId : uint64_t
{
	None = 0,
	Best = 1,
	// [2, 1000] reserved for future special labels; the first real label id is FirstRealLabelId.
};

inline constexpr uint64_t FirstRealLabelId = 1001;

[[nodiscard]] inline uint64_t toUInt64(LabelId id) noexcept { return static_cast<uint64_t>(id); }
[[nodiscard]] inline LabelId  labelIdFromUInt64(uint64_t value) noexcept { return static_cast<LabelId>(value); }

[[nodiscard]] inline QString toString(LabelId id) { return QString::number(toUInt64(id)); }

// None on empty or non-numeric input.
[[nodiscard]] inline LabelId labelIdFromString(const QString& s)
{
	bool ok = false;
	const qulonglong value = s.toULongLong(&ok);
	return ok ? labelIdFromUInt64(value) : LabelId::None;
}
