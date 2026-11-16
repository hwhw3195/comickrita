/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPopupButtonTest.h"

#include <QFrame>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>

#include <KisPopupButton.h>

namespace {
/// Like the brush editor: saves its window's geometry when hidden and restores it when shown, whatever the screen
class RestoringPanel : public QWidget
{
public:
    QRect restored;

protected:
    void showEvent(QShowEvent *) override
    {
        window()->setGeometry(restored);
    }
    void hideEvent(QHideEvent *) override
    {
        restored = window()->geometry();
    }
};

QFrame *frameOf(KisPopupButton &button)
{
    return button.window()->findChild<QFrame *>("KisPopupButtonFrame");
}
}

void KisPopupButtonTest::testPopupFitsScreen_data()
{
    QTest::addColumn<bool>("detached");
    QTest::newRow("attached") << false;
    QTest::newRow("detached") << true;
}

void KisPopupButtonTest::testPopupFitsScreen()
{
    QFETCH(bool, detached);
    const QSize screen = QGuiApplication::primaryScreen()->availableGeometry().size();

    KisPopupButton button(nullptr);
    button.show();
    // a panel that cannot be made as small as the screen
    QWidget *panel = new QWidget();
    panel->setMinimumSize(screen.width() + 500, screen.height() + 400);
    button.setPopupWidget(panel);
    button.setPopupWidgetDetached(detached);

    // twice: the second time the popup has the size its layout gave it
    for (int i = 0; i < 2; i++) {
        button.showPopupWidget();
        QTest::qWait(50);
        QFrame *frame = frameOf(button);
        QVERIFY(frame);
        QVERIFY(frame->isVisible());
        QVERIFY2(frame->width() <= screen.width() && frame->height() <= screen.height(),
                 qPrintable(QString("%1x%2 on a %3x%4 screen")
                                .arg(frame->width()).arg(frame->height()).arg(screen.width()).arg(screen.height())));
        // the whole panel is still there, behind scroll bars
        QVERIFY(frame->findChild<QScrollArea *>());
        QCOMPARE(panel->size(), panel->minimumSize());
        button.hidePopupWidget();
        QTest::qWait(50);
    }
}

void KisPopupButtonTest::testSmallPopupKeepsItsSize()
{
    KisPopupButton button(nullptr);
    button.show();
    QWidget *panel = new QWidget();
    panel->setMinimumSize(200, 150);
    panel->resize(200, 150);
    button.setPopupWidget(panel);

    button.showPopupWidget();
    QTest::qWait(50);
    QFrame *frame = frameOf(button);
    QVERIFY(frame);
    // no scroll area for a popup that fits: nothing changes for it
    QVERIFY(!frame->findChild<QScrollArea *>());
    QVERIFY(frame->width() >= 200 && frame->height() >= 150);
    QCOMPARE(panel->parentWidget(), frame);
}

void KisPopupButtonTest::testRestoredGeometryFitsScreen()
{
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();

    KisPopupButton button(nullptr);
    button.show();
    RestoringPanel *panel = new RestoringPanel();
    panel->setMinimumSize(screen.width() / 2, screen.height() / 2);
    // saved on a larger screen
    panel->restored = QRect(screen.topLeft() + QPoint(100, 50), screen.size() + QSize(600, 500));
    button.setPopupWidget(panel);
    button.setPopupWidgetDetached(true);

    for (int i = 0; i < 2; i++) {
        button.showPopupWidget();
        QTest::qWait(50);
        QFrame *frame = frameOf(button);
        QVERIFY(frame);
        QVERIFY(frame->isVisible());
        QVERIFY2(screen.contains(frame->geometry()),
                 qPrintable(QString("frame %1,%2 %3x%4 on screen %5,%6 %7x%8")
                                .arg(frame->x()).arg(frame->y()).arg(frame->width()).arg(frame->height())
                                .arg(screen.x()).arg(screen.y()).arg(screen.width()).arg(screen.height())));
        // the popup's own code still reaches the popup window, not the scroll area
        QCOMPARE(panel->window(), frame);
        button.hidePopupWidget();
        QTest::qWait(50);
    }
}

SIMPLE_TEST_MAIN(KisPopupButtonTest)
