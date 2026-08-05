#include "UiComponents/MediaItemWidget.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Theme/Theme.h"
#include "Theme/Icons.h"
#include "Utils.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>

namespace {

static constexpr int CARD_FOOTER_SPACING = 5;

class LabelDotStrip final : public QWidget {
public:
	explicit LabelDotStrip(QWidget* parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setColors(const std::vector<QColor>& colors)
	{
		_colors = colors;
		updateGeometry();
		update();
	}

	[[nodiscard]] QSize sizeHint() const override
	{
		const int n = static_cast<int>(_colors.size());
		if (n == 0)
			return { 0, 0 };
		return { n * DOT + (n - 1) * GAP, DOT };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		if (_colors.empty())
			return;

		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);

		const int y = (height() - DOT) / 2;
		int x = 0;
		for (const QColor& c : _colors)
		{
			p.setBrush(c.isValid() ? c : QColor("#888888"));
			p.drawEllipse(x, y, DOT, DOT);
			x += DOT + GAP;
		}
	}

private:
	static constexpr int DOT = 8;
	static constexpr int GAP = 4;

	std::vector<QColor> _colors;
};

class ElidedLabel final : public QLabel {
public:
	using QLabel::QLabel;

	void setFullText(const QString& text)
	{
		_full = text;
		setToolTip(text);
		updateElision();
	}

protected:
	void resizeEvent(QResizeEvent* event) override
	{
		QLabel::resizeEvent(event);
		updateElision();
	}

private:
	void updateElision()
	{
		setText(fontMetrics().elidedText(_full, Qt::ElideRight, width()));
	}

	QString _full;
};

class FramesReadyBadge final : public QWidget {
public:
	explicit FramesReadyBadge(QWidget* parent)
		: QWidget(parent), _icon(Theme::tintedIcon(QStringLiteral(":/UI/icon_grid.svg"), &Theme::ThemeColors::ReadyGreen))
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		resize(sizeHint());
	}

	[[nodiscard]] QSize sizeHint() const override { return { 2 * PAD + SIZE, 2 * PAD + SIZE }; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 90));
		p.drawRoundedRect(rect(), BACKDROP_RADIUS, BACKDROP_RADIUS);

		_icon.paint(&p, QRect(PAD, PAD, SIZE, SIZE));
	}

private:
	static constexpr int SIZE = 16;
	static constexpr int PAD = 3;
	static constexpr int BACKDROP_RADIUS = 6;
	QIcon _icon;
};

QString formatDuration(qint64 ms)
{
	const qint64 totalSeconds = ms / 1000;
	const qint64 h = totalSeconds / 3600;
	const qint64 m = (totalSeconds / 60) % 60;
	const qint64 s = totalSeconds % 60;
	return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
	             : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

class DurationBadge final : public QWidget {
public:
	explicit DurationBadge(QWidget* parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		QFont f = font();
		f.setPointSize(8);
		setFont(f);
	}

	void setText(const QString& text)
	{
		if (_text == text)
			return;
		_text = text;
		resize(sizeHint());
		update();
	}

	[[nodiscard]] QSize sizeHint() const override
	{
		const QFontMetrics fm(font());
		return { PAD_H * 2 + triangleWidth(fm) + GAP + fm.horizontalAdvance(_text), fm.height() + PAD_V * 2 };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 128));
		p.drawRoundedRect(rect(), RADIUS, RADIUS);

		const QColor fg(255, 255, 255, 245);
		const QFontMetrics fm(font());
		const double cy = height() / 2.0;
		const int triH = triangleHeight(fm);
		const int triW = triangleWidth(fm);

		QPolygonF triangle;
		triangle << QPointF(PAD_H, cy - triH / 2.0) << QPointF(PAD_H, cy + triH / 2.0) << QPointF(PAD_H + triW, cy);
		p.setBrush(fg);
		p.drawPolygon(triangle);

		const int textX = PAD_H + triW + GAP;
		p.setPen(fg);
		p.drawText(QRect(textX, 0, width() - textX - PAD_H, height()), Qt::AlignVCenter | Qt::AlignLeft, _text);
	}

