#include "UiComponents/LabelRowDelegate.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"
#include "Theme/Theme.h"

#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QStyle>
#include <QWidget>

namespace {

constexpr int   SWATCH_W = 14;
constexpr int   SWATCH_H = 8;
constexpr qreal SWATCH_RADIUS = 3.5;
constexpr int ICON = 16;
constexpr int GAP = 8;
constexpr int PAD_L = 10;
constexpr int PAD_R = 12;
constexpr int PAD_V = 6;
constexpr int COUNT_GAP = 16;
constexpr int MARGIN = 2;
constexpr int DIVIDER_H = 12;

}

QColor LabelRowDelegate::swatchColor(const QString& labelColor)
{
	if (!labelColor.isEmpty())
		return QColor(labelColor);
	return Theme::current().palette.textDim;
}

QSize LabelRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	if (index.data(DividerRole).toBool())
		return { 0, DIVIDER_H };

	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);

	const QString name  = index.data(Qt::DisplayRole).toString();
	const QString count = index.data(CountRole).toString();
	const int leading = index.data(AllRole).toBool() ? ICON : SWATCH_W;
	const int starW = index.data(StarRole).toBool() ? opt.fontMetrics.horizontalAdvance(QStringLiteral("★")) + GAP : 0;
	const int countW = count.isEmpty() ? 0 : COUNT_GAP + opt.fontMetrics.horizontalAdvance(count);
	const int width = PAD_L + leading + GAP + starW + opt.fontMetrics.horizontalAdvance(name) + countW + PAD_R;
	return { width, qMax(leading, opt.fontMetrics.height()) + 2 * PAD_V };
}

void LabelRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);

	p->save();
	p->setRenderHint(QPainter::Antialiasing);
	p->setFont(opt.font);

	const QRect r = opt.rect;
	const Theme::Theme& t = Theme::current();
	const int RADIUS = t.metrics.controlRadius;

	if (index.data(DividerRole).toBool())
	{
		p->setPen(QPen(t.palette.borderSubtle, 1.0));
		const int dy = r.bottom() - 5;
		p->drawLine(r.left() + PAD_L, dy, r.right() - PAD_R, dy);
		p->restore();
		return;
	}

	const QString name   = index.data(Qt::DisplayRole).toString();
	const QString count  = index.data(CountRole).toString();
	const QColor  swatch = index.data(SwatchColorRole).value<QColor>();
	const bool    active = index.data(ActiveRole).toBool();
	const bool    hover  = opt.state & QStyle::State_MouseOver;
	const bool    isAll  = index.data(AllRole).toBool();

	const qreal cy = r.top() + r.height() / 2.0;  // exact center: QRect::center() truncates and sits visibly high in an even-height row
	const int swatchX = r.left() + PAD_L;
	const QRectF swatchRect(swatchX, cy - SWATCH_H / 2.0, SWATCH_W, SWATCH_H);

	const QRect pill = r.adjusted(MARGIN, 1, -MARGIN, -1);
	if (active)
	{
		constexpr int   TINT_ALPHA = 33;
		constexpr int   SPINE_W    = 4;
		constexpr int   END_INSET  = 4;
		constexpr qreal CAP_RADIUS = 1.0;
		constexpr qreal RING_W     = 1.5;
		constexpr int   RING_ALPHA = 60;

		const QColor accent = swatch.isValid() ? swatch : t.palette.accent;
		QColor tint = accent;
		tint.setAlpha(TINT_ALPHA);
		p->setPen(Qt::NoPen);
		p->setBrush(tint);
		p->drawRoundedRect(pill, RADIUS, RADIUS);

		if (!isAll)
		{
			const auto isRowActive = [&index](int row) {
				const QModelIndex sibling = index.siblingAtRow(row);
				return sibling.isValid() && sibling.data(ActiveRole).toBool();
			};
			const bool joinAbove = isRowActive(index.row() - 1);
			const bool joinBelow = isRowActive(index.row() + 1);

			// Extend joined spines across the row boundary so adjacent active rows form one run.
			const qreal cx      = swatchX + SWATCH_W / 2.0;
			const qreal topY    = joinAbove ? r.top() - CAP_RADIUS : pill.top() + END_INSET;
			const qreal bottomY = joinBelow ? r.top() + r.height() + CAP_RADIUS : pill.top() + pill.height() - END_INSET;
			p->setBrush(accent);
			p->drawRoundedRect(QRectF(cx - SPINE_W / 2.0, topY, SPINE_W, bottomY - topY), CAP_RADIUS, CAP_RADIUS);

			const QRectF ringRect = swatchRect.adjusted(-RING_W, -RING_W, RING_W, RING_W);
			const qreal ringRadius = SWATCH_RADIUS + RING_W;
			p->setBrush(t.palette.windowBg);
			p->drawRoundedRect(ringRect, ringRadius, ringRadius);
			QColor halo = accent;
			halo.setAlpha(RING_ALPHA);
			p->setBrush(halo);
			p->drawRoundedRect(ringRect, ringRadius, ringRadius);
		}
	}

	if (hover)
	{
		QColor line = opt.palette.color(QPalette::Text);
		line.setAlpha(120);
		QPen pen(line);
		pen.setStyle(Qt::DashLine);
		p->setPen(pen);
		p->setBrush(Qt::NoBrush);
		p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);
	}

	if (isAll)
		p->drawPixmap(QPointF(swatchX, cy - ICON / 2.0), allRowIcon(t.palette.textDim, iconDpr(opt)));
	else if (swatch.isValid())
	{
		p->setPen(Qt::NoPen);
		p->setBrush(swatch);
		p->drawRoundedRect(swatchRect, SWATCH_RADIUS, SWATCH_RADIUS);
	}

	int nameX = swatchX + (isAll ? ICON : SWATCH_W) + GAP;
	if (index.data(StarRole).toBool())
	{
		const QString star  = QStringLiteral("★");
		const int     starW = opt.fontMetrics.horizontalAdvance(star);
		p->setPen(t.starActive);
		p->drawText(QRect(nameX, r.top(), starW, r.height()), Qt::AlignLeft | Qt::AlignVCenter, star);
		nameX += starW + GAP;
	}

	const int   countW = opt.fontMetrics.horizontalAdvance(count);
	const QRect countRect(r.right() - PAD_R - countW, r.top(), countW, r.height());
	p->setPen(t.palette.textDim);
	p->drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, count);

	const QRect nameRect(nameX, r.top(), countRect.left() - (count.isEmpty() ? 0 : COUNT_GAP) - nameX, r.height());
	p->setPen(opt.palette.color(QPalette::Text));
	p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, opt.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width()));

	p->restore();
}

qreal LabelRowDelegate::iconDpr(const QStyleOptionViewItem& option)
{
	return option.widget ? option.widget->devicePixelRatioF() : 1.0;
}

// Cache SVG rasterization by tint and DPR.
const QPixmap& LabelRowDelegate::allRowIcon(const QColor& color, qreal dpr) const
{
	if (_allIcon.isNull() || _allIconColor != color || !qFuzzyCompare(_allIconDpr, dpr))
	{
		_allIcon = tintedSvgPixmap(QStringLiteral(":/UI/icon_stack.svg"), color, QSize(ICON, ICON), dpr);
		_allIconColor = color;
		_allIconDpr = dpr;
	}
	return _allIcon;
}
