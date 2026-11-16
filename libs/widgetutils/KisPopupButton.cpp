/*
 *  SPDX-FileCopyrightText: 2007 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPopupButton.h"

#include <QPointer>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QScreen>
#include <QStyleOption>
#include <QStylePainter>
#include <QWindow>
#include <QMenu>
#include <QScrollArea>
#include <QGuiApplication>
#include <QTimer>

#include "kis_global.h"
#include <kis_debug.h>


/// [custom] Holds a popup whose smallest size doesn't fit the screen; asks for the popup's own size
class KisPopupButtonScrollArea : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;

    QSize sizeHint() const override
    {
        const QSize frame(2 * frameWidth(), 2 * frameWidth());
        return widget() ? widget()->sizeHint() + frame : QScrollArea::sizeHint();
    }
};

class KisPopupButtonFrame : public QFrame
{
    QHBoxLayout* frameLayout {0};
    QScrollArea *scrollArea {0};
    // [custom] a detached frame's share of its screen, kept when it moves to another screen
    QPointer<QScreen> shareScreen;
    QSizeF screenShare;
    bool keepingShare {false};

public:
    KisPopupButtonFrame(QWidget *parent, bool detach, bool popupIsMenu)
        : QFrame(parent)
    {
        setObjectName("KisPopupButtonFrame");
        setProperty("_kis_excludeFromLayoutThumbnail", true);
        frameLayout = new QHBoxLayout(this);
        frameLayout->setContentsMargins(0, 0, 0, 0);

        setDetached(detach, popupIsMenu);
    }

    void setDetached(bool detach, bool popupIsMenu)
    {
#if defined Q_OS_ANDROID || defined Q_OS_MACOS
        // for some reason when calling destroy() the platform window isn't
        // hidden first, this corrupts state of the window stack
        hide();
#endif

        // Need to destroy the platform window before changing window flags
        // so that Qt knows to actually apply the new flags...
        // At least on Windows, not doing this may result in weird window drop
        // shadows.
        destroy();

        if (detach) {
            setWindowFlags(Qt::Dialog);
            setFrameStyle(QFrame::NoFrame);        
        }
        else {
            setWindowFlags(Qt::Popup);
            // Menus already have a frame, don't need another.
            if (popupIsMenu) {
                setFrameStyle(QFrame::NoFrame);
            } else {
                setFrameStyle(QFrame::Box | QFrame::Plain);
            }
        }

        updateGeometry();
    }

    // [custom] the popup is never larger than the screen it is on: a popup
    // that cannot shrink that far gets scroll bars. Without this, a detached
    // popup moved to a smaller screen kept its size, with its edges out of reach.

    /// The size the popup takes when shown
    QSize popupSize(const QWidget *popupWidget) const
    {
        // in a scroll area the popup keeps its smallest size, however small the frame is
        return scrollArea ? size() : popupWidget->size();
    }

    /// The part of @p screenRect a window's contents may cover (its title bar and borders stay on the screen)
    QRect usableRect(const QRect &screenRect) const
    {
        const QWindow *handle = windowHandle();
        return handle ? screenRect.marginsRemoved(handle->frameMargins()) : screenRect;
    }

    /// @p wanted, made to fit @p screenRect and moved into it
    QRect fittedToScreen(QRect wanted, const QRect &screenRect, QWidget *popupWidget)
    {
        const QRect usable = usableRect(screenRect);
        // the layout would grow the frame to the popup's smallest size
        const QSize needed = scrollArea ? wanted.size() : wanted.size().expandedTo(minimumSizeHint());
        if (needed.width() <= usable.width() && needed.height() <= usable.height()) {
            return kisEnsureInRect(wanted, screenRect);
        }
        makeScrollable(popupWidget);
        wanted.setSize(needed.boundedTo(usable.size()));
        return kisEnsureInRect(wanted, usable);
    }

    /// Shrinks the shown frame to the screen it is on now. It is not moved: the user may be dragging it
    void fitToCurrentScreen()
    {
        QScreen *screen = currentScreen();
        if (!screen || !isVisible()) return;
        const QSize usable = usableRect(screen->availableGeometry()).size();
        if (width() <= usable.width() && height() <= usable.height()) return;

        makeScrollable(currentPopupWidget());
        resize(size().boundedTo(usable));
    }

protected:
    /// The screen under the frame's center: right after a geometry change the window's own may not be updated yet
    QScreen *currentScreen() const
    {
        QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen && windowHandle()) screen = windowHandle()->screen();
        return screen;
    }

    QWidget *currentPopupWidget() const
    {
        return scrollArea ? scrollArea->widget()
                          : (frameLayout->count() ? frameLayout->itemAt(0)->widget() : nullptr);
    }

    void showEvent(QShowEvent *event) override
    {
        QFrame::showEvent(event);
        // the platform window is a new one after every setDetached()
        if (windowHandle()) {
            connect(windowHandle(), &QWindow::screenChanged, this, &KisPopupButtonFrame::slotScreenChanged,
                    Qt::UniqueConnection);
        }
        // The popup's own showEvent ran before this one and may have restored a
        // saved geometry (the brush editor does), on any screen and of any size:
        // fit that, on the screen it is on, and move it into that screen.
        QScreen *screen = currentScreen();
        if (!screen) return;
        const QRect fitted = fittedToScreen(geometry(), screen->availableGeometry(), currentPopupWidget());
        if (fitted != geometry()) {
            setGeometry(fitted);
        }
        // the share starts over on the screen it opens on
        shareScreen = nullptr;
        rememberScreenShare();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QFrame::resizeEvent(event);
        // e.g. the brush editor widens its window when its side panels open;
        // not from inside the resize itself
        if (isVisible()) {
            QTimer::singleShot(0, this, &KisPopupButtonFrame::fitToCurrentScreen);
        }
        rememberScreenShare();
    }

    /// Records which part of its screen the frame covers: a resize by the user, or by the popup
    void rememberScreenShare()
    {
        if (keepingShare || !isVisible()) return;
        QScreen *screen = currentScreen();
        if (!screen) return;
        // Qt's own resize of a window moved to a screen of another scale can come
        // before the screen change: that one is not the user's, so it is not kept
        if (shareScreen && screen != shareScreen) return;
        const QSize usable = usableRect(screen->availableGeometry()).size();
        if (usable.isEmpty()) return;
        shareScreen = screen;
        screenShare = QSizeF(qreal(width()) / usable.width(), qreal(height()) / usable.height());
    }

    /// A detached frame keeps its share of the screen: half the width of one screen is half the width of the next
    void slotScreenChanged(QScreen *newScreen)
    {
        if (windowType() == Qt::Dialog && isVisible() && newScreen && shareScreen && newScreen != shareScreen
            && !screenShare.isEmpty()) {
            const QSize usable = usableRect(newScreen->availableGeometry()).size();
            const QSize target = QSize(qRound(screenShare.width() * usable.width()),
                                       qRound(screenShare.height() * usable.height()))
                                     .boundedTo(usable);
            keepingShare = true;
            // a popup that cannot be that small scrolls rather than taking a larger share
            const QSize smallest = scrollArea ? QSize() : minimumSizeHint();
            if (smallest.width() > target.width() || smallest.height() > target.height()) {
                makeScrollable(currentPopupWidget());
            }
            // not moved: the user may be dragging it
            resize(target);
            shareScreen = newScreen;
            // again after the events in flight: Qt resizes a window itself after a
            // move to a screen of another scale
            QTimer::singleShot(0, this, [this, target] {
                if (isVisible() && size() != target) {
                    resize(target);
                }
                keepingShare = false;
                fitToCurrentScreen();
            });
            return;
        }
        shareScreen = newScreen;
        fitToCurrentScreenLater();
    }

    /// Now and once more after the events in flight: on Windows a move to a
    /// screen of another scale is followed by Qt's own resize of the window
    void fitToCurrentScreenLater()
    {
        fitToCurrentScreen();
        QTimer::singleShot(0, this, &KisPopupButtonFrame::fitToCurrentScreen);
    }

    void makeScrollable(QWidget *popupWidget)
    {
        // a menu sizes and scrolls itself
        if (scrollArea || !popupWidget || qobject_cast<QMenu *>(popupWidget)) return;
        scrollArea = new KisPopupButtonScrollArea(this);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setFocusPolicy(Qt::NoFocus);
        scrollArea->setWidgetResizable(true);
        frameLayout->removeWidget(popupWidget);
        scrollArea->setWidget(popupWidget);
        frameLayout->addWidget(scrollArea);
        // Moving the popup hid it. Show it again now, not at the next event:
        // its showEvent may set the window's geometry (the brush editor restores
        // the one it saved), and the caller's fitted geometry must come after that.
        if (isVisible()) {
            scrollArea->show();
        }
        // now, not at the next event: the frame's smallest size is still the popup's
        frameLayout->invalidate();
        frameLayout->activate();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Cancel)) {
            event->accept();
            hide();
        } else {
            QFrame::keyPressEvent(event);
        }
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::Close) {
            e->ignore();
            hide();
            return true;
        }
        return QFrame::event(e);
    }
};


struct KisPopupButton::Private {
    Private()
        // : frameLayout(nullptr)
    {}
    QPointer<KisPopupButtonFrame> frame;
    QPointer<QWidget> popupWidget;
    bool arrowVisible { true };
    bool isPopupDetached { false };
    bool isDetachedGeometrySet { false };
};

KisPopupButton::KisPopupButton(QWidget* parent)
        : QToolButton(parent)
        , m_d(new Private)
{
    setObjectName("KisPopupButton");
    connect(this, SIGNAL(released()), SLOT(showPopupWidget()));
}

KisPopupButton::~KisPopupButton()
{
    delete m_d->frame;
    delete m_d;
}

void KisPopupButton::setPopupWidgetDetached(bool detach)
{
    m_d->isPopupDetached = detach;
    if (m_d->frame) {
        bool wasVisible = isPopupWidgetVisible();
        m_d->frame->setDetached(detach, qobject_cast<QMenu *>(m_d->popupWidget));
        if (wasVisible) {
            // Setting the window flags closes the widget, so make it visible again.
            setPopupWidgetVisible(true);
            if (detach) {
                m_d->isDetachedGeometrySet = true;
            }
            adjustPosition();
        }
    }
}

void KisPopupButton::setPopupWidget(QWidget* widget)
{
    if (widget) {
        delete m_d->frame;

        // Bit wonky to assign a popup menu to a popup widget, so they need
        // extra coddling to make them work properly.
        QMenu *menu = qobject_cast<QMenu *>(widget);
        m_d->frame = new KisPopupButtonFrame(this->window(), m_d->isPopupDetached, menu);
        m_d->frame->setWindowTitle(widget->windowTitle());

        m_d->popupWidget = widget;

        m_d->frame->layout()->addWidget(m_d->popupWidget);

        if (menu) {
            // The menu may decide to hide itself in response to user input.
            // Compensate for that by catching the situation where we'd hide
            // the menu while the frame is still up and show it again.
            connect(menu, &QMenu::aboutToHide, this, &KisPopupButton::slotMenuAboutToHide);
            // If the menu still hides itself somehow, make it show up again.
            connect(this, &KisPopupButton::sigPopupWidgetShown, menu, &QMenu::show);
        }
    }
}

void KisPopupButton::setPopupWidgetWidth(int w)
{
    m_d->frame->resize(w, m_d->frame->height());
}

void KisPopupButton::showPopupWidget()
{
    if (m_d->popupWidget && !m_d->frame->isVisible()) {
        setPopupWidgetVisible(true);
        if (!m_d->isPopupDetached) {
            adjustPosition();
        }
    } else {
        hidePopupWidget();
    }
}

void KisPopupButton::hidePopupWidget()
{
    setPopupWidgetVisible(false);
}

void KisPopupButton::setPopupWidgetVisible(bool visible)
{
    if (m_d->popupWidget) {
        if (visible) {
            if (m_d->isPopupDetached) {
                // Force layout sizing before positioning
                m_d->popupWidget->adjustSize();
                m_d->frame->adjustSize();
                adjustPosition();
            }
            m_d->frame->raise();
            m_d->frame->show();
            m_d->frame->activateWindow();
            Q_EMIT sigPopupWidgetShown();
        } else {
            m_d->frame->setVisible(false);
        }
    }
}

bool KisPopupButton::isPopupWidgetVisible()
{
    return m_d->popupWidget && m_d->frame->isVisible();
}

void KisPopupButton::paintEvent ( QPaintEvent * event  )
{
    QToolButton::paintEvent(event);
    if (m_d->arrowVisible) {
        paintPopupArrow();
    }
}

void KisPopupButton::paintPopupArrow()
{
    QStylePainter p(this);
    QStyleOption option;
    option.rect = QRect(rect().right() - 15, rect().bottom() - 15, 14, 14);
    option.palette = palette();
    option.palette.setBrush(QPalette::ButtonText, Qt::black); // Force color to black
    option.state = QStyle::State_Enabled;
    p.setBrush(Qt::black); // work around some theme that don't use QPalette::ButtonText like they  should, but instead the QPainter brushes and pen
    p.setPen(Qt::black);
    p.drawPrimitive(QStyle::PE_IndicatorArrowDown, option);
}

void KisPopupButton::adjustPosition()
{
    QScreen *currentScreen = nullptr;
    auto getCurrentScreen = [this, &currentScreen] {
        if (!currentScreen) {
            QWindow *mainWinHandle = window()->windowHandle();
            if (mainWinHandle) {
                currentScreen = mainWinHandle->screen();
            } else {
                currentScreen = QApplication::primaryScreen();
            }
        }
        return currentScreen;
    };

    // If popup is not detached, or if its detached geometry hasn't been set,
    // we first move the popup to the "current" screen.
    if (!m_d->isPopupDetached || !m_d->isDetachedGeometrySet) {
        QWindow *winHandle = m_d->frame->windowHandle();
        if (winHandle) {
            winHandle->setScreen(getCurrentScreen());
        }
    }

    // Attach to the button if it's visible, else attach to the cursor.
    QPoint pos = this->isVisible() ? this->mapToGlobal(QPoint(0, this->size().height())) : QCursor().pos();
    QSize popSize = m_d->frame->popupSize(m_d->popupWidget); // [custom]
    QRect popupRect(pos, popSize);

    // Get the available geometry of the screen which contains the popup.
    QScreen *screen = [this, &getCurrentScreen]() {
        QWindow *winHandle = m_d->frame->windowHandle();
        if (winHandle && winHandle->screen()) {
            return winHandle->screen();
        }
        return getCurrentScreen();
    }();
    QRect screenRect = screen->availableGeometry();
    if (m_d->isPopupDetached) {
        if (m_d->isDetachedGeometrySet) {
            popupRect.moveTo(m_d->frame->geometry().topLeft());
        } else {
            popupRect.moveTo(this->window()->geometry().center() - QRect(QPoint(0, 0), popSize).center());
            m_d->isDetachedGeometrySet = true;
        }
    }
    popupRect = m_d->frame->fittedToScreen(popupRect, screenRect, m_d->popupWidget); // [custom] was kisEnsureInRect()

    m_d->frame->setGeometry(popupRect);
}

void KisPopupButton::setArrowVisible (bool v)
{
    if (v) {
        m_d->arrowVisible = true;
    } else {
        m_d->arrowVisible = false;
    }
}

void KisPopupButton::slotMenuAboutToHide()
{
    // Menus will hide themselves when clicking on one of their separators,
    // which just ends up with a blank popup frame. Fight back against this by
    // showing the menu again if the frame is still up when the menu disappears.
    if (m_d->popupWidget && m_d->frame->isVisible()) {
        m_d->popupWidget->show();
    }
}
