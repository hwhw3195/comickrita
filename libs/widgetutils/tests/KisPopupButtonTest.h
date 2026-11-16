/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_POPUP_BUTTON_TEST_H
#define KIS_POPUP_BUTTON_TEST_H

#include <simpletest.h>

class KisPopupButtonTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPopupFitsScreen_data();
    void testPopupFitsScreen();
    void testSmallPopupKeepsItsSize();
    void testRestoredGeometryFitsScreen();
};

#endif
