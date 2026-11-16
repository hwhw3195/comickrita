/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeToolSwitcher.h"

#include "VCurveShape.h"
#include "VFreehandLayer.h"

#include <KoCanvasBase.h>
#include <KoCanvasController.h>
#include <KoCanvasResourceProvider.h>
#include <KoCanvasResourcesIds.h>
#include <KoPathShape.h>
#include <KoShapeManager.h>
#include <KoSelection.h>
#include <KoToolManager.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <kis_node.h>

#include <QApplication>
#include <QButtonGroup>
#include <QPalette>
#include <QToolButton>
#include <QWidget>

const QString VStrokeToolSwitcher::BrushToolId = QStringLiteral("KritaShape/KisToolBrush");
const QString VStrokeToolSwitcher::VectorToolId = QStringLiteral("VStrokeTool");
const QString VStrokeToolSwitcher::ShapeSelectToolId = QStringLiteral("InteractionTool");
const QString VStrokeToolSwitcher::EditShapesToolId = QStringLiteral("PathTool");
const QString VStrokeToolSwitcher::PointEditToolId = QStringLiteral("VPointEditTool");
const QString VStrokeToolSwitcher::VectorEditToolId = QStringLiteral("VEditTool");

namespace {

/// the selection holds stock paths and no freehand curves
bool stockPathsOnly(KoCanvasBase *canvas)
{
    bool stock = false;
    Q_FOREACH (KoShape *shape, canvas->shapeManager()->selection()->selectedEditableShapes()) {
        if (dynamic_cast<VCurveShape*>(shape)) return false;
        stock |= bool(dynamic_cast<KoPathShape*>(shape));
    }
    return stock;
}

}

VStrokeToolSwitcher::VStrokeToolSwitcher(QObject *parent)
    : QObject(parent)
{
    // each window has its own tool manager
    connect(KisPart::instance(), &KisPart::sigViewAdded, this, &VStrokeToolSwitcher::slotViewAdded);
    Q_FOREACH (QPointer<KisView> view, KisPart::instance()->views()) {
        if (view) slotViewAdded(view);
    }
}

void VStrokeToolSwitcher::start()
{
    // KoPluginLoader deletes the plugin object right after loading, and
    // KisPart can't be created while the plugins load (the resource
    // servers aren't ready), so the switcher lives in KisPart from the
    // first time a canvas creates its tools
    static QPointer<VStrokeToolSwitcher> s_switcher;
    if (!s_switcher) {
        s_switcher = new VStrokeToolSwitcher(KisPart::instance());
    }
}

void VStrokeToolSwitcher::slotViewAdded(KisView *view)
{
    KoToolManager *manager = view ? KoToolManager::instance() : nullptr;
    if (!manager || m_managers.contains(manager)) return;

    m_managers.removeAll(QPointer<KoToolManager>());
    m_managers << manager;
    connect(manager, &KoToolManager::changedTool, this, [this, manager] (KoCanvasController *controller) {
        slotToolChanged(manager, controller);
    });
}

QString VStrokeToolSwitcher::switchTarget(const QString &activeToolId, KisNodeSP node, bool stockPathsSelected)
{
    const bool freehand = VFreehandLayer::isFreehand(node);
    const bool vector = node && node->inherits("KisShapeLayer");
    if (activeToolId == BrushToolId && freehand) return VectorToolId;
    if (activeToolId == VectorToolId && !freehand) {
        // a stock vector layer gets what stock Krita gives the brush there
        return vector ? ShapeSelectToolId : BrushToolId;
    }
    if (activeToolId == EditShapesToolId && freehand && !stockPathsSelected) return PointEditToolId;
    if (activeToolId == PointEditToolId && (!freehand || stockPathsSelected)) {
        // as stock Krita swaps Edit Shapes for the brush on pixel layers
        return vector ? EditShapesToolId : BrushToolId;
    }
    if (activeToolId == VectorEditToolId && !freehand) return vector ? ShapeSelectToolId : BrushToolId;
    return QString();
}

void VStrokeToolSwitcher::slotToolChanged(KoToolManager *manager, KoCanvasController *controller)
{
    // KisNodeManager::slotUiActivatedNode() swaps the brush for the shape
    // select tool when a vector layer is clicked, before the layer changes
    const QString toolId = manager->activeToolId();
    if (manager == m_manager && m_lastToolId == BrushToolId && toolId == ShapeSelectToolId) {
        m_brushReplaced = true;
    }
    m_manager = manager;
    m_lastToolId = toolId;

    // follow the current layer of the canvas whose tool changed
    KoCanvasResourceProvider *resources =
        controller && controller->canvas() ? controller->canvas()->resourceManager() : nullptr;
    if (resources != m_resources) {
        disconnect(m_resourcesConnection);
        m_resources = resources;
        if (resources) {
            m_resourcesConnection = connect(resources, &KoCanvasResourceProvider::canvasResourceChanged, this,
                                            [this] (int key, const QVariant &) {
                if (key == KoCanvasResource::CurrentKritaNode) {
                    m_nodeChanged = true;
                    scheduleCheck();
                }
            });
        }
    }

    if (controller && controller->canvas()) {
        followSelection(controller->canvas());
        // after KoToolBox::setActiveTool(), which handles the same signal
        QPointer<QWidget> widget = controller->canvas()->canvasWidget();
        QMetaObject::invokeMethod(this, [widget, toolId] () {
            if (widget) updateToolBoxButtons(widget->window(), toolId);
        }, Qt::QueuedConnection);
    }
    scheduleCheck();
}

