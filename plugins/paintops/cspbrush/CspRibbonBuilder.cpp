/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspRibbonBuilder.h"

#include <QtMath>
#include <cmath>

#include <KoColorSpace.h>
#include <KoMixColorsOp.h>
#include <kis_algebra_2d.h>
#include <kis_fixed_paint_device.h>

#include "CspTipSet.h"

namespace {
QPointF normalized(const QPointF &v)
{
    const qreal len = std::hypot(v.x(), v.y());
    return len > 1e-9 ? v / len : QPointF();
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

/**
 * Inverse bilinear map of the quad p00, p10, p11, p01 (u across, v along).
 * Solves the quadratic in v with the cancellation-free formula, takes the
 * root closest to the unit square and verifies it by mapping forward, so
 * pixels of the bounding box that lie outside the quad never get a
 * (u, v) inside it (KisFourPointInterpolatorBackward does not guarantee
 * this outside the polygon, which left speckles along the ribbon edges).
 */
class InverseBilinear
{
public:
    InverseBilinear(const QPointF &p00, const QPointF &p10, const QPointF &p11, const QPointF &p01)
        : m_p00(p00)
        , m_e(p10 - p00)
        , m_f(p01 - p00)
        , m_g(p00 - p10 + p11 - p01)
        , m_k2(cross(m_g, m_f))
        , m_ef(cross(m_e, m_f))
    {
    }

    bool map(const QPointF &p, qreal *u, qreal *v) const
    {
        const QPointF h = p - m_p00;
        const qreal k1 = cross(h, m_g) + m_ef;
        const qreal k0 = cross(h, m_e);
        // k2 v^2 + k1 v + k0 = 0
        qreal roots[2];
        int count = 0;
        const qreal disc = k1 * k1 - 4 * m_k2 * k0;
        if (disc < 0) return false;
        const qreal q = -0.5 * (k1 + std::copysign(std::sqrt(disc), k1));
        if (q != 0) roots[count++] = k0 / q;
        if (qAbs(m_k2) > 1e-12 * qMax(1.0, qAbs(k1))) roots[count++] = q / m_k2;

        bool found = false;
        qreal bestPenalty = 0;
        for (int i = 0; i < count; i++) {
            const qreal rv = roots[i];
            const QPointF across = m_e + m_g * rv;
            const qreal len2 = KisAlgebra2D::dotProduct(across, across);
            if (len2 < 1e-12) continue;
            const qreal ru = KisAlgebra2D::dotProduct(h - m_f * rv, across) / len2;
            // reject solutions that do not map back onto the pixel
            const QPointF back = m_e * ru + m_f * rv + m_g * (ru * rv);
            if (KisAlgebra2D::norm(back - h) > 1e-3) continue;
            const qreal penalty = qMax(0.0, qMax(-ru, ru - 1.0)) + qMax(0.0, qMax(-rv, rv - 1.0));
            if (!found || penalty < bestPenalty) {
                found = true;
                bestPenalty = penalty;
                *u = ru;
                *v = rv;
            }
        }
        return found;
    }

private:
    QPointF m_p00, m_e, m_f, m_g;
    qreal m_k2, m_ef;
};
}

CspRibbonBuilder::CspRibbonBuilder(const CspBrushParams &params, CspTipSet *tips)
    : m_params(params)
    , m_tips(tips)
{
    // the angle only turns the image in steps of 90 degrees (CSP ribbon
    // tutorials use 90 for horizontally designed materials)
    m_quarterTurns = ((qRound(params.angle / 90.0) % 4) + 4) % 4;
}

qreal CspRibbonBuilder::sampleSpacing(qreal brushSize)
{
    return qBound(1.0, brushSize / 16.0, 4.0);
}

CspRibbonBuilder::TipFrame CspRibbonBuilder::frame(int tipIndex) const
{
    const QImage &image = m_tips->tipImage(qMax(0, tipIndex));
    TipFrame f;
    const bool turned = m_quarterTurns % 2;
    f.alongPx = qMax(1, turned ? image.width() : image.height());
    f.acrossPx = qMax(1, turned ? image.height() : image.width());
    return f;
}

qreal CspRibbonBuilder::tileLength(const Sample &sample, int tipIndex) const
{
    const QImage &image = m_tips->tipImage(qMax(0, tipIndex));
    const qreal scale = sample.brushSize / qMax(1, qMax(image.width(), image.height()));
    return qMax(0.5, frame(tipIndex).alongPx * scale);
}

CspRibbonBuilder::Edge CspRibbonBuilder::makeEdge(const Sample &at, const QPointF &direction) const
{
    const int tip = m_tileTips.isEmpty() ? 0 : qMax(0, m_tileTips.last());
    const QImage &image = m_tips->tipImage(tip);
    const qreal scale = at.brushSize / qMax(1, qMax(image.width(), image.height()));
    const qreal halfWidth = 0.5 * frame(tip).acrossPx * scale * at.thickness;

    // right-hand side of the stroke direction on screen (y down)
    const QPointF right(-direction.y(), direction.x());
    Edge e;
    e.left = at.pos - right * halfWidth;
    e.right = at.pos + right * halfWidth;
    return e;
}

int CspRibbonBuilder::tipForTile(int tile, KisRandomSourceSP random)
{
    while (m_tileTips.size() <= tile) {
        m_tileTips << m_tips->nextTipIndex(random);
    }
    return m_tileTips[qMax(0, tile)];
}

bool CspRibbonBuilder::addSample(const Sample &sample, const KoColorSpace *cs, KisRandomSourceSP random,
                                 KisFixedPaintDeviceSP *dab, QRect *rect)
{
    if (!m_samples.isEmpty() && KisAlgebra2D::norm(sample.pos - m_samples.last().pos) < 1e-3) {
        return false;
    }
    m_samples << sample;
    if (m_samples.size() > 3) m_samples.removeFirst();

    if (m_samples.size() == 2 && m_edges.isEmpty()) {
        tipForTile(0, random);
        Edge first = makeEdge(m_samples[0], normalized(m_samples[1].pos - m_samples[0].pos));
        first.v = 0.0;
        m_edges << first;
        return false;
    }
    if (m_samples.size() < 3) return false;

    const Sample &a = m_samples[0];
    const Sample &b = m_samples[1];
    const Sample &c = m_samples[2];

    QPointF direction = normalized(c.pos - a.pos);
    if (direction.isNull()) direction = normalized(b.pos - a.pos);
    Edge edge = makeEdge(b, direction);

    // a copy: appending the new edge below may reallocate m_edges
    const Edge prev = m_edges.last();
    const QPointF travel = normalized(b.pos - a.pos);
    // pin a side that would move backwards: the stroke turns tighter than
    // the half width there, the quads fan around that point
    if (KisAlgebra2D::dotProduct(edge.left - prev.left, travel) < 0) edge.left = prev.left;
    if (KisAlgebra2D::dotProduct(edge.right - prev.right, travel) < 0) edge.right = prev.right;

    const int tile = int(std::floor(prev.v));
    edge.v = prev.v + KisAlgebra2D::norm(b.pos - a.pos) / tileLength(a, tipForTile(tile, random));
    m_edges << edge;

    renderQuad(prev, edge, cs, random, dab, rect);
    return *dab && !rect->isEmpty();
}

bool CspRibbonBuilder::sampleTip(int tipIndex, bool mirrorAlong, qreal u, qreal v, const KoColorSpace *cs, quint8 *dst)
{
    const QImage &image = m_tips->tipImage(tipIndex);
    const int w = image.width();
    const int h = image.height();
    const QByteArray &pixels = m_tips->colorizedTip(tipIndex, cs);
    const int pixelSize = cs->pixelSize();

    qreal along = mirrorAlong ? 1.0 - v : v;
    qreal across = u;
    if (m_params.flipHorizontal == 1) across = 1.0 - across;
    if (m_params.flipVertical == 1) along = 1.0 - along;

    qreal ix = 0, iy = 0;
    switch (m_quarterTurns) {
    case 0: ix = across * w; iy = along * h; break;
    case 1: ix = along * w; iy = (1.0 - across) * h; break;
    case 2: ix = (1.0 - across) * w; iy = (1.0 - along) * h; break;
    default: ix = (1.0 - along) * w; iy = across * h; break;
    }

    const bool alongIsY = m_quarterTurns % 2 == 0;
    const qreal fx = ix - 0.5;
    const qreal fy = iy - 0.5;
    const int x0 = int(std::floor(fx));
    const int y0 = int(std::floor(fy));
    const qreal wx = fx - x0;
    const qreal wy = fy - y0;

    if (m_transparent.size() != pixelSize) {
        m_transparent = QByteArray(pixelSize, 0);
        cs->setOpacity(reinterpret_cast<quint8 *>(m_transparent.data()), quint8(0), 1);
    }

    // wrap along the stroke (tiles join top to bottom), transparent across
    auto fetch = [&](int x, int y) -> const quint8 * {
        if (alongIsY) {
            y = ((y % h) + h) % h;
            if (x < 0 || x >= w) return reinterpret_cast<const quint8 *>(m_transparent.constData());
        } else {
            x = ((x % w) + w) % w;
            if (y < 0 || y >= h) return reinterpret_cast<const quint8 *>(m_transparent.constData());
        }
        return reinterpret_cast<const quint8 *>(pixels.constData()) + (y * w + x) * pixelSize;
    };

    const quint8 *colors[4] = {fetch(x0, y0), fetch(x0 + 1, y0), fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1)};
    qint16 weights[4] = {qint16(qRound((1 - wx) * (1 - wy) * 255)), qint16(qRound(wx * (1 - wy) * 255)),
                         qint16(qRound((1 - wx) * wy * 255)), 0};
    weights[3] = 255 - weights[0] - weights[1] - weights[2];
    cs->mixColorsOp()->mixColors(colors, weights, 4, dst, 255);
    return true;
}