private:
	static constexpr int PAD_H = 6;
	static constexpr int PAD_V = 3;
	static constexpr int GAP = 4;
	static constexpr int RADIUS = 5;

	static int triangleHeight(const QFontMetrics& fm) { return qRound(fm.ascent() * 0.62); }
	static int triangleWidth(const QFontMetrics& fm)  { return qRound(triangleHeight(fm) * 0.85); }

	QString _text;
};

}  // namespace

MediaItemWidget::MediaItemWidget(
	QSize maxImageSize, const QStringList& previewPaths, const QString& label,
	const MediaId& mediaId,
	bool inBest, std::function<void()> onToggleBest,
	std::function<void()> onDoubleClick,
	std::function<void(QPoint)> onContextMenu,
	bool dynamicSizeHint,
	bool filmStrip,
	QWidget* parent
)
	: QWidget{ parent }
	, _mediaId{ mediaId }
	, _filmStrip{ filmStrip }
	, _onDoubleClick{ std::move(onDoubleClick) }
	, _onContextMenu{ std::move(onContextMenu) }
{
	setObjectName("mediaItemCard");
	setAcceptDrops(true);

	setAttribute(Qt::WA_StyledBackground);
	setAttribute(Qt::WA_Hover);
	setContentsMargins(CardChromePerSide, CardChromePerSide, CardChromePerSide, CardChromePerSide);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(CARD_FOOTER_SPACING);

	_thumb = new ThumbnailWidget(
		previewPaths, QString(), this, maxImageSize, dynamicSizeHint, /*framed*/ false, /*filmStrip*/ _filmStrip);
	_thumb->installEventFilter(this);

	_framesReadyBadge = new FramesReadyBadge(_thumb);
	_framesReadyBadge->hide();

	_durationBadge = new DurationBadge(_thumb);
	_durationBadge->hide();

	if (_onContextMenu)
	{
		_thumb->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(_thumb, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
			// A menu action can rebuild the grid and delete this card synchronously.
			QPointer<MediaItemWidget> self(this);
			_onContextMenu(_thumb->mapToGlobal(pos));
			if (self)
				clearStuckHoverIfCursorLeft(self);
		});
	}

	layout->addWidget(_thumb, 1);

	_footer = new QWidget(this);
	_footer->setObjectName("cardFooter");
	auto* footerLayout = new QHBoxLayout(_footer);
	footerLayout->setContentsMargins(0, 0, 0, 0);
	footerLayout->setSpacing(6);

	_starButton = new QPushButton("★", _footer);
	_starButton->setObjectName("cardStar");
	_starButton->setCheckable(true);
	_starButton->setChecked(inBest);
	_starButton->setFlat(true);
	_starButton->setCursor(Qt::PointingHandCursor);
	_starButton->setFocusPolicy(Qt::NoFocus);
	_starButton->setFixedSize(18, 18);
	connect(_starButton, &QPushButton::clicked, this, [onToggleBest = std::move(onToggleBest)]() {
		onToggleBest();
	});
	footerLayout->addWidget(_starButton, 0, Qt::AlignVCenter);

	_labelDots = new LabelDotStrip(_footer);
	_labelDots->setObjectName("cardLabelDots");
	_labelDots->hide();
	footerLayout->addWidget(_labelDots, 0, Qt::AlignVCenter);

	auto* name = new ElidedLabel(_footer);
	name->setObjectName("cardName");
	name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	QFont nameFont = name->font();
	nameFont.setPointSize(9);
	name->setFont(nameFont);
	name->setFullText(label);
	_name = name;
	footerLayout->addWidget(name, 1);

	layout->addWidget(_footer, 0);
}

void MediaItemWidget::setInBest(bool inBest)
{
	_starButton->setChecked(inBest);
}

void MediaItemWidget::setLabel(const QString& label)
{
	static_cast<ElidedLabel*>(_name)->setFullText(label);
}

