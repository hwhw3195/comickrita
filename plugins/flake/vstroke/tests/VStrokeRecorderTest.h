/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_RECORDER_TEST_H
#define VSTROKE_RECORDER_TEST_H

#include <QObject>

/**
 * The pen tool's live stroke must be what a re-render gives, so it can
 * become the stroke's rendering without rendering twice.
 */
class VStrokeRecorderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    void testLiveStrokeMatchesReRender_data();
    void testLiveStrokeMatchesReRender();
    void testCancel();
};

#endif // VSTROKE_RECORDER_TEST_H
