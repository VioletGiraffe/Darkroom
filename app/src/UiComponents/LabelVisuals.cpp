#include "UiComponents/LabelVisuals.h"

#include <QAction>
#include <QColor>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QWidget>

#include <utility>

QIcon LabelVisuals::checkboxIcon(Presence presence, const QColor& tint, const QWidget* context)
{
	const qreal dpr = context ? context->devicePixelRatioF() : 1.0;
	constexpr int box = 16;

	QPixmap pm(qRound(box * dpr), qRound(box * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);

	const QColor fill = tint.isValid() ? tint : QColor("#888888");

	QColor outline = context ? context->palette().color(QPalette::WindowText) : QColor(Qt::black);
	outline.setAlpha(120);
	p.setPen(QPen(outline, 1.0));
	p.setBrush(fill);
	p.drawRect(QRectF(0, 0, box, box).adjusted(1.5, 1.5, -1.5, -1.5));

	if (presence != Presence::None)
	{
		const qreal luminance = 0.299 * fill.redF() + 0.587 * fill.greenF() + 0.114 * fill.blueF();
		const QColor markColor = luminance < 0.5 ? Qt::white : Qt::black;

		if (presence == Presence::All)
		{
			QPen mark(markColor, 1.7);
			mark.setCapStyle(Qt::RoundCap);
			mark.setJoinStyle(Qt::RoundJoin);
			p.setPen(mark);
			static constexpr QPointF check[] = { { 4.5, 8.5 }, { 7.0, 11.0 }, { 11.5, 5.0 } };
			p.drawPolyline(check, 3);
		}
		else
		{
			p.setPen(Qt::NoPen);
			p.setBrush(markColor);
			p.drawRect(QRectF(4, 4, 8, 8));
		}
	}
	return QIcon(pm);
}

void LabelVisuals::buildChecklistMenu(QMenu* menu, std::vector<ChecklistRow> rows)
{
	if (rows.empty())
	{
		menu->addAction(QObject::tr("(no labels yet)"))->setEnabled(false);
		return;
	}

	for (ChecklistRow& row : rows)
	{
		QAction* action = menu->addAction(checkboxIcon(row.presence, row.color, menu), row.displayName);
		const bool addToAll = row.presence != Presence::All;
		QObject::connect(action, &QAction::triggered, menu,
			[onToggle = std::move(row.onToggle), addToAll] { onToggle(addToAll); });
	}
}
