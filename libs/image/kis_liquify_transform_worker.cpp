/*
 *  SPDX-FileCopyrightText: 2014 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_liquify_transform_worker.h"

#include <KoColorSpace.h>
#include <cmath>
#include <limits>
#include "kis_grid_interpolation_tools.h"
#include "kis_dom_utils.h"
#include "krita_utils.h"
#include "KisSpatialContainer.h"


struct Q_DECL_HIDDEN KisLiquifyTransformWorker::Private
{
    Private(const QRect &_srcBounds,
            KoUpdater *_progress,
            int _pixelPrecision)
        : srcBounds(_srcBounds)
        , originalPointsContainer(_srcBounds)
        , transformedPointsContainer(_srcBounds)
        , progress(_progress)
        , pixelPrecision(_pixelPrecision)
    {
    }

    QRect srcBounds;

    QVector<QPointF> originalPoints;
    QVector<QPointF> transformedPoints;

    KisSpatialContainer originalPointsContainer;
    KisSpatialContainer transformedPointsContainer;

    QRectF accumulatedBrushStrokes;

    KoUpdater *progress;
    int pixelPrecision;
    QSize gridSize;

    void preparePoints();

    struct MapIndexesOp;

    template <class ProcessOp>
    void processTransformedPixelsBuildUp(ProcessOp op,
                                         const QPointF &base,
                                         qreal sigma);

    template <class ProcessOp>
    void processTransformedPixelsWash(ProcessOp op,
                                      const QPointF &base,
                                      qreal sigma,
                                      qreal flow);

    template <class ProcessOp>
    void processTransformedPixels(ProcessOp op,
                                  const QPointF &base,
                                  qreal sigma,
                                  bool useWashMode,
                                  qreal flow);
};

KisLiquifyTransformWorker::KisLiquifyTransformWorker(const QRect &srcBounds,
                                                     KoUpdater *progress,
                                                     int pixelPrecision)
    : m_d(new Private(srcBounds, progress, pixelPrecision))
{
    KIS_ASSERT_RECOVER_RETURN(!srcBounds.isEmpty());

    // TODO: implement 'progress' stuff
    m_d->preparePoints();
}

KisLiquifyTransformWorker::KisLiquifyTransformWorker(const KisLiquifyTransformWorker &rhs)
    : m_d(new Private(*rhs.m_d.data()))
{
}

KisLiquifyTransformWorker::~KisLiquifyTransformWorker()
{
}

bool KisLiquifyTransformWorker::operator==(const KisLiquifyTransformWorker &other) const
{
    bool result =
            m_d->srcBounds == other.m_d->srcBounds &&
            m_d->pixelPrecision == other.m_d->pixelPrecision &&
            m_d->gridSize == other.m_d->gridSize &&
            m_d->originalPoints.size() == other.m_d->originalPoints.size() &&
            m_d->transformedPoints.size() == other.m_d->transformedPoints.size();

    if (!result) return false;

    const qreal eps = 1e-6;

    result =
        KisAlgebra2D::fuzzyPointCompare(m_d->originalPoints, other.m_d->originalPoints, eps) &&
        KisAlgebra2D::fuzzyPointCompare(m_d->transformedPoints, other.m_d->transformedPoints, eps);

    return result;
}

bool KisLiquifyTransformWorker::isIdentity() const
{
    const qreal eps = 1e-6;
    return KisAlgebra2D::fuzzyPointCompare(m_d->originalPoints, m_d->transformedPoints, eps);
}

int KisLiquifyTransformWorker::pointToIndex(const QPoint &cellPt)
{
    return GridIterationTools::pointToIndex(cellPt, m_d->gridSize);
}

QSize KisLiquifyTransformWorker::gridSize() const
{
    return m_d->gridSize;
}

const QVector<QPointF>& KisLiquifyTransformWorker::originalPoints() const
{
    return m_d->originalPoints;
}

QVector<QPointF>& KisLiquifyTransformWorker::transformedPoints()
{
    return m_d->transformedPoints;
}

struct AllPointsFetcherOp
{
    AllPointsFetcherOp(QRectF srcRect) : m_srcRect(srcRect) {}

    inline void processPoint(int col, int row,
                             int prevCol, int prevRow,
                             int colIndex, int rowIndex) {

        Q_UNUSED(prevCol);
        Q_UNUSED(prevRow);
        Q_UNUSED(colIndex);
        Q_UNUSED(rowIndex);

        QPointF pt(col, row);
        m_points << pt;
    }

    inline void nextLine() {
    }

    QVector<QPointF> m_points;
    QRectF m_srcRect;
};

void KisLiquifyTransformWorker::Private::preparePoints()
{
    gridSize =
        GridIterationTools::calcGridSize(srcBounds, pixelPrecision);

    AllPointsFetcherOp pointsOp(srcBounds);
    GridIterationTools::processGrid(pointsOp, srcBounds, pixelPrecision);

    const int numPoints = pointsOp.m_points.size();

    KIS_ASSERT_RECOVER_RETURN(numPoints == gridSize.width() * gridSize.height());

    originalPoints = pointsOp.m_points;
    transformedPoints = pointsOp.m_points;

    originalPointsContainer.initializeWithGridPoints(srcBounds, pixelPrecision);
    transformedPointsContainer.initializeWithGridPoints(srcBounds, pixelPrecision);

}

void KisLiquifyTransformWorker::translate(const QPointF &offset)
{
    KIS_ASSERT_RECOVER_RETURN(m_d->originalPoints.size() ==
                              m_d->transformedPoints.size());

    // TODO: make it within Spatial Container, either a hidden offset, or just offsetting all points at once
    // and benchmark
    for (int i = 0; i < m_d->transformedPoints.count(); i++) {
        m_d->originalPointsContainer.movePoint(i, m_d->originalPoints[i], m_d->originalPoints[i] + offset);
        m_d->transformedPointsContainer.movePoint(i, m_d->transformedPoints[i], m_d->transformedPoints[i] + offset);

        m_d->originalPoints[i] += offset;
        m_d->transformedPoints[i] += offset;
    }

    m_d->accumulatedBrushStrokes.translate(offset);
}

void KisLiquifyTransformWorker::translateDstSpace(const QPointF &offset)
{
    // TODO: make it within Spatial Container, either a hidden offset, or just offsetting all points at once
    // and benchmark
    for (int i = 0; i < m_d->transformedPoints.count(); i++) {
        m_d->transformedPointsContainer.movePoint(i, m_d->transformedPoints[i], m_d->transformedPoints[i] + offset);
        m_d->transformedPoints[i] += offset;
    }
}

void KisLiquifyTransformWorker::undoPoints(const QPointF &base,
                                           qreal amount,
                                           qreal sigma)
{
    const qreal maxDistCoeff = 3.0;
    const qreal maxDist = maxDistCoeff * sigma;

    KIS_ASSERT_RECOVER_RETURN(m_d->originalPoints.size() ==
                              m_d->transformedPoints.size());

    QVector<int> indexes;
    m_d->transformedPointsContainer.findAllInRange(indexes, base, maxDist);

    // each point moves by its own position only: all of them, then the container
    QVector<QPointF> before(indexes.size());
    QVector<QPointF> after(indexes.size());
    for (int i = 0; i < indexes.count(); i++) {

        QPointF diff = m_d->transformedPoints[indexes[i]] - base;
        qreal dist = KisAlgebra2D::norm(diff);
        qreal lambda = exp(-0.5 * pow2(dist / sigma));
        lambda *= amount;

        before[i] = m_d->transformedPoints[indexes[i]];
        m_d->transformedPoints[indexes[i]] = m_d->originalPoints[indexes[i]] * lambda + m_d->transformedPoints[indexes[i]] * (1.0 - lambda);
        after[i] = m_d->transformedPoints[indexes[i]];
    }
    m_d->transformedPointsContainer.movePoints(indexes, before, after);
}

template <class ProcessOp>
void KisLiquifyTransformWorker::Private::
processTransformedPixelsBuildUp(ProcessOp op,
                                const QPointF &base,
                                qreal sigma)
{
    const qreal maxDist = ProcessOp::maxDistCoeff * sigma;
    QRectF clipRect(base.x() - maxDist, base.y() - maxDist,
                    2 * maxDist, 2 * maxDist);

    accumulatedBrushStrokes |= kisGrowRect(clipRect, pixelPrecision);

    QVector<int> indexes;
    transformedPointsContainer.findAllInRange(indexes, base, maxDist);

    // each point moves by its own position only: all of them, then the container
    QVector<int> moved;
    QVector<QPointF> before;
    QVector<QPointF> after;
    moved.reserve(indexes.size());
    before.reserve(indexes.size());
    after.reserve(indexes.size());
    for (int i = 0; i < indexes.count(); i++) {

        QPointF diff = transformedPoints[indexes[i]] - base;
        qreal dist = KisAlgebra2D::norm(diff);
        if (dist > maxDist) continue;

        const qreal lambda = exp(-0.5 * pow2(dist / sigma));
        moved << indexes[i];
        before << transformedPoints[indexes[i]];
        transformedPoints[indexes[i]] = op(transformedPoints[indexes[i]], base, diff, lambda);
        after << transformedPoints[indexes[i]];
    }
    transformedPointsContainer.movePoints(moved, before, after);
}

template <class ProcessOp>
void KisLiquifyTransformWorker::Private::
processTransformedPixelsWash(ProcessOp op,
                             const QPointF &base,
                             qreal sigma,
                             qreal flow)
{
    const qreal maxDist = ProcessOp::maxDistCoeff * sigma;
    QRectF clipRect(base.x() - maxDist, base.y() - maxDist,
                    2 * maxDist, 2 * maxDist);

    accumulatedBrushStrokes |= kisGrowRect(clipRect, pixelPrecision);

    KIS_ASSERT_RECOVER_RETURN(originalPoints.size() ==
                              transformedPoints.size());

    // TODO: remove the originalPointsContainer entirely, and use GridIterationTools to figure out indexes instead
    // and add unit tests for it

    QVector<int> indexes;
    originalPointsContainer.findAllInRange(indexes, base, maxDist);

    // each point moves by its own position only: all of them, then the container
    QVector<int> moved;
    QVector<QPointF> before;
    QVector<QPointF> after;
    moved.reserve(indexes.size());
    before.reserve(indexes.size());
    after.reserve(indexes.size());
    for (int i = 0; i < indexes.count(); i++) {

        QPointF diff = originalPoints[indexes[i]] - base;
        qreal dist = KisAlgebra2D::norm(diff);

        const qreal lambda = exp(-0.5 * pow2(dist / sigma));
        QPointF dstPt = op(originalPoints[indexes[i]], base, diff, lambda);

        if (kisDistance(dstPt, originalPoints[indexes[i]]) > kisDistance(transformedPoints[indexes[i]], originalPoints[indexes[i]])) {
            moved << indexes[i];
            before << transformedPoints[indexes[i]];
            transformedPoints[indexes[i]] = (1.0 - flow) * transformedPoints[indexes[i]] + flow * dstPt;
            after << transformedPoints[indexes[i]];
        }
    }
    transformedPointsContainer.movePoints(moved, before, after);
}

template <class ProcessOp>
void KisLiquifyTransformWorker::Private::
processTransformedPixels(ProcessOp op,
                         const QPointF &base,
                         qreal sigma,
                         bool useWashMode,
                         qreal flow)
{
    if (useWashMode) {
        processTransformedPixelsWash(op, base, sigma, flow);
    } else {
        processTransformedPixelsBuildUp(op, base, sigma);
    }
}

struct TranslateOp
{
    TranslateOp(const QPointF &offset) : m_offset(offset) {}

    QPointF operator() (const QPointF &pt,
                        const QPointF &base,
                        const QPointF &diff,
                        qreal lambda)
    {
        Q_UNUSED(base);
        Q_UNUSED(diff);
        return pt + lambda * m_offset;
    }

    static const qreal maxDistCoeff;

    QPointF m_offset;
};

const qreal TranslateOp::maxDistCoeff = 3.0;

struct ScaleOp
{
    ScaleOp(qreal scale) : m_scale(scale) {}

    QPointF operator() (const QPointF &pt,
                        const QPointF &base,
                        const QPointF &diff,
                        qreal lambda)
    {
        Q_UNUSED(pt);
        Q_UNUSED(diff);
        return base + (1.0 + m_scale * lambda) * diff;
    }

    static const qreal maxDistCoeff;

    qreal m_scale;
};

const qreal ScaleOp::maxDistCoeff = 3.0;

struct RotateOp
{
    RotateOp(qreal angle) : m_angle(angle) {}

    QPointF operator() (const QPointF &pt,
                        const QPointF &base,
                        const QPointF &diff,
                        qreal lambda)
    {
        Q_UNUSED(pt);

        const qreal angle = m_angle * lambda;
        const qreal sinA = std::sin(angle);
        const qreal cosA = std::cos(angle);

        qreal x =  cosA * diff.x() + sinA * diff.y();
        qreal y = -sinA * diff.x() + cosA * diff.y();

        return base + QPointF(x, y);
    }

    static const qreal maxDistCoeff;

    qreal m_angle;
};

const qreal RotateOp::maxDistCoeff = 3.0;

void KisLiquifyTransformWorker::translatePoints(const QPointF &base,
                                                const QPointF &offset,
                                                qreal sigma,
                                                bool useWashMode,
                                                qreal flow)
{
    TranslateOp op(offset);
    m_d->processTransformedPixels(op, base, sigma, useWashMode, flow);
}

void KisLiquifyTransformWorker::scalePoints(const QPointF &base,
                                            qreal scale,
                                            qreal sigma,
                                            bool useWashMode,
                                            qreal flow)
{
    ScaleOp op(scale);
    m_d->processTransformedPixels(op, base, sigma, useWashMode, flow);
}

void KisLiquifyTransformWorker::rotatePoints(const QPointF &base,
                                             qreal angle,
                                             qreal sigma,
                                             bool useWashMode,
                                             qreal flow)
{
    RotateOp op(angle);
    m_d->processTransformedPixels(op, base, sigma, useWashMode, flow);
}

void KisLiquifyTransformWorker::run(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(*srcDevice->colorSpace() == *dstDevice->colorSpace());

    dstDevice->clear();

    using namespace GridIterationTools;
    QRect correctSubGrid = calculateCorrectSubGrid(m_d->srcBounds, m_d->pixelPrecision, m_d->accumulatedBrushStrokes, m_d->gridSize);

    PaintDevicePolygonOp polygonOp(srcDevice, dstDevice);
    RegularGridIndexesOp indexesOp(m_d->gridSize);

    bool canMergeRects = GridIterationTools::canProcessRectsInRandomOrder(indexesOp, m_d->transformedPoints, correctSubGrid);
    polygonOp.setCanMergeRects(canMergeRects);

#ifdef DEBUG_PAINTING_POLYGONS
    polygonOp.setDebugColor(Qt::red);
#endif

    iterateThroughGrid<AlwaysCompletePolygonPolicy>(polygonOp, indexesOp,
                                                    m_d->gridSize,
                                                    m_d->originalPoints,
                                                    m_d->transformedPoints,
                                                    correctSubGrid);
    QList<QRectF> areasToCopy = cutOutSubgridFromBounds(correctSubGrid, m_d->srcBounds, m_d->gridSize, m_d->originalPoints);
#ifdef DEBUG_PAINTING_POLYGONS
    QList<QColor> colors = {Qt::blue, Qt::green, Qt::yellow, Qt::black};
#endif
    for (int i = 0; i < areasToCopy.length(); i++) {
#ifdef DEBUG_PAINTING_POLYGONS
        polygonOp.setDebugColor(colors[i]);
#endif
        polygonOp.fastCopyArea(areasToCopy[i].toRect(), false);
    }
}

bool KisLiquifyTransformWorker::changedRect(const KisLiquifyTransformWorker &previous, QRect *rect) const
{
    const Private &a = *m_d;
    const Private &b = *previous.m_d;
    if (a.srcBounds != b.srcBounds || a.pixelPrecision != b.pixelPrecision || a.gridSize != b.gridSize ||
        a.transformedPoints.size() != b.transformedPoints.size() || a.originalPoints != b.originalPoints) {
        return false;
    }

    const int width = a.gridSize.width();
    const int height = a.gridSize.height();
    qreal left = std::numeric_limits<qreal>::max();
    qreal top = left;
    qreal right = std::numeric_limits<qreal>::lowest();
    qreal bottom = right;
    auto add = [&] (const QPointF &pt) {
        left = qMin(left, pt.x());
        right = qMax(right, pt.x());
        top = qMin(top, pt.y());
        bottom = qMax(bottom, pt.y());
    };
    // every cell with a moved corner, at its old and new places
    auto addCell = [&] (int col, int row) {
        if (col < 0 || row < 0 || col >= width - 1 || row >= height - 1) return;
        const int i = row * width + col;
        for (int index : {i, i + 1, i + width, i + width + 1}) {
            add(a.transformedPoints[index]);
            add(b.transformedPoints[index]);
        }
    };
    for (int i = 0; i < a.transformedPoints.size(); i++) {
        if (a.transformedPoints[i] == b.transformedPoints[i]) continue;
        const int col = i % width;
        const int row = i / width;
        addCell(col - 1, row - 1);
        addCell(col, row - 1);
        addCell(col - 1, row);
        addCell(col, row);
    }
    *rect = left > right ? QRect() : QRectF(QPointF(left, top), QPointF(right, bottom)).toAlignedRect().adjusted(-1, -1, 1, 1);
    return true;
}

namespace {

/// PaintDevicePolygonOp painting only within a rect
struct ClippedPolygonOp
{
    ClippedPolygonOp(GridIterationTools::PaintDevicePolygonOp &op, const QRect &clip)
        : m_op(op),
          m_clip(clip),
          m_testedClip(QRectF(clip).adjusted(-1, -1, 1, 1)),
          // pixels are painted at their integer coordinates: half a pixel
          // around them, so exactly the clip's pixels are inside, none on an
          // edge (bands of a rect that threads paint at once never overlap)
          m_clipPolygon(QRectF(clip).translated(-0.5, -0.5))
    {
        // that half pixel makes the polygon's aligned bounds one row and column too big, and
        // threads painting neighbouring bands must not lock each other's tiles
        m_op.setIterationLimit(clip);
    }

    void operator() (const QPolygonF &srcPolygon, const QPolygonF &dstPolygon) {
        const QRectF bounds = dstPolygon.boundingRect();
        if (!bounds.intersects(m_testedClip)) return;
        // the pixels a cell may paint, the ones on its right and bottom edges too
        if (QRect(m_clip).contains(bounds.toAlignedRect().adjusted(0, 0, 1, 1))) {
            m_op(srcPolygon, dstPolygon);
        } else if (KisAlgebra2D::fuzzyPointCompare(srcPolygon, dstPolygon, m_op.m_epsilon) &&
                   KisAlgebra2D::isPolygonPixelAlignedRect(dstPolygon, m_op.m_epsilon)) {
            // an unmoved cell, which run() copies as it is
            m_op.fastCopyArea(bounds.toAlignedRect() & m_clip, false);
        } else {
            m_op(srcPolygon, dstPolygon, dstPolygon.intersected(m_clipPolygon));
        }
    }

    void finalize() {
        m_op.finalize();
    }

    GridIterationTools::PaintDevicePolygonOp &m_op;
    QRect m_clip;
    QRectF m_testedClip;
    QPolygonF m_clipPolygon;
};

}

void KisLiquifyTransformWorker::runInRect(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice, const QRect &rect)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(*srcDevice->colorSpace() == *dstDevice->colorSpace());
    if (rect.isEmpty()) return;

    dstDevice->clear(rect);

    using namespace GridIterationTools;
    QRect correctSubGrid = calculateCorrectSubGrid(m_d->srcBounds, m_d->pixelPrecision, m_d->accumulatedBrushStrokes, m_d->gridSize);

    PaintDevicePolygonOp polygonOp(srcDevice, dstDevice);
    RegularGridIndexesOp indexesOp(m_d->gridSize);
    // the lazy copies merge rects across the clip: copy each at once
    polygonOp.setCanMergeRects(false);
    ClippedPolygonOp clippedOp(polygonOp, rect);

    /**
     * Only the rows of cells that may reach the rect: cell row r spans grid
     * rows r and r + 1; rows whose points all stay above or below the rect
     * (with more margin than ClippedPolygonOp's) paint nothing in it, so
     * skipping them changes nothing. Cuts the cost of a thin rect, such as
     * a band the incremental preview paints in a thread of its own.
     */
    QRect subGrid = correctSubGrid;
    if (!subGrid.isEmpty()) {
        const qreal top = rect.top() - 2;
        const qreal bottom = rect.bottom() + 3;
        // whether all points of each grid row are above the rect, or all below
        const int rows = subGrid.height();
        QVector<char> above(rows), below(rows);
        for (int i = 0; i < rows; i++) {
            const QPointF *points = m_d->transformedPoints.constData() + (subGrid.top() + i) * m_d->gridSize.width();
            bool allAbove = true;
            bool allBelow = true;
            for (int col = subGrid.left(); col <= subGrid.right(); col++) {
                const qreal y = points[col].y();
                allAbove &= y < top;
                allBelow &= y > bottom;
            }
            above[i] = allAbove;
            below[i] = allBelow;
        }
        // cell row r (grid rows r and r + 1) is skipped when both its grid rows are above, or both below
        int first = -1;
        int last = -1;
        for (int i = 0; i + 1 < rows; i++) {
            const bool skipped = (above[i] && above[i + 1]) || (below[i] && below[i + 1]);
            if (skipped) continue;
            if (first < 0) first = i;
            last = i;
        }
        subGrid = first < 0 ? QRect() :
            QRect(subGrid.left(), subGrid.top() + first, subGrid.width(), last - first + 2);
    }

    if (!subGrid.isEmpty()) {
        iterateThroughGrid<AlwaysCompletePolygonPolicy>(clippedOp, indexesOp,
                                                        m_d->gridSize,
                                                        m_d->originalPoints,
                                                        m_d->transformedPoints,
                                                        subGrid);
    } else {
        clippedOp.finalize();
    }
    const QList<QRectF> areasToCopy = cutOutSubgridFromBounds(correctSubGrid, m_d->srcBounds, m_d->gridSize, m_d->originalPoints);
    for (const QRectF &area : areasToCopy) {
        const QRect clipped = area.toRect() & rect;
        if (!clipped.isEmpty()) polygonOp.fastCopyArea(clipped, false);
    }
}

