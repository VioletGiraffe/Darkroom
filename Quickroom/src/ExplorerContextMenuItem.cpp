#include "ExplorerContextMenuItem.h"

#ifdef Q_OS_WIN
#include "system/shellcontextmenu.hpp"

DISABLE_COMPILER_WARNINGS
#include <QAction>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QStyle>
#include <QTimer>
#include <QWindow>
RESTORE_COMPILER_WARNINGS

#include <cmath>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

namespace {

// Returns the item's rectangle in physical screen pixels, converted by Windows: Qt's global coordinates don't scale uniformly across screens.
RECT screenRect(const QMenu& menu, QAction& item)
{
	const QRect rect = menu.actionGeometry(&item);
	const qreal scale = menu.devicePixelRatio();
	const auto scaled = [scale](int logical) { return std::lround(logical * scale); };
	RECT native{ scaled(rect.left()), scaled(rect.top()), scaled(rect.left() + rect.width()), scaled(rect.top() + rect.height()) };
	MapWindowPoints(reinterpret_cast<HWND>(menu.winId()), HWND_DESKTOP, reinterpret_cast<POINT*>(&native), 2);
	return native;
}

// Opens the Explorer menu when the cursor rests on the item for the submenu delay, or when the item is clicked or activated from
// the keyboard; the Qt menu stays open behind it.
// Filters the clicks and keys out of the Qt menu, which would otherwise trigger the item and close.
class ExplorerMenuOpener final : public QObject
{
public:
	ExplorerMenuOpener(QMenu& menu, QAction& item, const QStringList& paths, QWidget& owner)
		: QObject{ &menu }, _menu{ menu }, _item{ item }, _owner{ owner }
	{
		_paths.reserve(static_cast<size_t>(paths.size()));
		for (const QString& path : paths)
			_paths.emplace_back(path.toStdWString());

		_dwellTimer.setSingleShot(true);
		_dwellTimer.setInterval(menu.style()->styleHint(QStyle::SH_Menu_SubMenuPopupDelay, nullptr, &menu));
		connect(&_dwellTimer, &QTimer::timeout, this, [this] {
			if (_menu.activeAction() == &_item && _menu.actionAt(_menu.mapFromGlobal(QCursor::pos())) == &_item)
				openExplorerMenu();
		});

		menu.installEventFilter(this);
	}

protected:
	bool eventFilter(QObject*, QEvent* event) override
	{
		switch (event->type())
		{
		case QEvent::MouseMove:
			trackDwell(isOverItem(*static_cast<const QMouseEvent*>(event)));
			return false;
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
			return isOverItem(*static_cast<const QMouseEvent*>(event));
		case QEvent::MouseButtonRelease:
			if (!isOverItem(*static_cast<const QMouseEvent*>(event)))
				return false;
			break;
		case QEvent::KeyPress:
			if (!isOpeningKey(*static_cast<const QKeyEvent*>(event)))
				return false;
			break;
		default:
			return false;
		}

		openExplorerMenu();
		return true;
	}

private:
	[[nodiscard]] bool isOverItem(const QMouseEvent& event) const
	{
		return _menu.actionAt(event.position().toPoint()) == &_item;
	}

	// Starts the timer on entering the item, not on every move: an Explorer menu closed with Esc stays closed until the cursor
	// leaves the item and comes back, like a native submenu.
	void trackDwell(bool overItem)
	{
		if (!overItem)
			_dwellTimer.stop();
		else if (!_cursorOverItem)
			_dwellTimer.start();
		_cursorOverItem = overItem;
	}

	// A submenu's opening key, and the keys that trigger an item
	[[nodiscard]] bool isOpeningKey(const QKeyEvent& event) const
	{
		if (_menu.activeAction() != &_item)
			return false;

		switch (event.key())
		{
		case Qt::Key_Right:
		case Qt::Key_Return:
		case Qt::Key_Enter:
			return true;
		case Qt::Key_Space:
			return _menu.style()->styleHint(QStyle::SH_Menu_SpaceActivatesItem, nullptr, &_menu) != 0;
		default:
			return false;
		}
	}

	void openExplorerMenu()
	{
		_dwellTimer.stop();
		std::function<void()> runVerb = pickShellContextMenuVerb(_paths, reinterpret_cast<HWND>(_owner.window()->winId()), screenRect(_menu, _item));
		if (!runVerb)
		{
			// The Explorer menu took the mouse capture from the Qt menu; give it back.
			if (_menu.isVisible()) // events dispatched during the Explorer menu can close the Qt menu
				_menu.windowHandle()->setMouseGrabEnabled(true);
			return;
		}

		_menu.close();
		QMetaObject::invokeMethod(&_owner, std::move(runVerb), Qt::QueuedConnection);
	}

private:
	QMenu& _menu;
	QAction& _item;
	QWidget& _owner;
	std::vector<std::filesystem::path> _paths;
	QTimer _dwellTimer;
	bool _cursorOverItem = false;
};

} // namespace

void addExplorerContextMenuItem(QMenu& menu, const QStringList& paths, QWidget& owner)
{
	// The text after a tab is right-aligned, in the shortcut column.
	QAction* item = menu.addAction(QObject::tr("Explorer context menu") + "\t▸");
	new ExplorerMenuOpener(menu, *item, paths, owner);
}
#endif
