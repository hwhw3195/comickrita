/*
 *  SPDX-FileCopyrightText: 2014 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_LIQUIFY_TRANSFORM_WORKER_H
#define __KIS_LIQUIFY_TRANSFORM_WORKER_H

#include <QScopedPointer>
#include <boost/operators.hpp>

#include <kritaimage_export.h>
#include <kis_types.h>

class QImage;
class QRect;
class QSize;
class QTransform;
class QDomElement;


class KRITAIMAGE_EXPORT KisLiquifyTransformWorker : boost::equality_comparable<KisLiquifyTransformWorker>
{
public:
    KisLiquifyTransformWorker(const QRect &srcBounds,
                              KoUpdater *progress,
                              int pixelPrecision = 8);

    KisLiquifyTransformWorker(const KisLiquifyTransformWorker &rhs);

    ~KisLiquifyTransformWorker();

    bool operator==(const KisLiquifyTransformWorker &other) const;
    bool isIdentity() const;


    int pointToIndex(const QPoint &cellPt);
    QSize gridSize() const;

    void translatePoints(const QPointF &base,
                         const QPointF &offset,
                         qreal sigma,
                         bool useWashMode,
                         qreal flow);

    void scalePoints(const QPointF &base,
                     qreal scale,
                     qreal sigma,
                     bool useWashMode,
                     qreal flow);

    void rotatePoints(const QPointF &base,
                      qreal angle,
                      qreal sigma,
                      bool useWashMode,
                      qreal flow);

    void undoPoints(const QPointF &base,
                    qreal amount,
                    qreal sigma);

    const QVector<QPointF>& originalPoints() const;
    QVector<QPointF>& transformedPoints();

    void run(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice);

    /**
     * The area of the destination where run() gives other pixels than it
     * gave for \p previous: the cells whose corners moved, at their old and
     * new places. Returns false when the two grids don't match (another
     * source area or precision), and run() has to redo everything.
     */
    bool changedRect(const KisLiquifyTransformWorker &previous, QRect *rect) const;

    /**
     * run() within \p rect only: clears it in \p dstDevice and paints there
     * exactly what run() paints, leaving the rest of \p dstDevice alone.
     */
    void runInRect(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice, const QRect &rect);
    QImage runOnQImage(const QImage &srcImage,
                       const QPointF &srcImageOffset,
                       const QTransform &imageToThumbTransform,
                       QPointF *newOffset);

    void toXML(QDomElement *e) const;
    static KisLiquifyTransformWorker* fromXML(const QDomElement &e);

    void translate(const QPointF &offset);
    void translateDstSpace(const QPointF &offset);

    QRect approxChangeRect(const QRect &rc);
    QRect approxNeedRect(const QRect &rc, const QRect &fullBounds);
    QRectF accumulatedStrokesBounds() const;

    void transformSrcAndDst(const QTransform &t);

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif /* __KIS_LIQUIFY_TRANSFORM_WORKER_H */
