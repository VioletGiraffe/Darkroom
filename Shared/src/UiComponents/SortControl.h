#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QElapsedTimer>
#include <QPushButton>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Persisted values also define the field-toggle segment order.
namespace SortBy { enum { Name, Date }; }

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
	// The order persists under settingsGroup; defaultOrder applies until it does.
	// Without favoritesEnabled the popover has no favorites option, and favoritesFirst stays false.
	SortControl(const QString& settingsGroup, const SortOrder& defaultOrder, bool favoritesEnabled, QWidget* parent = nullptr);

	// The order a SortControl constructed with these arguments starts with.
	[[nodiscard]] static SortOrder savedOrder(const QString& settingsGroup, const SortOrder& defaultOrder, bool favoritesEnabled);

	// Keeps its address for the control's lifetime.
	[[nodiscard]] const SortOrder& order() const { return _order; }

signals:
	void changed();

private:
	void openPopover();
	void updateFace();

	const QString _settingsGroup;
	const bool _favoritesEnabled;
	SortOrder _order;

	// Distinguishes the replayed dismissing click from a later request to reopen.
	QElapsedTimer _sincePopoverClosed;
};
