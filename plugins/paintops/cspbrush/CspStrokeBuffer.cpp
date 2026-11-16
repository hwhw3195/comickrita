/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspStrokeBuffer.h"

#include <QtMath>

#include <KoColorSpace.h>
#include <KoCompositeOp.h>
#include <KoColorSpaceRegistry.h>
#include <KoMixColorsOp.h>
#include <KoCompositeOpRegistry.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <kis_fixed_paint_device.h>
#include <kis_painter.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>

#include "CspColorMixer.h"

namespace {
// box blur of radius r in one direction; two passes in each direction
// approximate a gaussian well enough for an edge band
void boxBlur(QVector<float> &v, int w, int h, int r, bool horizontal)
{
    const int n = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    QVector<float> line(n);
    for (int l = 0; l < lines; l++) {
        auto at = [&](int i) -> float & { return horizontal ? v[l * w + i] : v[i * w + l]; };
        for (int i = 0; i < n; i++) line[i] = at(i);
        float sum = 0;
        for (int i = -r; i < n; i++) {
            const int add = i + r;
            const int remove = i - r - 1;
            if (add < n) sum += line[add];
            if (remove >= 0) sum -= line[remove];
            if (i >= 0) at(i) = sum / (2 * r + 1); // outside the area counts as transparent
        }
    }
}
}

CspStrokeBuffer::CspStrokeBuffer(const CspBrushParams &params, const KoColorSpace *cs)
    : m_params(params)
    , m_cs(cs)
    , m_stroke(new KisPaintDevice(cs))
    , m_painter(new KisPainter(m_stroke))
{
    m_painter->setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
}

CspStrokeBuffer::~CspStrokeBuffer()
{
}

int CspStrokeBuffer::margin(qreal radius) const
{
    return m_params.waterEdge ? qCeil(2 * radius) + 1 : 0;
}

QVector<float> CspStrokeBuffer::edgeStrength(const QVector<float> &alpha, int w, int h, int radius)
{
    QVector<float> blurred = alpha;
    const int r = qMax(1, radius / 2);
    for (int pass = 0; pass < 2; pass++) {
        boxBlur(blurred, w, h, r, true);
        boxBlur(blurred, w, h, r, false);
    }
    QVector<float> edge(alpha.size());
    for (int i = 0; i < alpha.size(); i++) {
        // inside a solid area the blur equals the alpha; next to the border the
        // blur drops, up to half of it right at the edge
        edge[i] = alpha[i] > 0 ? qBound(0.0f, 2.0f * (alpha[i] - blurred[i]) / qMax(alpha[i], 1e-3f), 1.0f) : 0.0f;
    }
    return edge;
}

void CspStrokeBuffer::flush(const QRect &changed, KisPainter *target)
{
    const qreal radius = m_params.waterEdgeRadius * KisLodTransform::lodToScale(target->device());
    const int m = margin(radius);
    const QRect out = changed.adjusted(-m, -m, m, m);
    KisPaintDeviceSP result = new KisPaintDevice(m_cs);

    if (!m_params.waterEdge) {
        KisPainter::copyAreaOptimized(out.topLeft(), m_stroke, result, out);
    } else {
        // the edge of a pixel depends on alpha up to 2r away
        const int r = qCeil(radius);
        const QRect area = out.adjusted(-2 * r, -2 * r, 2 * r, 2 * r);
        const int pixelSize = m_cs->pixelSize();
        QByteArray pixels(area.width() * area.height() * pixelSize, 0);
        quint8 *data = reinterpret_cast<quint8 *>(pixels.data());
        m_stroke->readBytes(data, area);

        QVector<float> alpha(area.width() * area.height());
        for (int i = 0; i < alpha.size(); i++) alpha[i] = m_cs->opacityF(data + i * pixelSize);
        const QVector<float> edge = edgeStrength(alpha, area.width(), area.height(), 2 * r);

        // CSP: "Opacity" makes the border darker than the inside (the inside
        // gets more transparent), "Darkness" darkens the border's colour
        QVector<quint16> lab(4);
        for (int i = 0; i < alpha.size(); i++) {
            if (alpha[i] <= 0) continue;
            quint8 *px = data + i * pixelSize;
            const float e = edge[i];
            const float a = alpha[i] * (1.0f - float(m_params.waterEdgeAlphaPower) * (1.0f - e));
            if (m_params.waterEdgeValuePower > 0 && e > 0) {
                m_cs->toLabA16(px, reinterpret_cast<quint8 *>(lab.data()), 1);
                lab[0] = quint16(lab[0] * (1.0f - float(m_params.waterEdgeValuePower) * e));
                m_cs->fromLabA16(reinterpret_cast<const quint8 *>(lab.data()), px, 1);
            }
            m_cs->setOpacity(px, qreal(a), 1);
        }
        result->writeBytes(data, area);
        result->crop(out);
    }

    // the target area is cleared and replaced: the stroke so far, processed
    target->setOpacityToUnit();
    target->setFlow(1.0);
    target->renderDabWithMirroringNonIncremental(out, result);
}

