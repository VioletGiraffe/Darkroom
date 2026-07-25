#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

class QMenu;
class QWidget;

// Shared tri-state label checklist used by main-grid and import context menus.
namespace LabelVisuals {

enum class Presence { None, Some, All };

inline Presence presenceForCount(int haveCount, int totalCount)
{
	return haveCount == 0 ? Presence::None
	     : haveCount == totalCount ? Presence::All
	     : Presence::Some;
}

// Hand-painted because QAction and QSS cannot represent Some. context supplies palette and DPR.
QIcon checkboxIcon(Presence presence, const QColor& tint, const QWidget* context);

struct ChecklistRow {
	QString displayName;
	QColor color;
	Presence presence;
	std::function<void(bool addToAll)> onToggle;
};

// onToggle receives true unless the entire selection already has the label. Empty rows add a disabled placeholder.
void buildChecklistMenu(QMenu* menu, std::vector<ChecklistRow> rows);

}
