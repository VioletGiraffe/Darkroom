#include "UiComponents/LabelRowDelegate.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"

#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QStyle>
#include <QWidget>

namespace {

constexpr int   SWATCH_W = 14;        // leading color swatch size (ordinary label rows)
constexpr int   SWATCH_H = 8;
constexpr qreal SWATCH_RADIUS = 3.5;  // below full (H/2) so the swatch reads as a squircle, not a capsule
constexpr int ICON = 16; // leading glyph box (the All row's stack icon) - wider than a swatch, per the mockup
constexpr int GAP = 8;   // swatch/icon -> name gap
constexpr int PAD_L = 10;  // left breathing room
constexpr int PAD_R = 12;  // right breathing room (count -> edge)
constexpr int PAD_V = 6;   // vertical breathing room per row
constexpr int COUNT_GAP = 16;  // minimum name -> count gap
constexpr int MARGIN = 2;   // inset of the highlight pill from the row edges
constexpr int RADIUS = Theme::ControlRadius;   // highlight pill corner radius
constexpr int DIVIDER_H = 12;   // height of a separator row

inline QColor colorFromHex(const char* hex) { return QColor(QString::fromLatin1(hex)); }

}

QColor LabelRowDelegate::swatchColor(const QString& labelColor)
{
	if (!labelColor.isEmpty())
		return QColor(labelColor);
	return colorFromHex(Theme::current().MutedText);
}

QSize LabelRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	if (index.data(DividerRole).toBool())
		return { 0, DIVIDER_H };   // width 0 so the separator never drives the panel's hugged width

	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);   // pulls the per-item font (the Import dialog italicizes provisional rows)

	const QString name  = index.data(Qt::DisplayRole).toString();
	const QString count = index.data(CountRole).toString();
	const int leading = index.data(AllRole).toBool() ? ICON : SWATCH_W;
	const int starW = index.data(StarRole).toBool() ? opt.fontMetrics.horizontalAdvance(QStringLiteral("★")) + GAP : 0;
	const int countW = count.isEmpty() ? 0 : COUNT_GAP + opt.fontMetrics.horizontalAdvance(count);   // count-less rows don't reserve the gap
	const int width = PAD_L + leading + GAP + starW + opt.fontMetrics.horizontalAdvance(name) + countW + PAD_R;
	return { width, qMax(leading, opt.fontMetrics.height()) + 2 * PAD_V };
}

void LabelRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);   // pulls the per-item font (the Import dialog italicizes provisional rows)

	p->save();
	p->setRenderHint(QPainter::Antialiasing);
	p->setFont(opt.font);

	const QRect r = opt.rect;
	const Theme::ThemeColors& t = Theme::current();

	if (index.data(DividerRole).toBool())   // separator between the pinned rows and the labels
	{
		p->setPen(QPen(colorFromHex(t.BorderSubtle), 1.0));
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
	const bool    isAll  = index.data(AllRole).toBool();   // gets the stack glyph instead of a swatch

	const qreal cy = r.top() + r.height() / 2.0;  // exact center: QRect::center() truncates and sits visibly high in an even-height row
	const int swatchX = r.left() + PAD_L;
	const QRectF swatchRect(swatchX, cy - SWATCH_H / 2.0, SWATCH_W, SWATCH_H);

	// Active rows get a pill filled with a translucent tint of the label's own color (composited over the
	// panel background, so one alpha works in both themes) and a vertical spine centered on the swatch. The
	// spines of vertically adjacent active rows fuse across the row boundary into one continuous line; a
	// terminated end stops short of the pill edge. Hover is a dashed outline in a translucent text-color
	// overlay, so it reads against both the plain window and an active row's fill.
	const QRect pill = r.adjusted(MARGIN, 1, -MARGIN, -1);
	if (active)
	{
		constexpr int   TINT_ALPHA = 33;    // ~13%: enough to read as the label's color, faint enough under text
		constexpr int   SPINE_W    = 4;     // even, so it centers crisply on the even-width swatch
		constexpr int   END_INSET  = 4;     // terminated spine end -> pill edge gap
		constexpr qreal CAP_RADIUS = 1.0;   // softens the cut ends without reading as a cap shape
		constexpr qreal RING_W     = 1.5;   // faint halo thickness around the swatch (the ring band knocked out over the spine)
		constexpr int   RING_ALPHA = 60;    // ~25%: halo accent over BackgroundPrimary, ~2x the fill so the outline reads but stays quiet

		const QColor accent = swatch.isValid() ? swatch : colorFromHex(t.AccentBorder);
		QColor tint = accent;
		tint.setAlpha(TINT_ALPHA);
		p->setPen(Qt::NoPen);
		p->setBrush(tint);
		p->drawRoundedRect(pill, RADIUS, RADIUS);

		// The All row's stack icon leaves no swatch to thread the spine through, and All is only ever active
		// alone - its tint is the whole highlight.
		if (!isAll)
		{
			const auto isRowActive = [&index](int row) {
				const QModelIndex sibling = index.siblingAtRow(row);
				return sibling.isValid() && sibling.data(ActiveRole).toBool();
			};
			const bool joinAbove = isRowActive(index.row() - 1);   // the divider row is never active, so runs can't cross it
			const bool joinBelow = isRowActive(index.row() + 1);

			// Spine: one continuous bar straight through the swatch's center, drawn in a single call - the swatch and
			// its halo paint over the middle (below). A joined end overshoots the row boundary by the cap radius
			// so its rounded cap lands in the neighbor's row: covered there by the neighbor's own full-width
			// spine, or - if the view clips painting to the row - cut off flat at the boundary. Either way the
			// seam ends up flush and square, with no second squaring pass. A terminated end stops short of the
			// pill and keeps its softened cap.
			const qreal cx      = swatchX + SWATCH_W / 2.0;
			const qreal topY    = joinAbove ? r.top() - CAP_RADIUS : pill.top() + END_INSET;
			const qreal bottomY = joinBelow ? r.top() + r.height() + CAP_RADIUS : pill.top() + pill.height() - END_INSET;
			p->setBrush(accent);
			p->drawRoundedRect(QRectF(cx - SPINE_W / 2.0, topY, SPINE_W, bottomY - topY), CAP_RADIUS, CAP_RADIUS);

			// Ring: a faint halo around the swatch so it reads as a node on the line, not fused to it. Opaque
			// BackgroundPrimary (what the pill tint composites over) erases the spine within the ring band, then
			// the accent re-applied a touch stronger than the fill tints that ring. Grown from the swatch rect
			// (inflated by RING_W, radius grown to match), so nothing can misalign.
			const QRectF ringRect = swatchRect.adjusted(-RING_W, -RING_W, RING_W, RING_W);
			const qreal ringRadius = SWATCH_RADIUS + RING_W;
			p->setBrush(colorFromHex(t.BackgroundPrimary));
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
		p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);   // half-px inset keeps the 1px stroke crisp
	}

	if (isAll)
		p->drawPixmap(QPointF(swatchX, cy - ICON / 2.0), allRowIcon(colorFromHex(t.MutedText), iconDpr(opt)));
	else if (swatch.isValid())
	{
		p->setPen(Qt::NoPen);
		p->setBrush(swatch);
		p->drawRoundedRect(swatchRect, SWATCH_RADIUS, SWATCH_RADIUS);
	}

	int nameX = swatchX + (isAll ? ICON : SWATCH_W) + GAP;
	if (index.data(StarRole).toBool())   // the Best row: a gold star just right of its swatch
	{
		const QString star  = QStringLiteral("★");
		const int     starW = opt.fontMetrics.horizontalAdvance(star);
		p->setPen(colorFromHex(Theme::StarActive));
		p->drawText(QRect(nameX, r.top(), starW, r.height()), Qt::AlignLeft | Qt::AlignVCenter, star);
		nameX += starW + GAP;
	}

	// Count rides the right edge; the name fills (and elides into) the space up to it. A count-less row
	// (the Import dialog's list) gives the name the full width instead of reserving the gap.
	const int   countW = opt.fontMetrics.horizontalAdvance(count);
	const QRect countRect(r.right() - PAD_R - countW, r.top(), countW, r.height());
	p->setPen(colorFromHex(t.MutedText));
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

// The All row's stack glyph, rasterized once and cached until the tint color (a theme switch) or the DPR
// changes - so paint() doesn't re-render the SVG on every hover/scroll repaint. Mutable because paint() is const.
const QPixmap& LabelRowDelegate::allRowIcon(const QColor& color, qreal dpr) const
{
	if (m_allIcon.isNull() || m_allIconColor != color || !qFuzzyCompare(m_allIconDpr, dpr))
	{
		m_allIcon = Theme::tintedPixmap(QStringLiteral(":/UI/icon_stack.svg"), color, QSize(ICON, ICON), dpr);
		m_allIconColor = color;
		m_allIconDpr = dpr;
	}
	return m_allIcon;
}
