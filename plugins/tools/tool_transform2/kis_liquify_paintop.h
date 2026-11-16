/*
 *  SPDX-FileCopyrightText: 2014 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_LIQUIFY_PAINTOP_H
#define __KIS_LIQUIFY_PAINTOP_H

#include <QScopedPointer>
#include <QVector>

#include "KisShapeLayerHooks.h"

class KisLiquifyTransformWorker;
class KisPaintInformation;
class KisSpacingInformation;
class KisTimingInformation;
class KisDistanceInformation;
class KisLiquifyProperties;
class QPainterPath;


class KisLiquifyPaintop
{
public:
    /// \p dabs, when set, collects the Move and Offset dabs for the vector layers Liquify moves;
    /// without \p moveGrid, those dabs leave \p worker's grid alone: nothing reads it
    KisLiquifyPaintop(const KisLiquifyProperties &props,
                      KisLiquifyTransformWorker *worker,
                      QVector<KisShapeLayerHooks::LiquifyDab> *dabs = nullptr,
                      bool moveGrid = true);
    ~KisLiquifyPaintop();

    KisSpacingInformation paintAt(const KisPaintInformation &pi);

    /**
     * Updates the spacing in currentDistance based on the provided information.
     */
    void updateSpacing(const KisPaintInformation &info, KisDistanceInformation &currentDistance)
        const;

    /**
     * Updates the timing in currentDistance based on the provided information.
     */
    void updateTiming(const KisPaintInformation &info, KisDistanceInformation &currentDistance)
        const;

    KisTimingInformation updateTimingImpl(const KisPaintInformation &pi) const;

    static QPainterPath brushOutline(const KisLiquifyProperties &props, const KisPaintInformation &info);

    /// how far from the brush's center a dab of \p props moves anything, in image pixels
    static qreal effectReach(const KisLiquifyProperties &props);

protected:
    KisSpacingInformation updateSpacingImpl(const KisPaintInformation &pi) const;

private:
    qreal computeSize(const KisPaintInformation &pi) const;

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif /* __KIS_LIQUIFY_PAINTOP_H */