QRect KisLiquifyTransformWorker::approxChangeRect(const QRect &rc)
{
    const qreal margin = 0.05;
    QRect resultRect = m_d->transformedPointsContainer.exactBounds().toRect();
    return KisAlgebra2D::blowRect(resultRect | rc, margin);
}

QRect KisLiquifyTransformWorker::approxNeedRect(const QRect &rc, const QRect &fullBounds)
{
    Q_UNUSED(rc);
    return fullBounds;
}

QRectF KisLiquifyTransformWorker::accumulatedStrokesBounds() const
{
    return m_d->accumulatedBrushStrokes;
}

void KisLiquifyTransformWorker::transformSrcAndDst(const QTransform &t)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(t.type() <= QTransform::TxScale);

    m_d->srcBounds = t.mapRect(m_d->srcBounds);

    // the scale to a level of detail (a power of two) keeps the containers'
    // structure: scale them in one pass instead of moving each point
    if (t.type() == QTransform::TxScale && t.dx() == 0 && t.dy() == 0 &&
        KisSpatialContainer::isExactScale(t.m11()) && KisSpatialContainer::isExactScale(t.m22())) {

        m_d->originalPointsContainer.scaleExactly(t.m11(), t.m22());
        m_d->transformedPointsContainer.scaleExactly(t.m11(), t.m22());
        for (int i = 0; i < m_d->transformedPoints.count(); i++) {
            m_d->originalPoints[i] = t.map(m_d->originalPoints[i]);
            m_d->transformedPoints[i] = t.map(m_d->transformedPoints[i]);
        }
    } else {
        // TODO: do it within Spatial Container
        for (int i = 0; i < m_d->transformedPoints.count(); i++) {
            m_d->originalPointsContainer.movePoint(i, m_d->originalPoints[i], t.map(m_d->originalPoints[i]));
            m_d->transformedPointsContainer.movePoint(i, m_d->transformedPoints[i], t.map(m_d->transformedPoints[i]));

            m_d->originalPoints[i] = t.map(m_d->originalPoints[i]);
            m_d->transformedPoints[i] = t.map(m_d->transformedPoints[i]);
        }
    }
    m_d->accumulatedBrushStrokes = t.map(m_d->accumulatedBrushStrokes).boundingRect();
    if (t == QTransform::fromScale(t.m11(), t.m22()) && t.m11() == t.m22()) {
        m_d->pixelPrecision *= t.m11();
        KIS_SAFE_ASSERT_RECOVER(m_d->pixelPrecision > 0) { m_d->pixelPrecision = 1; }
        KIS_SAFE_ASSERT_RECOVER(QList<int>({1, 2, 4, 8, 16}).contains(m_d->pixelPrecision) || m_d->pixelPrecision%16 == 0) { m_d->pixelPrecision = 1; }
        // should check if pixelPrecision is a power of 2, but that's more complicated
    }
}

