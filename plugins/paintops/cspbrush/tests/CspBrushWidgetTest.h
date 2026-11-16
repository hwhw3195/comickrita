/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_WIDGET_TEST_H
#define CSP_BRUSH_WIDGET_TEST_H

#include <QObject>

/**
 * The brush editor pages: loading a preset, editing it and keeping every
 * CSP setting (also those without a control).
 */
class CspBrushWidgetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testLoadDoesNotModify();
    void testEditsReachConfiguration();
    void testDynamicsEdit();
    void testTexturePage();
    void testColorJitterPage();
    void testSprayDirection();
    void testPageOrder();
    void testRandomMinimumRange();
    void testPageScreenshots();
    void testSummary();
    void testRibbonGreysOutMixing();
    void testTipCreatorFlattensLayers();
    void testTipCreatorTip();
    void testTipCreatorDialog();
    void testTipLibraryPng();
};

#endif // CSP_BRUSH_WIDGET_TEST_H