void CspRibbonBuilder::renderQuad(const Edge &a, const Edge &b, const KoColorSpace *cs, KisRandomSourceSP random,
                                  KisFixedPaintDeviceSP *dab, QRect *rect)
{
    *dab = nullptr;
    *rect = QRect();

    const QPolygonF quad({a.left, a.right, b.right, b.left});
    // a quad with no area (a sample that did not move across) draws nothing
    if (qAbs(cross(a.right - a.left, b.left - a.left)) + qAbs(cross(b.right - b.left, b.right - a.right)) < 0.02) return;
    const InverseBilinear inverse(a.left, a.right, b.right, b.left);

    const QRect bounds = quad.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1);
    KisFixedPaintDeviceSP device = new KisFixedPaintDevice(cs);
    device->setRect(bounds);
    device->lazyGrowBufferWithoutInitialization();
    const int pixelSize = cs->pixelSize();
    memset(device->data(), 0, bounds.width() * bounds.height() * pixelSize);

    // allow about one pixel beyond the sides for antialiasing
    const qreal width = qMax(1.0, KisAlgebra2D::norm(a.right - a.left));
    const qreal margin = 1.0 / width;
    // and half a pixel past the ends: rounding must not leave gaps between
    // quads; the overlap does not show, overlapping coverage is not added up
    const qreal length = qMax(0.5, 0.5 * (KisAlgebra2D::norm(b.left - a.left) + KisAlgebra2D::norm(b.right - a.right)));
    const qreal endTolerance = 0.5 / length;
    const bool pingPong = m_params.repeatMethod == CspBrushParams::Reverse && m_tips->count() == 1;

    quint8 *row = device->data();
    for (int y = bounds.top(); y <= bounds.bottom(); y++, row += bounds.width() * pixelSize) {
        quint8 *pixel = row;
        for (int x = bounds.left(); x <= bounds.right(); x++, pixel += pixelSize) {
            qreal su = 0, sv = 0;
            if (!inverse.map(QPointF(x + 0.5, y + 0.5), &su, &sv)) continue;
            if (sv < -endTolerance || sv > 1.0 + endTolerance) continue;
            if (su < -margin || su > 1.0 + margin) continue;

            const qreal v = a.v + (b.v - a.v) * sv;
            const int tile = int(std::floor(v));
            const int tip = tipForTile(tile, random);
            if (tip < 0) continue;
            sampleTip(tip, pingPong && (tile % 2), su, v - tile, cs, pixel);
        }
    }

    *dab = device;
    *rect = bounds;
}
