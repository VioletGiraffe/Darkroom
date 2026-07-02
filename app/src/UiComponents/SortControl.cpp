#include "UiComponents/SortControl.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Theme.h"

#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

#include <functional>

namespace {

const QString SORT_BY_KEY         = "mainWindow/sortBy";
const QString SORT_DESCENDING_KEY = "mainWindow/sortDescending";
const QString FAVORITES_FIRST_KEY = "mainWindow/favoritesFirst";

inline QColor colorFromHex(const char* hex) { return QColor(QString::fromLatin1(hex)); }

// A small rounded checkbox painted to match the popover's bespoke look (the central QSS doesn't reach a
// checkbox indicator without a checkmark image). Box + label paint together; a click anywhere toggles.
class FavoritesCheck final : public QWidget
{
public:
	explicit FavoritesCheck(const QString& text, bool checked, QWidget* parent = nullptr)
		: QWidget(parent), m_text(text), m_checked(checked)
	{
		setCursor(Qt::PointingHandCursor);
	}

	std::function<void(bool)> onToggled;

	[[nodiscard]] bool isChecked() const { return m_checked; }

	[[nodiscard]] QSize sizeHint() const override
	{
		const QFontMetrics fm(font());
		return { BOX + GAP + fm.horizontalAdvance(m_text), qMax<int>(BOX, fm.height()) };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);

		const Theme::ThemeColors& t = Theme::current();
		const QRectF box(0, (height() - BOX) / 2.0, BOX, BOX);

		if (m_checked)
		{
			p.setPen(Qt::NoPen);
			p.setBrush(colorFromHex(t.AccentBorder));
			p.drawRoundedRect(box, Theme::CheckboxRadius, Theme::CheckboxRadius);

			QPen tick(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
			p.setPen(tick);
			QPainterPath path;
			path.moveTo(box.left() + BOX * 0.26, box.top() + BOX * 0.52);
			path.lineTo(box.left() + BOX * 0.43, box.top() + BOX * 0.68);
			path.lineTo(box.left() + BOX * 0.74, box.top() + BOX * 0.32);
			p.drawPath(path);
		}
		else
		{
			p.setPen(QPen(colorFromHex(t.BorderControl), 1));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), Theme::CheckboxRadius, Theme::CheckboxRadius);
		}

