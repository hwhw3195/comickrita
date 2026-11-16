/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_SHAPE_FACTORY_H
#define VSTROKE_SHAPE_FACTORY_H

#include <KoShapeFactoryBase.h>

#include "kritavstroke_export.h"

/**
 * Registers for the SVG <path> tag. SvgParser offers every path to it, but
 * VStrokeShape::loadSvg() accepts only krita:type="vstroke" and the parser
 * falls back to a plain path for the rest.
 */
class KRITAVSTROKE_EXPORT VStrokeShapeFactory : public KoShapeFactoryBase
{
public:
    VStrokeShapeFactory();

    KoShape *createDefaultShape(KoDocumentResourceManager *documentResources = 0) const override;
    bool supports(const QDomElement &element, KoShapeLoadingContext &context) const override;
};

#endif // VSTROKE_SHAPE_FACTORY_H