QString VStrokeToolSwitcher::standInToolId(const QString &toolId)
{
    if (toolId == VectorToolId) return BrushToolId;
    if (toolId == PointEditToolId) return EditShapesToolId;
    return QString();
}

void VStrokeToolSwitcher::updateToolBoxButtons(QWidget *window, const QString &toolId)
{
    // The toolbox finds no button for the plugin tools and leaves the last
    // one checked: often shape select, which Krita picks on the way to a
    // vector layer. Check the stock button that stands for the tool. Toolbox
    // buttons are named after their tool id.
    if (!window) return;
    const QString standIn = standInToolId(toolId);
    QToolButton *button = window->findChild<QToolButton*>(standIn.isEmpty() ? toolId : standIn);
    if (!button || !button->group()) return;
    if (!standIn.isEmpty()) {
        button->setChecked(true); // the exclusive group unchecks the others
    }
    // as KoToolBoxButton::setHighlightColor(), for every button: the toolbox
    // only resets the one it checked last
    const QPalette app = qApp->palette();
    QPalette highlighted = app;
    highlighted.setColor(QPalette::Button, app.color(QPalette::Highlight));
    Q_FOREACH (QAbstractButton *b, button->group()->buttons()) {
        b->setPalette(b->isChecked() ? highlighted : app);
    }
}

void VStrokeToolSwitcher::followSelection(KoCanvasBase *canvas)
{
    // Edit Shapes follows the selection: that of the current layer's shape
    // manager, which changes with the layer (each change comes with a check)
    KoSelection *selection = canvas->shapeManager()->selection();
    if (selection == m_selection) return;
    disconnect(m_selectionConnection);
    m_selection = selection;
    m_selectionConnection = connect(selection, &KoSelection::selectionChanged, this, &VStrokeToolSwitcher::scheduleCheck);
}

void VStrokeToolSwitcher::scheduleCheck()
{
    // not from inside a tool change or a layer change
    if (m_checkScheduled) return;
    m_checkScheduled = true;
    QMetaObject::invokeMethod(this, [this] () {
        m_checkScheduled = false;
        check();
    }, Qt::QueuedConnection);
}

QString VStrokeToolSwitcher::effectiveToolId(const QString &activeToolId, bool brushReplaced, bool nodeChanged)
{
    // the brush was active when the layer was clicked: a shape select tool
    // picked by hand comes without a layer change
    if (activeToolId == ShapeSelectToolId && brushReplaced && nodeChanged) return BrushToolId;
    return activeToolId;
}

void VStrokeToolSwitcher::check()
{
    const bool brushReplaced = m_brushReplaced;
    const bool nodeChanged = m_nodeChanged;
    m_brushReplaced = false;
    m_nodeChanged = false;

    KoToolManager *manager = m_manager;
    if (!manager || !m_resources) return;
    KoCanvasController *controller = manager->activeCanvasController();
    if (!controller || !controller->canvas() || controller->canvas()->resourceManager() != m_resources) return;

    followSelection(controller->canvas());
    const KisNodeSP node = m_resources->resource(KoCanvasResource::CurrentKritaNode).value<KisNodeWSP>();
    updateVectorEditButtons(controller->canvas(), VFreehandLayer::isFreehand(node));
    const QString target = switchTarget(effectiveToolId(manager->activeToolId(), brushReplaced, nodeChanged), node,
                                        stockPathsOnly(controller->canvas()));
    if (!target.isEmpty()) {
        manager->switchToolRequested(target);
    }
}

void VStrokeToolSwitcher::updateVectorEditButtons(KoCanvasBase *canvas, bool enabled)
{
    // the button's visibility code is flake/always, which the toolbox
    // leaves alone on layer and selection changes; its object name is the tool id
    QWidget *widget = canvas->canvasWidget();
    QWidget *window = widget ? widget->window() : nullptr;
    if (!window) return;
    Q_FOREACH (QToolButton *button, window->findChildren<QToolButton*>(VectorEditToolId)) {
        button->setEnabled(enabled);
    }
}
