/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VFreehandLayer.h"
#include "VDeform.h"
#include "VLiquify.h"

#include <QApplication>
#include <QHash>
#include <QThread>
#include <QtMath>

#include <KoPathShape.h>
#include <KoProperties.h>
#include <KoShapeContainer.h>
#include <KoShapeGroup.h>
#include <commands/KoShapeCreateCommand.h>
#include <commands/KoShapeDeleteCommand.h>
#include <kundo2command.h>
#include <klocalizedstring.h>

#include <KisSafeBlockingQueueConnectionProxy.h>
#include <KisShapeLayerHooks.h>
#include <kis_node.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_random_accessor_ng.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>

#include "VCurveConverter.h"
#include "VCurveShape.h"
#include "VCurveTrim.h"
#include "VGeomCurveShape.h"
#include "VSelectionSplit.h"
#include "VStrokeShape.h"
#include "VSelectionCover.h"

const char *const VFreehandLayer::Property = "freehandvector";

namespace {

class SetFlagCommand : public KUndo2Command
{
public:
    SetFlagCommand(KisNodeSP node, bool value, KUndo2Command *parent)
        : KUndo2Command(parent), m_node(node), m_value(value) {}

    void redo() override { VFreehandLayer::setFreehand(m_node.data(), m_value); }
    void undo() override { VFreehandLayer::setFreehand(m_node.data(), !m_value); }

private:
    KisNodeSP m_node;
    bool m_value;
};

/// the brush strokes in \p shapes, in groups too
void collectStrokes(const QList<KoShape*> &shapes, QList<VStrokeShape*> *strokes)
{
    for (KoShape *shape : shapes) {
        if (VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(shape)) {
            *strokes << stroke;
        } else if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
            collectStrokes(group->shapes(), strokes);
        }
    }
}

/// multiplies the brush sizes of a layer's strokes; shapes change in the GUI thread only, as KisShapeLayer::transform() does
class ScaleBrushSizesCommand : public KUndo2Command
{
public:
    ScaleBrushSizesCommand(KisShapeLayer *layer, qreal factor)
        : m_layer(layer),
          m_factor(factor),
          m_connection([this] (bool redo) { apply(redo); })
    {
    }

    void redo() override { run(true); }
    void undo() override { run(false); }

private:
    void run(bool redo)
    {
        if (QThread::currentThread() == qApp->thread()) {
            apply(redo);
        } else {
            m_connection.start(redo);
        }
    }

    void apply(bool redo)
    {
        if (!redo) {
            for (auto it = m_before.constBegin(); it != m_before.constEnd(); ++it) {
                it.key()->setCurvesAndUpdate(it.value());
            }
            m_before.clear();
            return;
        }
        QList<VStrokeShape*> strokes;
        collectStrokes(m_layer->shapes(), &strokes);
        for (VStrokeShape *stroke : strokes) {
            m_before.insert(stroke, stroke->curves());
            VCurveData scaled = stroke->curves();
            for (qreal &radius : scaled.radii) radius *= m_factor;
            stroke->setCurvesAndUpdate(scaled);
        }
    }

    KisShapeLayerSP m_layer;
    const qreal m_factor;
    QHash<VStrokeShape*, VCurveData> m_before;
    KisSafeBlockingQueueConnectionProxy<bool> m_connection;
};

/// cuts \p shape down to where \p selection is set; false when none of it is left
bool keepShapeWithin(KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels)
{
    if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
        Q_FOREACH (KoShape *child, group->shapes()) {
            if (!keepShapeWithin(child, selection, documentToPixels)) {
                group->removeShape(child);
                delete child;
            }
        }
        return !group->shapes().isEmpty();
    }

    VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
    if (!curve) {
        const QTransform toPixels = shape->absoluteTransformation() * documentToPixels;
        return toPixels.mapRect(shape->outlineRect()).toAlignedRect().intersects(selection->selectedExactRect());
    }

    const VCurveData after = VSelectionSplit::within(curve, selection, documentToPixels);
    if (after.isEmpty()) return false;
    if (!(after == curve->curves())) curve->setCurvesAndUpdate(after);
    return true;
}

}

bool VFreehandLayer::keepWithinSelection(KisShapeLayer *layer, KisSelectionSP selection)
{
    if (!isFreehand(layer) || !selection) return false;
    KisImageSP image = layer->image();
    if (!image) return false;

    const QTransform documentToPixels = QTransform::fromScale(image->xRes(), image->yRes());
    Q_FOREACH (KoShape *shape, layer->shapes()) {
        if (!keepShapeWithin(shape, selection, documentToPixels)) {
            layer->removeShape(shape);
            delete shape;
        }
    }
    return true;
}