#include <functional>
#include <QTransform>

using PointMapFunction = std::function<QPointF (const QPointF&)>;


PointMapFunction bindPointMapTransform(const QTransform &transform) {
    using namespace std::placeholders;

    typedef QPointF (QTransform::*MapFuncType)(const QPointF&) const;
    return std::bind(static_cast<MapFuncType>(&QTransform::map), &transform, _1);
}

QImage KisLiquifyTransformWorker::runOnQImage(const QImage &srcImage,
                                              const QPointF &srcImageOffset,
                                              const QTransform &imageToThumbTransform,
                                              QPointF *newOffset)
{
    KIS_ASSERT_RECOVER(m_d->originalPoints.size() == m_d->transformedPoints.size()) {
        return QImage();
    }

    KIS_ASSERT_RECOVER(!srcImage.isNull()) {
        return QImage();
    }

    KIS_ASSERT_RECOVER(srcImage.format() == QImage::Format_ARGB32) {
        return QImage();
    }

    QVector<QPointF> originalPointsLocal(m_d->originalPoints);
    QVector<QPointF> transformedPointsLocal(m_d->transformedPoints);

    PointMapFunction mapFunc = bindPointMapTransform(imageToThumbTransform);

    std::transform(originalPointsLocal.begin(), originalPointsLocal.end(),
                   originalPointsLocal.begin(), mapFunc);

    std::transform(transformedPointsLocal.begin(), transformedPointsLocal.end(),
                   transformedPointsLocal.begin(), mapFunc);

    QRectF dstBounds;
    Q_FOREACH (const QPointF &pt, transformedPointsLocal) {
        KisAlgebra2D::accumulateBounds(pt, &dstBounds);
    }

    const QRectF srcBounds(srcImageOffset, srcImage.size());
    dstBounds |= srcBounds;

    QPointF dstQImageOffset = dstBounds.topLeft();
    *newOffset = dstQImageOffset;

    QRect dstBoundsI = dstBounds.toAlignedRect();

    QImage dstImage(dstBoundsI.size(), srcImage.format());
    dstImage.fill(0);

    GridIterationTools::QImagePolygonOp polygonOp(srcImage, dstImage, srcImageOffset, dstQImageOffset);
    GridIterationTools::RegularGridIndexesOp indexesOp(m_d->gridSize);


    QRect correctSubGrid = GridIterationTools::calculateCorrectSubGrid(m_d->srcBounds, m_d->pixelPrecision, m_d->accumulatedBrushStrokes, m_d->gridSize);
    bool canMergeRects = GridIterationTools::canProcessRectsInRandomOrder(indexesOp, m_d->transformedPoints, correctSubGrid);
    polygonOp.setCanMergeRects(canMergeRects);


    GridIterationTools::iterateThroughGrid<GridIterationTools::AlwaysCompletePolygonPolicy>(polygonOp, indexesOp,
                                                    m_d->gridSize,
                                                    originalPointsLocal,
                                                    transformedPointsLocal,
                                                    correctSubGrid);


    QList<QRectF> areasToCopy = GridIterationTools::cutOutSubgridFromBounds(correctSubGrid, m_d->srcBounds, m_d->gridSize, m_d->originalPoints);
    polygonOp.setCanMergeRects(false);
    const qreal eps = 0.001;
    for (int i = 0; i < areasToCopy.length(); i++) {
        QPolygonF transformed = imageToThumbTransform.map(QPolygonF(areasToCopy[i]));
        if (KisAlgebra2D::isPolygonPixelAlignedRect(transformed, eps)) {
            polygonOp.fastCopyArea(transformed.boundingRect().toRect());
        } else {
            polygonOp.operator()(transformed, transformed);
        }
    }
    return dstImage;
}


