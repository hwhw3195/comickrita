/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeShapeFactory.h"

#include <KoDocumentResourceManager.h>
#include <KoXmlNS.h>
#include <klocalizedstring.h>

#include "VStrokeShape.h"

VStrokeShapeFactory::VStrokeShapeFactory()
    : KoShapeFactoryBase(VStrokeShapeId, i18n("Vector Brush Stroke"))
{
    setXmlElementNames(KoXmlNS::svg, QStringList("path"));
    setHidden(true); // created by the vector pen tool only
}

KoShape *VStrokeShapeFactory::createDefaultShape(KoDocumentResourceManager *documentResources) const
{
    VStrokeShape *shape = new VStrokeShape();
    if (documentResources && documentResources->hasResource(KoDocumentResourceManager::DocumentResolution)) {
        shape->setPixelsPerPoint(documentResources->documentResolution() / 72.0);
    }
    return shape;
}

bool VStrokeShapeFactory::supports(const QDomElement &element, KoShapeLoadingContext &context) const
{
    Q_UNUSED(context);
    return element.tagName() == "path" && element.attribute("krita:type") == "vstroke";
}