CspMixedStrokeBuffer::CspMixedStrokeBuffer(const CspBrushParams &params, KisPainter *target)
    : m_params(params)
    , m_target(target)
    , m_compositeOpId(target->compositeOpId())
    , m_original(new KisPaintDevice(*target->device()))
    , m_working(new KisPaintDevice(*target->device()))
    , m_coverage(new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8()))
    , m_painter(new KisPainter(m_working))
{
    m_painter->copyMirrorInformationFrom(target);
}

CspMixedStrokeBuffer::~CspMixedStrokeBuffer()
{
}

void CspMixedStrokeBuffer::addCoverage(const QRect &rect, KisFixedPaintDeviceSP mask, qreal opacity, qreal flow)
{
    // like alpha darken: coverage climbs towards the dab's value at the flow's rate and never comes down,
    // so overlapping dabs don't go past the opacity
    const float k = float(qBound(0.0, opacity, 1.0));
    const float rate = float(qBound(0.0, flow, 1.0));
    KisFixedPaintDeviceSP m = new KisFixedPaintDevice(*mask);
    bool flippedH = false, flippedV = false;
    for (const CspColorMixer::MirrorCopy &copy : CspColorMixer::mirrorCopies(m_target, rect)) {
        if (copy.flipHorizontally != flippedH || copy.flipVertically != flippedV) {
            m->mirror(copy.flipHorizontally != flippedH, copy.flipVertically != flippedV);
            flippedH = copy.flipHorizontally;
            flippedV = copy.flipVertically;
        }
        const int n = copy.rect.width() * copy.rect.height();
        QVector<quint8> current(n);
        m_coverage->readBytes(current.data(), copy.rect);
        const quint8 *dab = m->data();
        for (int i = 0; i < n; i++) {
            const int target = qRound(dab[i] * k);
            if (target > current[i] && rate > 0.0f) {
                current[i] += quint8(qMax(1, qRound((target - current[i]) * rate)));
            }
        }
        m_coverage->writeBytes(current.constData(), copy.rect);
    }
}

void CspMixedStrokeBuffer::flush(const QRect &changed)
{
    const qreal radius = m_params.waterEdge ? m_params.waterEdgeRadius * KisLodTransform::lodToScale(m_target->device()) : 0.0;
    const int m = m_params.waterEdge ? qCeil(2 * radius) + 1 : 0;
    for (const QRect &rect : m_target->calculateAllMirroredRects(changed)) {
        flushRect(rect.adjusted(-m, -m, m, m), qCeil(radius));
    }
}

