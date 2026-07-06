#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>

namespace {
constexpr int BORDER   = 1;
constexpr int SEG_HPAD = 12;  // horizontal padding inside each segment
constexpr int SEG_VPAD = 4;   // vertical padding inside each segment
constexpr int RADIUS   = Theme::ControlRadius;   // outer pill corner radius
constexpr int INSET    = 2;   // gap from a segment's edge to its selected/hover fill

inline QColor colorFromHex(const char* hex) { return QColor(QString::fromLatin1(hex)); }
}

SegmentedToggle::SegmentedToggle(const QStringList& segments, QWidget* parent)
	: QWidget(parent), m_segments(segments)
{
	setMouseTracking(true);                 // hover repaint without a button held
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void SegmentedToggle::setCurrentIndex(int index)
{
	index = qBound(0, index, static_cast<int>(m_segments.size()) - 1);
	if (index == m_current)
		return;
	m_current = index;
	update();
}

QSize SegmentedToggle::sizeHint() const
{
	const QFontMetrics fm(font());
	int seg = 0;
	for (const QString& s : m_segments)
		seg = qMax(seg, fm.horizontalAdvance(s));
	seg += 2 * SEG_HPAD;
	return { static_cast<int>(m_segments.size()) * seg + 2 * BORDER, fm.height() + 2 * SEG_VPAD + 2 * BORDER };
}

int SegmentedToggle::segmentAt(const QPoint& pos) const
{
	const int n = static_cast<int>(m_segments.size());
	if (n == 0)
		return -1;
	const double segW = (width() - 2.0 * BORDER) / n;
	return qBound(0, static_cast<int>((pos.x() - BORDER) / segW), n - 1);
}

void SegmentedToggle::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const Theme::ThemeColors& t = Theme::current();
	const QColor border  = colorFromHex(t.BorderStrong);
	const QColor textCol = palette().color(QPalette::Text);

	p.setPen(QPen(border, 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);

	const int    n    = static_cast<int>(m_segments.size());
	const double segW = (width() - 2.0 * BORDER) / n;
	for (int i = 0; i < n; ++i)
	{
		const QRectF seg(BORDER + i * segW, BORDER, segW, height() - 2.0 * BORDER);

		if (i > 0)   // hairline separator between adjacent segments
		{
			QColor sep = border;
			sep.setAlpha(140);
			p.setPen(QPen(sep, 1));
			p.drawLine(QPointF(seg.left(), seg.top() + 3), QPointF(seg.left(), seg.bottom() - 3));
		}

		const bool selected = (i == m_current);
		const bool hovered  = (i == m_hovered);
		if (selected)
		{
			p.setPen(Qt::NoPen);
			p.setBrush(colorFromHex(t.AccentBg));   // soft accent surface (the mockup's "info" fill)
			p.drawRoundedRect(seg.adjusted(INSET, INSET, -INSET, -INSET), RADIUS - INSET, RADIUS - INSET);
		}
		else if (hovered)
		{
			QColor fill = textCol;
			fill.setAlpha(14);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(seg.adjusted(INSET, INSET, -INSET, -INSET), RADIUS - INSET, RADIUS - INSET);
		}

		p.setPen(selected ? colorFromHex(t.AccentText) : textCol);   // accent text on the selected fill, normal text on the rest
		p.drawText(seg, Qt::AlignCenter, m_segments[i]);
	}
}

void SegmentedToggle::mousePressEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
	{
		if (m_segments.size() == 2)
		{
			// Clicking anywhere toggles the switch - intentional
			m_current = m_current == 0 ? 1 : 0;
		}
		else
		{
			const int clickedSegment = segmentAt(event->pos());
			if (clickedSegment == -1 || clickedSegment == m_current)
				return;

			m_current = clickedSegment;
		}

		update();
		emit currentChanged(m_current);
	}
	QWidget::mousePressEvent(event);
}

void SegmentedToggle::mouseMoveEvent(QMouseEvent* event)
{
	const int i = segmentAt(event->pos());
	if (i != m_hovered)
	{
		m_hovered = i;
		update();
	}
	QWidget::mouseMoveEvent(event);
}

void SegmentedToggle::leaveEvent(QEvent* event)
{
	if (m_hovered != -1)
	{
		m_hovered = -1;
		update();
	}
	QWidget::leaveEvent(event);
}