void KisLiquifyTransformWorker::toXML(QDomElement *e) const
{
    QDomDocument doc = e->ownerDocument();
    QDomElement liqEl = doc.createElement("liquify_points");
    e->appendChild(liqEl);

    KisDomUtils::saveValue(&liqEl, "srcBounds", m_d->srcBounds);
    KisDomUtils::saveValue(&liqEl, "originalPoints", m_d->originalPoints);
    KisDomUtils::saveValue(&liqEl, "transformedPoints", m_d->transformedPoints);
    KisDomUtils::saveValue(&liqEl, "pixelPrecision", m_d->pixelPrecision);
    KisDomUtils::saveValue(&liqEl, "gridSize", m_d->gridSize);
}

KisLiquifyTransformWorker* KisLiquifyTransformWorker::fromXML(const QDomElement &e)
{
    QDomElement liquifyEl;

    QRect srcBounds;
    QVector<QPointF> originalPoints;
    QVector<QPointF> transformedPoints;
    int pixelPrecision;
    QSize gridSize;

    bool result = false;


    result =
        KisDomUtils::findOnlyElement(e, "liquify_points", &liquifyEl) &&

        KisDomUtils::loadValue(liquifyEl, "srcBounds", &srcBounds) &&
        KisDomUtils::loadValue(liquifyEl, "originalPoints", &originalPoints) &&
        KisDomUtils::loadValue(liquifyEl, "transformedPoints", &transformedPoints) &&
        KisDomUtils::loadValue(liquifyEl, "pixelPrecision", &pixelPrecision) &&
        KisDomUtils::loadValue(liquifyEl, "gridSize", &gridSize);

    if (!result) {
        warnKrita << "WARNING: Failed to load liquify worker from XML";
        return new KisLiquifyTransformWorker(QRect(0,0,1024, 1024), 0, 8);
    }

    KisLiquifyTransformWorker *worker =
        new KisLiquifyTransformWorker(srcBounds, 0, pixelPrecision);

    const int numPoints = originalPoints.size();

    if (numPoints != transformedPoints.size() ||
        numPoints != worker->m_d->originalPoints.size() ||
        gridSize != worker->m_d->gridSize) {
        warnKrita << "WARNING: Inconsistent number of points!";
        warnKrita << ppVar(originalPoints.size());
        warnKrita << ppVar(transformedPoints.size());
        warnKrita << ppVar(gridSize);
        warnKrita << ppVar(worker->m_d->originalPoints.size());
        warnKrita << ppVar(worker->m_d->transformedPoints.size());
        warnKrita << ppVar(worker->m_d->gridSize);

        return worker;
    }

    QRectF changedRect = QRectF();

    for (int i = 0; i < numPoints; i++) {
        worker->m_d->originalPoints[i] = originalPoints[i];
        worker->m_d->transformedPoints[i] = transformedPoints[i];
        if (!KisAlgebra2D::fuzzyPointCompare(transformedPoints[i], originalPoints[i])) {
            KisAlgebra2D::accumulateBounds(transformedPoints[i], &changedRect);
            KisAlgebra2D::accumulateBounds(originalPoints[i], &changedRect);
        }
    }
    changedRect = kisGrowRect(changedRect, pixelPrecision);

    worker->m_d->transformedPointsContainer.initializeWith(worker->m_d->transformedPoints);
    worker->m_d->originalPointsContainer.initializeWith(worker->m_d->originalPoints);

    worker->m_d->accumulatedBrushStrokes = changedRect;


    return worker;
}
