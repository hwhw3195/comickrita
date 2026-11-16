/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveOpacity.h"

#include <QHash>
#include <QRectF>
#include <QVector>

#include <cmath>
#include <limits>

const QString VCurveOpacity::Attribute = QStringLiteral("opacity");

namespace {

struct Piece {
    QPointF a, b;
    qreal oa, ob;
};

/// the centerline as straight pieces, in image coordinates, about a pixel off at most
QVector<Piece> flatten(const VCurveData &d, const QTransform &toImage)
{
    QVector<Piece> pieces;
    const QStringList names{VCurveOpacity::Attribute};
    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int end = d.curveEnd(c);
        if (end - begin == 1) {
            const QPointF p = toImage.map(d.positions[begin]);
            const qreal o = d.attribute(VCurveOpacity::Attribute, begin, 1.0);
            pieces << Piece{p, p, o, o};
            continue;
        }
        for (int i = begin; i < end; i++) {
            const int j = d.nextIndex(i);
            if (j < 0) break;
            const VCurvePoint a = d.point(i, names);
            const VCurvePoint b = d.point(j, names);
            const qreal oa = a.attributes[0];
            const qreal ob = b.attributes[0];
            int parts = 1;
            if (a.segment == VCurveData::CurveSegment) {
                const qreal length = QLineF(toImage.map(a.position), toImage.map(a.handleOut)).length() +
                                     QLineF(toImage.map(a.handleOut), toImage.map(b.handleIn)).length() +
                                     QLineF(toImage.map(b.handleIn), toImage.map(b.position)).length();
                parts = qBound(1, int(std::ceil(length / 2.0)), 256);
            }
            QPointF previous = toImage.map(a.position);
            for (int s = 1; s <= parts; s++) {
                const qreal t = qreal(s) / parts;
                const QPointF p = toImage.map(VCurvePoint::segmentPoint(a, b, t));
                pieces << Piece{previous, p, oa + (ob - oa) * (t - 1.0 / parts), oa + (ob - oa) * t};
                previous = p;
            }
        }
    }
    return pieces;
}

/// the opacity at the place on \p piece nearest to \p p, and the squared distance to it
qreal nearest(const Piece &piece, const QPointF &p, qreal *distance2)
{
    const QPointF ab = piece.b - piece.a;
    const qreal l2 = ab.x() * ab.x() + ab.y() * ab.y();
    const qreal t = l2 > 0 ? qBound(0.0, ((p.x() - piece.a.x()) * ab.x() + (p.y() - piece.a.y()) * ab.y()) / l2, 1.0) : 0.0;
    const QPointF q = piece.a + t * ab - p;
    *distance2 = q.x() * q.x() + q.y() * q.y();
    return piece.oa + (piece.ob - piece.oa) * t;
}

}

bool VCurveOpacity::isVarying(const VCurveData &curves)
{
    auto it = curves.attributes.constFind(Attribute);
    if (it == curves.attributes.constEnd()) return false;
    for (qreal o : *it) {
        if (o < 1.0) return true;
    }
    return false;
}

void VCurveOpacity::applyMask(QImage &image, const QPoint &offset, const VCurveData &curves, const QTransform &toImage)
{
    if (image.isNull() || !isVarying(curves)) return;
    if (image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    const bool premultiplied = image.format() == QImage::Format_ARGB32_Premultiplied;

    const QVector<Piece> pieces = flatten(curves, toImage);
    if (pieces.isEmpty()) return;

    // the pieces by grid cell, in image pixels relative to the image's top left
    const qreal cell = 16.0;
    QHash<quint64, QVector<int>> grid;
    auto key = [] (int x, int y) { return (quint64(quint32(x)) << 32) | quint32(y); };
    QRectF reach;
    for (int k = 0; k < pieces.size(); k++) {
        const QRectF r = QRectF(pieces[k].a, pieces[k].b).normalized().translated(-offset);
        reach |= r.adjusted(-0.5, -0.5, 0.5, 0.5);
        for (int x = int(std::floor(r.left() / cell)); x <= int(std::floor(r.right() / cell)); x++) {
            for (int y = int(std::floor(r.top() / cell)); y <= int(std::floor(r.bottom() / cell)); y++) {
                grid[key(x, y)] << k;
            }
        }
    }
    const int maxRing = int(std::ceil(std::hypot(image.width() + reach.width(), image.height() + reach.height()) / cell)) + 1;

    for (int y = 0; y < image.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const QRgb pixel = line[x];
            if (qAlpha(pixel) == 0) continue;
            const QPointF p(x + 0.5, y + 0.5);
            const int cx = int(std::floor(p.x() / cell));
            const int cy = int(std::floor(p.y() / cell));
            qreal best = std::numeric_limits<qreal>::max();
            qreal opacity = 1.0;
            // rings of cells outward until no nearer piece can be left
            for (int ring = 0; ring <= maxRing; ring++) {
                if (ring > 0 && (ring - 1) * cell > std::sqrt(best)) break;
                for (int gx = cx - ring; gx <= cx + ring; gx++) {
                    for (int gy = cy - ring; gy <= cy + ring; gy++) {
                        if (qMax(std::abs(gx - cx), std::abs(gy - cy)) != ring) continue;
                        auto it = grid.constFind(key(gx, gy));
                        if (it == grid.constEnd()) continue;
                        for (int k : *it) {
                            qreal d2;
                            const qreal o = nearest(pieces[k], p + offset, &d2);
                            if (d2 < best) {
                                best = d2;
                                opacity = o;
                            }
                        }
                    }
                }
            }
            if (opacity >= 1.0) continue;
            const qreal f = qBound(0.0, opacity, 1.0);
            if (premultiplied) {
                line[x] = qRgba(qRound(qRed(pixel) * f), qRound(qGreen(pixel) * f), qRound(qBlue(pixel) * f),
                                qRound(qAlpha(pixel) * f));
            } else {
                line[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), qRound(qAlpha(pixel) * f));
            }
        }
    }
}
