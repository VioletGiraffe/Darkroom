#include "UiComponents/LabelVisuals.h"

#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QWidget>

QIcon LabelVisuals::checkboxIcon(Presence presence, const QColor& tint, const QWidget* context)
{
	const qreal dpr = context ? context->devicePixelRatioF() : 1.0;
	constexpr int box = 16;   // logical px; sized to sit in the menu item's small-icon column

	QPixmap pm(qRound(box * dpr), qRound(box * dpr));
	pm.setDevicePixelRatio(dpr);   // painter then works in the 16x16 logical space, crisp on high-DPI
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);

	const QColor fill = tint.isValid() ? tint : QColor("#888888");   // unset label color -> neutral grey

	// Tinted body + hairline outline so a fill close to the menu background still reads as a box. Half-pixel
	// inset keeps the 1px stroke crisp (see the quirks doc).
	QColor outline = context ? context->palette().color(QPalette::WindowText) : QColor(Qt::black);
	outline.setAlpha(120);
	p.setPen(QPen(outline, 1.0));
	p.setBrush(fill);
	p.drawRect(QRectF(0, 0, box, box).adjusted(1.5, 1.5, -1.5, -1.5));

	if (presence != Presence::None)
	{
		// The mark is black or white - whichever contrasts the fill's luminance - so it stays visible on any tint.
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
			// 'Some' -> a filled square centered in the box, the indeterminate-checkbox convention.
			p.setPen(Qt::NoPen);
			p.setBrush(markColor);
			p.drawRect(QRectF(4, 4, 8, 8));
		}
	}
	return QIcon(pm);
}
