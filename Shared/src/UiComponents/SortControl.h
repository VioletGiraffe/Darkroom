#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFlags>
#include <QPushButton>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Persisted values also define the field-toggle segment order.
namespace SortBy { enum { Name, Date, Type }; }

struct SortOrder
{
	int  sortBy = SortBy::Name;
	bool descending = false;
	bool favoritesFirst = false;
};

// Persistent sort-field, direction, and optional favorites-first popover. The host performs sorting from order().
class SortControl final : public QPushButton
{
	Q_OBJECT
public:
	// What the popover offers. A field's bit is 1 << its SortBy value.
	enum Option {
		NameField       = 1 << SortBy::Name,
		DateField       = 1 << SortBy::Date,
		TypeField       = 1 << SortBy::Type,
		FavoritesOption = 1 << 8,
	};
	Q_DECLARE_FLAGS(Options, Option)

	// The order persists under settingsGroup; defaultOrder applies until it does, and options must offer its field.
	// Without FavoritesOption, favoritesFirst stays false.
	SortControl(const QString& settingsGroup, const SortOrder& defaultOrder, Options options, QWidget* parent = nullptr);

	// The order a SortControl constructed with these arguments starts with.
	[[nodiscard]] static SortOrder savedOrder(const QString& settingsGroup, const SortOrder& defaultOrder, Options options);

	// Keeps its address for the control's lifetime.
	[[nodiscard]] const SortOrder& order() const { return _order; }

signals:
	void changed();

private:
	void openPopover();
	void updateFace();

	const QString _settingsGroup;
	const Options _options;
	SortOrder _order;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(SortControl::Options)
