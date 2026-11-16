/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KConfigGroup>
#include <KSharedConfig>
#include <KisActionPlugin.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeGroup.h>
#include <kis_canvas2.h>
#include <KisDocument.h>
#include <KisViewManager.h>
#include <kis_action.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node_commands_adapter.h>
#include <kis_node_manager.h>
#include <kis_processing_applicator.h>
#include <kis_shape_layer.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include "VCurveShape.h"
#include "VEditActions.h"
#include "VFreehandLayer.h"

/**
 * The freehand vector layer actions: Layer > New > Freehand Vector Layer
 * and Layer > Convert > to Freehand Vector Layer (also in the Layers
 * docker's menus), and Apply Current Brush to Selected Strokes (no menu:
 * a shortcut, or Edit Shapes' Brush page).
 */
class VStrokeActions : public KisActionPlugin
{
    Q_OBJECT
public:
    VStrokeActions(QObject *parent, const QVariantList &)
        : KisActionPlugin(parent)
    {
        connect(createAction("add_new_freehand_vector_layer"), &KisAction::triggered,
                this, &VStrokeActions::slotAddLayer);
        connect(createAction("convert_to_freehand_vector_layer"), &KisAction::triggered,
                this, &VStrokeActions::slotConvert);
        connect(createAction("apply_current_brush_to_strokes"), &KisAction::triggered,
                this, &VStrokeActions::slotApplyBrush);
    }

private Q_SLOTS:
    void slotAddLayer()
    {
        KisViewManager *view = viewManager();
        if (!view || !view->document() || !view->image()) return;
        KisImageSP image = view->image();
        if (!view->blockUntilOperationsFinished(image)) return;

        // the flag and name are set before the layer is added: the add runs
        // in a stroke, and a change to a layer the Layers docker hasn't
        // heard of yet trips an assert there
        KisShapeLayerSP layer = new KisShapeLayer(view->document()->shapeController(), image.data(),
                                                  i18nc("@item:inlistbox a layer name; %1 is the vector layer name", "Freehand %1",
                                                        image->nextLayerName(i18n("Vector Layer"))),
                                                  OPACITY_OPAQUE_U8);
        VFreehandLayer::setFreehand(layer.data());

        // where Layer > New > Vector Layer puts it (KisLayerManager::adjustLayerPosition)
        KisNodeSP parent = view->activeNode() ? view->activeNode() : image->root();
        KisNodeSP above = parent->lastChild();
        if (parent->inherits("KisGroupLayer") && parent->collapsed()) {
            above = parent;
            parent = parent->parent();
        } else {
            while (parent && (!parent->allowAsChild(layer) || !parent->isEditable(false))) {
                above = parent;
                parent = parent->parent();
            }
        }
        if (!parent) {
            parent = image->root();
            above = parent->lastChild();
        }
        KisGroupLayer *group = dynamic_cast<KisGroupLayer*>(parent.data());
        const bool update = group && !group->projectionIsValid();
        KisNodeCommandsAdapter(view).addNodeAsync(layer, parent, above, update, update);
    }

    void slotApplyBrush()
    {
        if (!viewManager() || !viewManager()->canvasBase()) return;
        KoCanvasBase *canvas = viewManager()->canvasBase();
        QList<VCurveShape*> shapes;
        std::function<void(KoShape*)> collect = [&] (KoShape *shape) {
            if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
                for (KoShape *child : group->shapes()) collect(child);
            } else if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) {
                shapes << curve;
            }
        };
        for (KoShape *shape : canvas->selectedShapesProxy()->selection()->selectedEditableShapes()) collect(shape);
        // Edit Shapes' Keep stroke size
        const bool keepSize = KSharedConfig::openConfig()->group("VPointEditTool/actions/brush").readEntry("keepSize", true);
        VEditActions::applyCurrentBrush(canvas, shapes, keepSize);
    }

    void slotConvert()
    {
        if (!viewManager()) return;
        KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(viewManager()->activeNode().data());
        if (!layer) return;
        if (VFreehandLayer::isFreehand(layer)) {
            viewManager()->showFloatingMessage(i18n("The layer is a freehand vector layer already."), QIcon());
            return;
        }
        KUndo2Command *command = VFreehandLayer::convertCommand(layer, viewManager()->document()->shapeController());
        KisProcessingApplicator::runSingleCommandStroke(viewManager()->image(), command,
                                                        KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    }
};

K_PLUGIN_FACTORY_WITH_JSON(VStrokeActionsFactory, "kritavstrokeactions.json", registerPlugin<VStrokeActions>();)

#include "VStrokeActions.moc"
