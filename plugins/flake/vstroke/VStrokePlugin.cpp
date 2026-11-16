/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokePlugin.h"
#include "VTrace.h"

#include <KoShapeRegistry.h>
#include <KoToolRegistry.h>
#include <kpluginfactory.h>

#include "VEditTool.h"
#include "VFreehandLayer.h"
#include "VGeomCurveShape.h"
#include "VPointEditTool.h"
#include "VStrokeShapeFactory.h"
#include "VStrokeTool.h"

K_PLUGIN_FACTORY_WITH_JSON(VStrokePluginFactory, "krita_vstroke.json", registerPlugin<VStrokePlugin>();)

/**
 * A tool plugin, so the shape and the tool come from one module. Krita
 * loads tool plugins after the shape registry, so the shape factory goes
 * in with addFactory(), which also maps the SVG element names.
 */
VStrokePlugin::VStrokePlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KoShapeRegistry::instance()->addFactory(new VStrokeShapeFactory());
    KoShapeRegistry::instance()->addFactory(new VGeomCurveShapeFactory());
    KoToolRegistry::instance()->add(new VStrokeToolFactory());
    KoToolRegistry::instance()->add(new VPointEditToolFactory());
    KoToolRegistry::instance()->add(new VEditToolFactory());
    // only sets functions: safe while plugins load
    VFreehandLayer::installHooks();
    // the lag log (KRITA_VSTROKE_TRACE); plugins load in the GUI thread
    VTrace::start();
}

#include "VStrokePlugin.moc"
