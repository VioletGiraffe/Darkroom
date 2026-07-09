#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

class QMenu;
class QWidget;

// Shared visuals for the label checklists in the item context menus (main grid and the Import dialog). Both menus
// render an identical per-label row, so the indicator lives here rather than being duplicated per call site.
namespace LabelVisuals {

// How much of a multi-selection carries a given label - drives the tri-state indicator and the toggle direction.
enum class Presence { None, Some, All };

inline Presence presenceForCount(int haveCount, int totalCount)
{
	return haveCount == 0 ? Presence::None
	     : haveCount == totalCount ? Presence::All
	     : Presence::Some;
}

// The Labels-menu row indicator: a square checkbox filled with the label's own color, showing a checkmark when
// the whole selection has the label, a dash when only some do, and an empty tinted box when none do. The mark is
// drawn in whichever of black/white contrasts the tint, so it reads on any color. QAction's checkmark is
// boolean-only and QSS can't paint a check or dash (see docs/tips/qt-styling-system-quirks.md), so the whole
// indicator is hand-painted into the action's icon. `context` supplies the outline color (its palette) and the
// device pixel ratio to render crisply.
QIcon checkboxIcon(Presence presence, const QColor& tint, const QWidget* context);

// One row of the Labels checklist: the label's display name and tint color, how much of the current selection
// carries it (drives the tri-state indicator and the toggle direction), and what a click does. onToggle(addToAll)
// runs with addToAll == true unless the whole selection already has the label - the caller decides what "add to
// all" / "strip from all" mean (mutating the Catalog, or the Import dialog's staged state).
struct ChecklistRow {
	QString displayName;
	QColor color;
	Presence presence;
	std::function<void(bool addToAll)> onToggle;
};

// Fills `menu` with the shared per-label checklist (tinted tri-state checkbox + name), wiring each row's trigger
// to its onToggle. Empty `rows` yields a single disabled "(no labels yet)" entry. Shared by the main grid's and
// the Import dialog's item context menus so the two look and behave identically.
void buildChecklistMenu(QMenu* menu, std::vector<ChecklistRow> rows);

}
