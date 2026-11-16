/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KOSTROKEFEATURES_H
#define KOSTROKEFEATURES_H

#include <QColor>
#include <QFlags>

#include "kritaflake_export.h"

/**
 * Implemented by shapes that follow only some of the options of KoShapeStroke.
 * The Stroke tab greys out what a selected shape doesn't follow; shapes
 * without this interface follow them all.
 */
class KRITAFLAKE_EXPORT KoStrokeFeatures
{
public:
    enum Feature {
        Width = 1,
        CapAndJoin = 2,
        LineStyle = 4,
        Markers = 8,
        Gradient = 16,
        All = Width | CapAndJoin | LineStyle | Markers | Gradient
    };
    Q_DECLARE_FLAGS(Features, Feature)

    virtual ~KoStrokeFeatures();

    virtual Features strokeFeatures() const = 0;

    /**
     * The color a shape keeps while it has no fill or line, for the Solid
     * button to bring back. Invalid for shapes that don't keep one.
     */
    virtual QColor hiddenColor() const { return QColor(); }
};

Q_DECLARE_OPERATORS_FOR_FLAGS(KoStrokeFeatures::Features)

#endif // KOSTROKEFEATURES_H