		p.setPen(palette().color(QPalette::Text));
		p.drawText(QRectF(BOX + GAP, 0, width() - BOX - GAP, height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
	}

	void mousePressEvent(QMouseEvent* event) override
	{
		if (event->button() == Qt::LeftButton)
		{
			m_checked = !m_checked;
			update();
			if (onToggled)
				onToggled(m_checked);
		}
		QWidget::mousePressEvent(event);
	}

private:
	static constexpr int BOX = 16;  // checkbox side
	static constexpr int GAP = 8;   // box-to-label spacing
	QString m_text;
	bool    m_checked;
};

// The popover card shown under the sort button. A frameless Qt::Popup top-level (auto-dismisses on an
// outside click) whose translucent window leaves room around an inner shadowed card. Reports the full
// ordering state through onChanged whenever any control inside it moves.
class SortPopover final : public QWidget
{
public:
	SortPopover(QWidget* parent, int sortBy, bool descending, bool favoritesFirst,
	            std::function<void(int sortBy, bool descending, bool favoritesFirst)> onChanged)
		: QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), m_onChanged(std::move(onChanged))
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_TranslucentBackground);

		const Theme::ThemeColors& t = Theme::current();

		auto* card = new QFrame(this);
		card->setObjectName("sortPopoverCard");
		card->setStyleSheet(QStringLiteral(
			"QFrame#sortPopoverCard { background-color: palette(window); border: 1px solid %1; border-radius: %2px; }")
			.arg(t.BorderControl).arg(Theme::PopoverRadius));

		auto* shadow = new QGraphicsDropShadowEffect(card);
		shadow->setBlurRadius(20);
		shadow->setOffset(0, 5);
		shadow->setColor(QColor(0, 0, 0, 60));
		card->setGraphicsEffect(shadow);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(MARGIN, MARGIN, MARGIN, MARGIN);  // room for the card's shadow to render
		outer->addWidget(card);

		auto* col = new QVBoxLayout(card);
		col->setContentsMargins(10, 10, 10, 10);
		col->setSpacing(6);

		col->addWidget(makeSectionLabel(tr("Sort by"), t));
		m_field = new SegmentedToggle({ tr("Name"), tr("Date") }, card);
		m_field->setCurrentIndex(sortBy);   // silent
		m_field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(m_field, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(m_field);

		col->addSpacing(3);
		col->addWidget(makeSectionLabel(tr("Direction"), t));
		m_direction = new SegmentedToggle({ tr("↑ Ascending"), tr("↓ Descending") }, card);
		m_direction->setCurrentIndex(descending ? 1 : 0);   // silent
		m_direction->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(m_direction, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(m_direction);

		auto* divider = new QFrame(card);
		divider->setFrameShape(QFrame::HLine);
		divider->setFixedHeight(1);
		divider->setStyleSheet(QStringLiteral("background-color: %1; border: none;").arg(t.BorderSubtle));
		col->addSpacing(4);
		col->addWidget(divider);
		col->addSpacing(2);

		m_favorites = new FavoritesCheck(tr("Favorites first"), favoritesFirst, card);
		m_favorites->onToggled = [this](bool) { notify(); };
		col->addWidget(m_favorites);
	}

	void popupBelow(QWidget* anchor)
	{
		adjustSize();
		const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
		// Right-align the card under the button and tuck it just below; MARGIN backs out the shadow gutter.
		int x = topLeft.x() + anchor->width() - width() + MARGIN;
		int y = topLeft.y() + anchor->height() + GAP - MARGIN;
		if (QScreen* scr = anchor->screen())
		{
			const QRect avail = scr->availableGeometry();
			x = qBound(avail.left() - MARGIN, x, avail.right() + MARGIN - width());
		}
		move(x, y);
		show();
	}

private:
	static constexpr int MARGIN = 22;  // translucent gutter around the card for the drop shadow
	static constexpr int GAP    = 3;   // visible gap between the button and the card

	static QLabel* makeSectionLabel(const QString& text, const Theme::ThemeColors& t)
	{
		auto* label = new QLabel(text);
		QFont f = label->font();
		if (f.pointSizeF() > 0)   // a pixel-sized base font reports -1; leave it alone then
			f.setPointSizeF(f.pointSizeF() - 1.0);
		label->setFont(f);
		label->setStyleSheet(QStringLiteral("color: %1;").arg(t.MutedText));
		return label;
	}

	void notify()
	{
		if (m_onChanged)
			m_onChanged(m_field->currentIndex(), m_direction->currentIndex() == 1, m_favorites->isChecked());
	}

	std::function<void(int, bool, bool)> m_onChanged;
	SegmentedToggle* m_field     = nullptr;
	SegmentedToggle* m_direction = nullptr;
	FavoritesCheck*  m_favorites = nullptr;
};

} // namespace

SortControl::SortControl(QWidget* parent) : QPushButton(parent)
{
	m_sortBy         = QSettings{}.value(SORT_BY_KEY, SortBy::Date).toInt();
	m_descending     = QSettings{}.value(SORT_DESCENDING_KEY, true).toBool();
	m_favoritesFirst = QSettings{}.value(FAVORITES_FIRST_KEY, false).toBool();

	setToolTip(tr("Sort order"));
	updateFace();
	connect(this, &QPushButton::clicked, this, &SortControl::openPopover);
}

void SortControl::updateFace()
{
	setText(QStringLiteral("%1  %2").arg(m_sortBy == SortBy::Date ? tr("Date") : tr("Name"), m_descending ? "↓" : "↑"));
}

void SortControl::openPopover()
{
	auto* pop = new SortPopover(this, m_sortBy, m_descending, m_favoritesFirst,
		[this](int sortBy, bool descending, bool favoritesFirst) {
			m_sortBy         = sortBy;
			m_descending     = descending;
			m_favoritesFirst = favoritesFirst;
			QSettings s;
			s.setValue(SORT_BY_KEY, m_sortBy);
			s.setValue(SORT_DESCENDING_KEY, m_descending);
			s.setValue(FAVORITES_FIRST_KEY, m_favoritesFirst);
			updateFace();
			emit changed();
		});
	pop->popupBelow(this);
}
