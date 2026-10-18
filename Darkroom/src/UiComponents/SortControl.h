#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QElapsedTimer>
#include <QPushButton>
RESTORE_COMPILER_WARNINGS

// Persisted values also define the field-toggle segment order.
namespace SortBy { enum { Name, Date }; }

// Persistent sort-field, direction, and favorites-first popover. The host performs sorting from the getters.
class SortControl final : public QPushButton
{
	Q_OBJECT
public:
	explicit SortControl(QWidget* parent = nullptr);

	[[nodiscard]] int  sortBy() const { return _sortBy; }
	[[nodiscard]] bool descending() const { return _descending; }
	[[nodiscard]] bool favoritesFirst() const { return _favoritesFirst; }

signals:
	void changed();

private:
	void openPopover();
	void updateFace();

	int  _sortBy = SortBy::Name;
	bool _descending = false;
	bool _favoritesFirst = false;

	// Distinguishes the replayed dismissing click from a later request to reopen.
	QElapsedTimer _sincePopoverClosed;
};
