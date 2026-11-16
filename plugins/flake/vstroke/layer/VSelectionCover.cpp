/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VSelectionCover.h"

#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>

#include <KoInsets.h>
#include <KoShape.h>
#include <KoShapeStrokeModel.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>

#include "VCurveShape.h"

QList<KoShape*> VSelectionCover::shapesToClear(KisShapeLayer *layer, KisSelectionSP selection)
{
    QList<KoShape*> result;
    if (!layer) return result;
    if (!selection) return layer->shapes();

    KisImageSP image = layer->image();
    if (!image) return result;
    const QTransform documentToPixels = QTransform::fromScale(image->xRes(), image->yRes());

    Q_FOREACH (KoShape *shape, layer->shapes()) {
        if (coveredBySelection(shape, selection, documentToPixels)) result << shape;
    }
    return result;
}

bool VSelectionCover::coveredBySelection(KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels)
{
    const QRect selected = selection->selectedExactRect();

    // the area the shape paints, in image pixels
    QPainterPath area = shape->outline();
    // a freehand curve's outline is already all of it: its stroke() is that same width
    if (shape->stroke() && !dynamic_cast<VCurveShape*>(shape)) {
        KoInsets insets;
        shape->stroke()->strokeInsets(shape, insets);
        QPainterPathStroker stroker;
        stroker.setWidth(2 * qMax(qMax(insets.left, insets.right), qMax(insets.top, insets.bottom)));
        area.addPath(stroker.createStroke(area)); // united() is slow on long strokes
        area.setFillRule(Qt::WindingFill);
    }
    area = (shape->absoluteTransformation() * documentToPixels).map(area);

    const QRect bounds = area.boundingRect().toAlignedRect();
    if (bounds.isEmpty() || !selected.contains(bounds)) return false;

    // every pixel the shape paints must be selected
    QImage mask(bounds.size(), QImage::Format_Alpha8);
    mask.fill(0);
    QPainter gc(&mask);
    gc.translate(-bounds.topLeft());
    gc.fillPath(area, Qt::black);
    gc.end();

    QVector<quint8> pixels(bounds.width() * bounds.height());
    selection->projection()->readBytes(pixels.data(), bounds);

    for (int y = 0; y < bounds.height(); y++) {
        const quint8 *maskLine = mask.constScanLine(y);
        const quint8 *selectionLine = pixels.constData() + y * bounds.width();
        for (int x = 0; x < bounds.width(); x++) {
            if (maskLine[x] >= 128 && selectionLine[x] < 128) return false;
        }
    }
    return true;
}