void CspMixedStrokeBuffer::flushRect(const QRect &out, int r)
{
    // the edge of a pixel depends on the coverage up to 2r away
    const QRect area = out.adjusted(-2 * r, -2 * r, 2 * r, 2 * r);
    QVector<quint8> coverageBytes(area.width() * area.height());
    m_coverage->readBytes(coverageBytes.data(), area);
    QVector<float> coverage(coverageBytes.size());
    for (int i = 0; i < coverage.size(); i++) coverage[i] = coverageBytes[i] / 255.0f;
    QVector<float> edge;
    if (m_params.waterEdge) edge = CspStrokeBuffer::edgeStrength(coverage, area.width(), area.height(), 2 * r);

    const KoColorSpace *cs = m_target->device()->colorSpace();
    const int pixelSize = cs->pixelSize();
    const int n = out.width() * out.height();
    QByteArray original(n * pixelSize, 0);
    QByteArray working(n * pixelSize, 0);
    m_original->readBytes(reinterpret_cast<quint8 *>(original.data()), out);
    m_working->readBytes(reinterpret_cast<quint8 *>(working.data()), out);

    // the selection goes into the blend, so rewriting an area never compounds it
    QVector<quint8> selection(n, 255);
    if (KisSelectionSP s = m_target->selection()) s->projection()->readBytes(selection.data(), out);

    KisFixedPaintDeviceSP result = new KisFixedPaintDevice(cs);
    result->setRect(out);
    result->lazyGrowBufferWithoutInitialization();
    quint8 *dst = result->data();
    QVector<quint16> lab(4);
    // Normal and Erase are the mix itself; other modes compose the mixed pixels below
    const bool composes = m_compositeOpId != COMPOSITE_OVER && m_compositeOpId != COMPOSITE_ERASE;
    QVector<quint8> composeWeights(composes ? n : 0, 0);
    for (int y = 0; y < out.height(); y++) {
        for (int x = 0; x < out.width(); x++) {
            const int i = y * out.width() + x;
            const int a = (y + out.top() - area.top()) * area.width() + (x + out.left() - area.left());
            quint8 *orig = reinterpret_cast<quint8 *>(original.data()) + i * pixelSize;
            quint8 *mixed = reinterpret_cast<quint8 *>(working.data()) + i * pixelSize;
            const float c = coverage[a];
            const float e = m_params.waterEdge ? edge[a] : 1.0f;
            if (c <= 0 || selection[i] == 0) {
                memcpy(dst + i * pixelSize, orig, pixelSize);
                continue;
            }
            // CSP: "Opacity" lets the inside show the original more, "Darkness" darkens the border
            if (m_params.waterEdge && m_params.waterEdgeValuePower > 0 && e > 0) {
                cs->toLabA16(mixed, reinterpret_cast<quint8 *>(lab.data()), 1);
                lab[0] = quint16(lab[0] * (1.0f - float(m_params.waterEdgeValuePower) * e));
                cs->fromLabA16(reinterpret_cast<const quint8 *>(lab.data()), mixed, 1);
            }
            const float weight = c * (1.0f - float(m_params.waterEdgeAlphaPower) * (1.0f - e)) * selection[i] / 255.0f;
            const qint16 w = qint16(qRound(qBound(0.0f, weight, 1.0f) * 255.0f));
            if (composes) {
                composeWeights[i] = quint8(w);
                continue;
            }
            const quint8 *colors[2] = {orig, mixed};
            const qint16 weights[2] = {qint16(255 - w), w};
            cs->mixColorsOp()->mixColors(colors, weights, 2, dst + i * pixelSize, 255);
        }
    }

    if (composes) {
        // always from the original, so one stroke doesn't compound
        memcpy(dst, original.constData(), n * pixelSize);
        const int stride = out.width() * pixelSize;
        cs->compositeOp(m_compositeOpId)->composite(dst, stride, reinterpret_cast<const quint8 *>(working.constData()), stride,
                                                    composeWeights.constData(), out.width(), out.height(), out.width(),
                                                    1.0f, m_target->channelFlags());
        // some modes change the layer even where the source is transparent
        for (int i = 0; i < n; i++) {
            if (!composeWeights[i]) memcpy(dst + i * pixelSize, original.constData() + i * pixelSize, pixelSize);
        }
    }

    // the layer's own pixels outside the stroke are rewritten unchanged; alpha lock still applies
    KisPainter writer(m_target->device());
    writer.setChannelFlags(m_target->channelFlags());
    writer.setCompositeOpId(COMPOSITE_COPY);
    writer.bltFixed(out.topLeft(), result, result->bounds());
    m_target->addDirtyRect(out);
}