bool VFreehandLayer::isFreehand(const KisNode *node)
{
    return node && node->inherits("KisShapeLayer") && node->nodeProperties().boolProperty(Property, false);
}

void VFreehandLayer::setFreehand(KisNode *node, bool freehand)
{
    if (node) node->setNodeProperty(Property, freehand);
}

bool VFreehandLayer::containsCurves(const QList<KoShape*> &shapes)
{
    for (KoShape *shape : shapes) {
        if (dynamic_cast<VCurveShape*>(shape)) return true;
        if (KoShapeContainer *container = dynamic_cast<KoShapeContainer*>(shape)) {
            if (containsCurves(container->shapes())) return true;
        }
    }
    return false;
}

void VFreehandLayer::installHooks()
{
    static bool installed = false;
    if (installed) return;
    installed = true;

    KisShapeLayerHooks::setConvertShapes([] (QList<KoShape*> &shapes, KoShapeContainer *parent) {
        if (isFreehand(dynamic_cast<KisShapeLayer*>(parent))) {
            VCurveConverter::convertInPlace(shapes);
        }
    });
    KisShapeLayerHooks::setNeedsNewLayer([] (const QList<KoShape*> &shapes, KisShapeLayer *activeLayer) {
        return !isFreehand(activeLayer) && containsCurves(shapes);
    });
    KisShapeLayerHooks::setInitNewLayer([] (KisShapeLayer *layer, const QList<KoShape*> &shapes) {
        if (!containsCurves(shapes)) return;
        setFreehand(layer);
        KisNode *node = layer;
        node->setName(i18nc("@item:inlistbox a layer name; %1 is the vector layer name", "Freehand %1", node->name()));
    });
    KisShapeLayerHooks::setClearCommand([] (KisShapeLayer *layer, KoShapeControllerBase *controller, KisSelectionSP selection) -> KUndo2Command* {
        if (selection && isFreehand(layer)) return VSelectionSplit::cutCommand(layer, controller, selection);
        const QList<KoShape*> shapes = VSelectionCover::shapesToClear(layer, selection);
        if (shapes.isEmpty() || !controller) return nullptr;
        return new KoShapeDeleteCommand(controller, shapes);
    });
    KisShapeLayerHooks::setLayerIcon([] (const KisShapeLayer *layer) {
        return isFreehand(layer) ? QStringLiteral("freehandVectorLayer") : QString();
    });
    KisShapeLayerHooks::setKeepWithinSelection(&VFreehandLayer::keepWithinSelection);
    KisShapeLayerHooks::setCutWithinSelection(&VSelectionSplit::cutCommand);
    KisShapeLayerHooks::setSelectionMove(&VSelectionSplit::moveCommand);
    KisShapeLayerHooks::setBrushSizeHooks([] (const KisNode *node) {
        const KisShapeLayer *layer = dynamic_cast<const KisShapeLayer*>(node);
        if (!isFreehand(layer)) return false;
        QList<VStrokeShape*> strokes;
        collectStrokes(layer->shapes(), &strokes);
        return !strokes.isEmpty();
    }, [] (KisShapeLayer *layer, const QTransform &t) -> KUndo2Command* {
        if (!isFreehand(layer)) return nullptr;
        // the area's scale, as a stroke's width has one number; the affine part, as the layer can't do perspective
        const qreal factor = VSelectionSplit::brushScale(t);
        if (qFuzzyCompare(factor, 1.0) || factor <= 0) return nullptr;
        return new ScaleBrushSizesCommand(layer, factor);
    });
    VLiquify::installHooks();
    VDeform::installHooks();
}

KUndo2Command *VFreehandLayer::convertCommand(KisShapeLayer *layer, KoShapeControllerBase *controller)
{
    if (!layer || !controller || isFreehand(layer)) return nullptr;

    KUndo2Command *command = new KUndo2Command(kundo2_i18n("Convert to Freehand Vector Layer"));
    new SetFlagCommand(layer, true, command);

    QList<KoShape*> paths;
    QList<KoShape*> curves;
    for (KoShape *shape : layer->shapes()) {
        if (VGeomCurveShape *curve = VCurveConverter::fromPath(dynamic_cast<KoPathShape*>(shape))) {
            // shapes go into a vector layer in document coordinates
            curve->setTransformation(shape->absoluteTransformation());
            paths << shape;
            curves << curve;
        }
    }
    if (!paths.isEmpty()) {
        new KoShapeDeleteCommand(controller, paths, command);
        new KoShapeCreateCommand(controller, curves, layer, command);
    }
    return command;
}
