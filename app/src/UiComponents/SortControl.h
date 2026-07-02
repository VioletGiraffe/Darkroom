#pragma once

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

	[[nodiscard]] int  sortBy() const { return m_sortBy; }
	[[nodiscard]] bool descending() const { return m_descending; }
	[[nodiscard]] bool favoritesFirst() const { return m_favoritesFirst; }

signals:
	void changed();

private:
	void openPopover();
	void updateFace();

	int  m_sortBy = SortBy::Name;
	bool m_descending = false;
	bool m_favoritesFirst = false;
};
