#include "UiComponents/SortControl.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

#include <functional>

namespace {

const QString SORT_BY_KEY         = "mainWindow/sortBy";
const QString SORT_DESCENDING_KEY = "mainWindow/sortDescending";
const QString FAVORITES_FIRST_KEY = "mainWindow/favoritesFirst";

// How recently the popover must have closed for a button click to read as "toggle shut" rather than a
// fresh open. Only needs to clear a same-click replay (effectively instantaneous, since it happens inside
// the same event dispatch); stays well under the gap between two deliberate, separately-aimed clicks.
constexpr int PopoverReopenGuardMs = 100;

// The popover card shown under the sort button. A frameless Qt::Popup top-level (auto-dismisses on an
// outside click) whose translucent window leaves room around an inner shadowed card. Reports the full
// ordering state through onChanged whenever any control inside it moves.
class SortPopover final : public QWidget
{
public:
	SortPopover(QWidget* parent, int sortBy, bool descending, bool favoritesFirst,
	            std::function<void(int sortBy, bool descending, bool favoritesFirst)> onChanged,
	            std::function<void()> onClosed)
		: QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), m_onChanged(std::move(onChanged)), m_onClosed(std::move(onClosed))
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
		outer->setContentsMargins(MARGIN, TOP_MARGIN, MARGIN, MARGIN);  // room for the card's shadow; top kept short so the window clears the button
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

		m_favorites = new QCheckBox(tr("Favorites first"), card);
		m_favorites->setChecked(favoritesFirst);
		m_favorites->setCursor(Qt::PointingHandCursor);   // the popover's other controls (SegmentedToggle) use it too
		connect(m_favorites, &QCheckBox::toggled, this, [this] { notify(); });
		col->addWidget(m_favorites);
	}

	void popupBelow(QWidget* anchor)
	{
		adjustSize();
		const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
		// Right-align the card under the button and tuck it just below; the margins back out the shadow gutter
		// so the card lands GAP below the button. TOP_MARGIN (<= GAP) keeps the window itself clear of the button.
		int x = topLeft.x() + anchor->width() - width() + MARGIN;
		int y = topLeft.y() + anchor->height() + GAP - TOP_MARGIN;
		if (QScreen* scr = anchor->screen())
		{
			const QRect avail = scr->availableGeometry();
			x = qBound(avail.left() - MARGIN, x, avail.right() + MARGIN - width());
		}
		move(x, y);
		show();
	}

protected:
	void closeEvent(QCloseEvent* e) override
	{
		// Fires on the click that dismisses the popup; lets the owner timestamp the close so the same click,
		// once replayed to the sort button, is read as a toggle-shut instead of reopening the popover.
		if (m_onClosed)
			m_onClosed();
		QWidget::closeEvent(e);
	}

private:
	static constexpr int MARGIN = 22;  // translucent gutter (left/right/bottom) for the drop shadow to render into
	static constexpr int GAP    = 3;   // visible gap between the button and the card
	// The top gutter is kept no taller than the gap so the frameless window sits fully below the button. A
	// taller top gutter would overlap the button, and clicks there land on inert transparent gutter -- the
	// Qt::Popup treats them as inside itself, so it neither dismisses nor reaches the button to toggle shut.
	// Costs only the faint upward reach of the shadow (which was being cast onto the button regardless).
	static constexpr int TOP_MARGIN = GAP;

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
	std::function<void()> m_onClosed;
	SegmentedToggle* m_field     = nullptr;
	SegmentedToggle* m_direction = nullptr;
	QCheckBox*       m_favorites = nullptr;
};

} // namespace

SortControl::SortControl(QWidget* parent) : QPushButton(parent)
{
	m_sortBy         = QSettings{}.value(SORT_BY_KEY, SortBy::Date).toInt();
	m_descending     = QSettings{}.value(SORT_DESCENDING_KEY, true).toBool();
	m_favoritesFirst = QSettings{}.value(FAVORITES_FIRST_KEY, false).toBool();

	setToolTip(tr("Sort order"));
	// The sort glyph's SVG carries right padding (a wider viewBox) for a gap before the text; the engine
	// renders it to whatever box we reserve, so setIconSize to that non-square 4:3 to avoid a square clamp.
	setIcon(Theme::tintedIcon(QStringLiteral(":/UI/icon_sort.svg"), QColor(QString::fromLatin1(Theme::current().InstructionText))));
	setIconSize(QSize(20, 15));
	updateFace();
	connect(this, &QPushButton::clicked, this, &SortControl::openPopover);
}

void SortControl::updateFace()
{
	setText(QStringLiteral("%1  %2").arg(m_sortBy == SortBy::Date ? tr("Date") : tr("Name"), m_descending ? "↓" : "↑"));
}

void SortControl::openPopover()
{
	// Clicking the button while the popover is open closes it (Qt::Popup dismisses on any outside press)
	// and then replays that same click to the button. Without this guard that replay would immediately
	// reopen the popover; a close that just happened means this click is the dismiss, so leave it shut.
	if (m_sincePopoverClosed.isValid() && m_sincePopoverClosed.elapsed() < PopoverReopenGuardMs)
		return;

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
		},
		[this] {
			m_sincePopoverClosed.restart();
		});

	pop->popupBelow(this);
}