void MediaItemWidget::setLabelDots(const std::vector<QColor>& colors, const QString& tooltip)
{
	auto* strip = static_cast<LabelDotStrip*>(_labelDots);
	strip->setColors(colors);
	strip->setVisible(!colors.empty());
	_thumb->setToolTip(tooltip);
}

void MediaItemWidget::setFramesExtracted(bool extracted)
{
	_framesReadyBadge->setVisible(extracted);
	if (extracted)
	{
		repositionFramesReadyBadge();
		_framesReadyBadge->raise();
	}
}

void MediaItemWidget::repositionFramesReadyBadge()
{
	static constexpr int MARGIN = 4;
	const int bandOffset = _filmStrip ? ThumbnailWidget::filmStripBandHeight(_thumb->height()) : 0;
	_framesReadyBadge->move(_thumb->width() - _framesReadyBadge->width() - MARGIN, MARGIN + bandOffset);
}

void MediaItemWidget::setDuration(qint64 durationMs)
{
	const bool show = durationMs > 0;
	_durationBadge->setVisible(show);
	if (!show)
		return;

	static_cast<DurationBadge*>(_durationBadge)->setText(formatDuration(durationMs));
	repositionDurationBadge();
	_durationBadge->raise();
}

void MediaItemWidget::repositionDurationBadge()
{
	static constexpr int MARGIN = 4;
	const int bandOffset = _filmStrip ? ThumbnailWidget::filmStripBandHeight(_thumb->height()) : 0;
	_durationBadge->move(_thumb->width() - _durationBadge->width() - MARGIN,
	                      _thumb->height() - _durationBadge->height() - MARGIN - bandOffset);
}

void MediaItemWidget::setOnMiddleButtonClick(std::function<void()> onClick)
{
	_onMiddleButtonClick = std::move(onClick);
	_thumb->installEventFilter(this);
}

void MediaItemWidget::setOnMouseWheelCallback(std::function<void(int)> handler)
{
	_thumb->setOnMouseWheelCallback(std::move(handler));
}

void MediaItemWidget::setOnLabelDropped(std::function<void(const QString&)> handler)
{
	_onLabelDropped = std::move(handler);
}

void MediaItemWidget::dragEnterEvent(QDragEnterEvent* event)
{
	if (_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void MediaItemWidget::dragMoveEvent(QDragMoveEvent* event)
{
	if (_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void MediaItemWidget::dropEvent(QDropEvent* event)
{
	if (!_onLabelDropped || !event->mimeData()->hasFormat(LabelMimeType))
		return;

	const QString labelId = QString::fromUtf8(event->mimeData()->data(LabelMimeType));
	if (labelId.isEmpty())
		return;

	event->acceptProposedAction();
	_onLabelDropped(labelId);
}

QSize MediaItemWidget::sizeHint() const
{
	if (!_thumb)
		return QWidget::sizeHint();

	const QMargins m = contentsMargins();
	const QSize thumb = _thumb->sizeHint();
	const int footerHeight = _footer->sizeHint().height();
	return QSize(thumb.width() + m.left() + m.right(),
	            thumb.height() + CARD_FOOTER_SPACING + footerHeight + m.top() + m.bottom());
}

bool MediaItemWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched != _thumb)
		return QWidget::eventFilter(watched, event);

	if (event->type() == QEvent::MouseButtonDblClick)
	{
		const auto* me = static_cast<QMouseEvent*>(event);

		if (me->button() == Qt::LeftButton && _onDoubleClick)
		{
			_onDoubleClick();
			return true;
		}
	}
	else if (event->type() == QEvent::MouseButtonRelease)
	{
		const auto* me = static_cast<QMouseEvent*>(event);
		if (me->button() == Qt::MiddleButton && _onMiddleButtonClick)
		{
			_onMiddleButtonClick();
			return true;
		}
	}
	else if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
	{
		// Fixed-size thumbnails may resize while hidden and receive only Show afterward.
		repositionFramesReadyBadge();
		repositionDurationBadge();
	}

	return QWidget::eventFilter(watched, event);
}
