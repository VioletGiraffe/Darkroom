#pragma once

#include <QElapsedTimer>
#include <QPushButton>

// Sort field, persisted as an int. Order doubles as the field toggle's segment order (Name | Date).
namespace SortBy { enum { Name, Date }; }

// The toolbar's single sort entry point: a chip-style button whose face shows the current field and
// direction (e.g. "Date  ↓"). Clicking opens a styled popover holding every ordering option — field
// (Name/Date), direction (ascending/descending) and a favorites-first toggle, the trio that used to be a
// combo + a direction button + a checkbox. Owns its own persistence (mainWindow/sortBy, /sortDescending,
// /favoritesFirst) and emits changed() whenever the user adjusts any of them; the host re-runs its sort
// by reading the getters below. The sort *logic* itself lives in the host, unchanged.
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

	// Time since the popover last dismissed itself, used to make a click on the button toggle the popover
	// shut instead of reopening it. See openPopover() for the reopen-race it guards against.
	QElapsedTimer _sincePopoverClosed;
};
