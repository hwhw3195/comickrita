/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VWINDOW_TEST_UTILS_H
#define VWINDOW_TEST_UTILS_H

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMimeData>
#include <QString>
#include <QTest>
#include <QVector>
#include <QtEndian>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <actions/KisPasteActionFactories.h>
#include <kis_image.h>

#include "VStrokeRenderCache.h"

/**
 * What the vstroke tests that open a real Krita window share: the document
 * and window set-up, waits, and pasting SVG. The one place to change how
 * test windows are shown (e.g. to keep them from popping up).
 */
namespace VWindowTest
{

/// a document holding \p image, known to KisPart
inline KisDocument *addDocument(KisImageSP image)
{
    KisDocument *doc = KisPart::instance()->createDocument();
    doc->setCurrentImage(image);
    KisPart::instance()->addDocument(doc);
    return doc;
}

/// a main window showing \p doc in a new view, which becomes the current one
inline KisView *openWindow(KisDocument *doc)
{
    KisMainWindow *window = KisPart::instance()->createMainWindow();
    KisViewManager *viewManager = window->viewManager();
    KisView *view = KisPart::instance()->createView(doc, viewManager, window);
    window->showView(view);
    viewManager->setCurrentView(view);
    return view;
}

/// runs the event loop for about \p ms
inline void settle(int ms)
{
    for (int i = 0; i < ms / 20; i++) QTest::qWait(20);
}

/// runs the event loop for about \p ms, then waits for \p image
inline void settle(KisImageSP image, int ms)
{
    settle(ms);
    image->waitForDone();
}

/// runs the event loop until no brush stroke has rendered for 500 ms (or \p timeoutMs)
inline void waitForRenders(int timeoutMs = 120000)
{
    QElapsedTimer total;
    total.start();
    int last = -1;
    QElapsedTimer quiet;
    quiet.start();
    while (total.elapsed() < timeoutMs) {
        QTest::qWait(50);
        const int now = VStrokeRenderCache::totalRenders();
        if (now != last) {
            last = now;
            quiet.restart();
        } else if (quiet.elapsed() > 500) {
            break;
        }
    }
}

/// waitForRenders(), then waits for \p image
inline void waitForRenders(KisImageSP image, int timeoutMs = 120000)
{
    waitForRenders(timeoutMs);
    image->waitForDone();
}

/// \p values as the base64 of their little-endian bytes, as freehand curves are saved
template <typename T>
QString encode(const QVector<T> &values)
{
    QByteArray bytes(values.size() * int(sizeof(T)), Qt::Uninitialized);
    for (int i = 0; i < values.size(); i++) qToLittleEndian<T>(values[i], bytes.data() + i * sizeof(T));
    return QString::fromLatin1(bytes.toBase64());
}

/// pastes \p svg through the clipboard, as Edit > Paste does
inline void paste(const QByteArray &svg, KisViewManager *viewManager)
{
    QMimeData *data = new QMimeData();
    data->setData("image/svg+xml", svg);
    QApplication::clipboard()->setMimeData(data);
    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
    viewManager->image()->waitForDone();
    QTest::qWait(50);
}

}

#endif // VWINDOW_TEST_UTILS_H
