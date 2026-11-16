/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VPointEditTool.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <tuple>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KisHandlePainterHelper.h>
#include <KisHandleStyle.h>
#include <KisViewManager.h>
#include <KoCanvasBase.h>
#include <KoDrag.h>
#include <KoShapeController.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeManager.h>
#include <KoColorDisplayRendererInterface.h>
#include <KoIcon.h>
#include <KoPointerEvent.h>
#include <KoViewConverter.h>
#include <kis_canvas2.h>
#include <kis_icon_utils.h>
#include <kis_shape_layer.h>
#include <kis_slider_spin_box.h>
#include <kis_tool.h>
#include <kis_tool_utils.h>
#include <kis_floating_message.h>
#include <kis_action.h>
#include <kis_action_registry.h>
#include <klocalizedstring.h>

#include "VCurveShape.h"
#include "VFreehandLayer.h"
#include "VPointEdit.h"
#include "VStrokeToolSwitcher.h"

namespace {
/// how close to a point a click must be, in screen pixels
const qreal GrabDistance = 8.0;
/// points closer than this on screen are thinned out, except selected ones
const qreal MinPointSpacing = 6.0;
/// the brush size shortcuts step the falloff as they step brush sizes
KisToolUtils::StandardBrushSizes s_falloffSteps(0, 2000);
}

VPointEditTool::VPointEditTool(KoCanvasBase *canvas)
    : KoToolBase(canvas),
      m_actions(canvas, &m_actionSettings),
      m_contextMenu(new QMenu())
{
    loadActionSettings();
}

VPointEditTool::~VPointEditTool()
{
}

KisShapeLayer *VPointEditTool::layer() const
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    if (!kisCanvas || !kisCanvas->viewManager()) return nullptr;
    KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(kisCanvas->viewManager()->activeNode().data());
    return VFreehandLayer::isFreehand(layer) ? layer : nullptr;
}

QList<VCurveShape*> VPointEditTool::targets() const
{
    // as Edit Shapes: the points of the selected objects only
    QList<VCurveShape*> result;
    KisShapeLayer *l = layer();
    if (!l) return result;
    Q_FOREACH (KoShape *shape, canvas()->selectedShapesProxy()->selection()->selectedEditableShapes()) {
        VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
        if (curve && curve->isVisible() && l->shapes().contains(curve)) result << curve;
    }
    return result;
}

QPointF VPointEditTool::falloffCenter() const
{
    if (m_falloffRadius <= 0) return QPointF();
    if (m_drag == Drag::Move) return m_grabbed + (m_dragNow - m_dragStart);
    if (m_drag == Drag::Radius) return m_grabbed;
    if (m_drag == Drag::None && m_hovering) return m_hovered;
    return QPointF();
}

QRectF VPointEditTool::decorationsRect() const
{
    const qreal handle = canvas()->viewConverter()->viewToDocumentX(handleRadius() + decorationThickness() + 2);
    QRectF rect;
    Q_FOREACH (VCurveShape *shape, targets()) {
        rect |= shape->absoluteTransformation().mapRect(shape->curves().controlBounds());
    }
    if (m_drag == Drag::Area || m_drag == Drag::Polygon) {
        rect |= selectionArea(true).boundingRect();
    }
    const QPointF center = falloffCenter();
    if (!center.isNull()) {
        rect |= QRectF(center - QPointF(m_falloffRadius, m_falloffRadius),
                       QSizeF(2 * m_falloffRadius, 2 * m_falloffRadius));
    }
    return rect.isNull() ? rect : rect.adjusted(-handle, -handle, handle, handle);
}

void VPointEditTool::dropStaleSelections()
{
    const QList<VCurveShape*> shapes = targets();
    for (auto it = m_selected.begin(); it != m_selected.end();) {
        // shapes undone away, or with points added or removed since
        if (!shapes.contains(it.key()) || it.value().size() != it.key()->curves().pointCount()) {
            it = m_selected.erase(it);
        } else {
            ++it;
        }
    }
}

QVector<bool> &VPointEditTool::selection(VCurveShape *shape)
{
    QVector<bool> &s = m_selected[shape];
    if (s.size() != shape->curves().pointCount()) {
        s = QVector<bool>(shape->curves().pointCount(), false);
    }
    return s;
}

bool VPointEditTool::isSelected(VCurveShape *shape, int index) const
{
    auto it = m_selected.constFind(shape);
    return it != m_selected.constEnd() && index < it->size() && (*it)[index];
}

QVector<int> VPointEditTool::selectedPoints(VCurveShape *shape) const
{
    QVector<int> result;
    auto it = m_selected.constFind(shape);
    if (it == m_selected.constEnd()) return result;
    for (int i = 0; i < it->size(); i++) {
        if ((*it)[i]) result << i;
    }
    return result;
}

void VPointEditTool::selectPoint(VCurveShape *shape, int index)
{
    repaintDecorations();
    m_selected.clear();
    selection(shape)[index] = true;
    repaintDecorations();
}

void VPointEditTool::selectPoints(VCurveShape *shape, const QVector<int> &indices)
{
    repaintDecorations();
    QVector<bool> &s = selection(shape);
    for (int i : indices) s[i] = true;
    repaintDecorations();
}

bool VPointEditTool::singleSelectedPoint(VCurveShape **shape, int *index) const
{
    VCurveShape *found = nullptr;
    int foundIndex = -1;
    const QList<VCurveShape*> shapes = targets();
    for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
        if (!shapes.contains(it.key()) || it->size() != it.key()->curves().pointCount()) continue;
        for (int i = 0; i < it->size(); i++) {
            if (!(*it)[i]) continue;
            if (found) return false;
            found = it.key();
            foundIndex = i;
        }
    }
    if (!found) return false;
    if (shape) *shape = found;
    if (index) *index = foundIndex;
    return true;
}

void VPointEditTool::setSelectedPointType(VPointEdit::PointType type)
{
    dropStaleSelections();
    m_actions.begin();
    for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
        VCurveData data = it.key()->curves();
        for (int i = 0; i < it->size(); i++) {
            if ((*it)[i]) VPointEdit::setPointType(data, i, type);
        }
        if (data == it.key()->curves()) continue;
        m_actions.touch(it.key());
        it.key()->setCurvesAndUpdate(data);
    }
    m_actions.commit(kundo2_i18n("Change Point Type"));
    repaintDecorations();
}

QMenu *VPointEditTool::popupActionsMenu()
{
    if (!layer()) return nullptr;
    dropStaleSelections();
    // as Edit Shapes: a right-click on a point that isn't selected selects it
    if (m_hoveredHit.shape && targets().contains(m_hoveredHit.shape) &&
        m_hoveredHit.index < m_hoveredHit.shape->curves().pointCount() &&
        !isSelected(m_hoveredHit.shape, m_hoveredHit.index)) {
        selectPoint(m_hoveredHit.shape, m_hoveredHit.index);
    }
    bool anySelected = false;
    for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
        anySelected |= std::find(it->begin(), it->end(), true) != it->end();
    }
    // nothing to change: the right-click shows the popup palette as usual
    if (!anySelected) return nullptr;

    VCurveShape *single = nullptr;
    int singleIndex = -1;
    const bool one = singleSelectedPoint(&single, &singleIndex);
    const VPointEdit::PointType current = one ? VPointEdit::pointType(single->curves(), singleIndex)
                                              : VPointEdit::PointType::Line;

    m_contextMenu->clear();
    auto add = [this, one, current] (const QString &icon, const QString &text, VPointEdit::PointType type, bool checkable) {
        QAction *action = m_contextMenu->addAction(KisIconUtils::loadIcon(icon), text);
        action->setObjectName(icon);
        if (checkable) {
            action->setCheckable(true);
            action->setChecked(one && current == type);
        }
        connect(action, &QAction::triggered, this, [this, type] () { setSelectedPointType(type); });
    };
    add(QStringLiteral("pathpoint-corner"), i18n("Corner point"), VPointEdit::PointType::Corner, true);
    add(QStringLiteral("pathpoint-smooth"), i18n("Smooth point"), VPointEdit::PointType::Smooth, true);
    add(QStringLiteral("pathpoint-symmetric"), i18n("Symmetric Point"), VPointEdit::PointType::Symmetric, true);
    m_contextMenu->addSeparator();
    add(QStringLiteral("pathpoint-curve"), i18n("Make curve point"), VPointEdit::PointType::Curve, false);
    add(QStringLiteral("pathpoint-line"), i18n("Make line point"), VPointEdit::PointType::Line, false);
    return m_contextMenu.data();
}

VPointEditTool::Hit VPointEditTool::hitAt(const QPointF &documentPos) const
{
    const qreal maxDistance = canvas()->viewConverter()->viewToDocumentX(GrabDistance);
    Hit best;
    qreal bestDistance = maxDistance;
    Q_FOREACH (VCurveShape *shape, targets()) {
        const QTransform toDocument = shape->absoluteTransformation();
        const int i = VPointEdit::nearestPoint(shape->curves(), toDocument, documentPos, bestDistance);
        if (i >= 0) {
            const qreal distance = QLineF(toDocument.map(shape->curves().positions[i]), documentPos).length();
            if (!best.shape || distance <= bestDistance) {
                best = {shape, i};
                bestDistance = distance;
            }
        }
    }
    return best;
}

KoShape *VPointEditTool::shapeUnder(const QPointF &documentPos) const
{
    // KoShapeManager::shapeAt() goes by the bounding box for these shapes
    // (KoShape::hitTest), which picks a stroke from anywhere around it
    const qreal tolerance = canvas()->viewConverter()->viewToDocumentX(GrabDistance / 2);
    const QRectF around(documentPos - QPointF(tolerance, tolerance), QSizeF(2 * tolerance, 2 * tolerance));
    QList<KoShape*> shapes = canvas()->shapeManager()->shapesAt(around);
    std::sort(shapes.begin(), shapes.end(), KoShape::compareShapeZIndex);
    for (int i = shapes.size() - 1; i >= 0; i--) {
        KoShape *shape = shapes[i];
        if (!shape->isSelectable()) continue;
        VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
        if (!curve) {
            if (shape->hitTest(documentPos)) return shape;
            continue;
        }
        const QTransform toLocal = curve->absoluteTransformation().inverted();
        const QPainterPath outline = curve->outline();
        if (outline.contains(toLocal.map(documentPos)) || outline.intersects(toLocal.mapRect(around))) {
            return curve;
        }
    }
    return nullptr;
}

QPainterPath VPointEditTool::selectionArea(bool preview) const
{
    QPainterPath area;
    area.setFillRule(Qt::WindingFill);
    switch (m_selectionShape) {
    case SelectionShape::Rectangle:
        area.addRect(QRectF(m_dragStart, m_dragNow).normalized());
        break;
    case SelectionShape::Ellipse:
        area.addEllipse(QRectF(m_dragStart, m_dragNow).normalized());
        break;
    case SelectionShape::Lasso:
    case SelectionShape::Polygon: {
        QPolygonF points = m_areaPoints;
        if (preview && m_drag == Drag::Polygon) points << m_dragNow;
        area.addPolygon(points);
        area.closeSubpath();
        break;
    }
    }
    return area;
}

void VPointEditTool::selectPointsIn(const QPainterPath &area)
{
    Q_FOREACH (VCurveShape *shape, targets()) {
        const QVector<int> inside = VPointEdit::pointsIn(shape->curves(), shape->absoluteTransformation(), area);
        if (inside.isEmpty()) continue;
        QVector<bool> &s = selection(shape);
        for (int i : inside) s[i] = true;
    }
}

void VPointEditTool::finishPolygon()
{
    if (m_drag != Drag::Polygon) return;
    dropStaleSelections();
    if (m_areaPoints.size() >= 3) selectPointsIn(selectionArea(false));
    cancelPolygon();
}

void VPointEditTool::cancelPolygon()
{
    if (m_drag != Drag::Polygon) return;
    repaintDecorations();
    m_drag = Drag::None;
    m_areaPoints.clear();
    repaintDecorations();
}

void VPointEditTool::showMessage(const QString &message)
{
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->showFloatingMessage(message, KisIconUtils::loadIcon("object-locked"));
    }
}

void VPointEditTool::activate(const QSet<KoShape*> &shapes)
{
    KoToolBase::activate(shapes);
    useCursor(Qt::ArrowCursor);
    m_selected.clear();
    m_hovering = false;
    // the points shown follow the selected objects
    m_selectionConnection = connect(canvas()->selectedShapesProxy(), &KoSelectedShapesProxy::selectionChanged,
                                    this, [this] () { repaintDecorations(); });
    m_layerSelectionConnection = connect(canvas()->shapeManager()->selection(), &KoSelection::selectionChanged,
                                         this, [this] () { repaintDecorations(); });
    // the brush size shortcuts (remappable in the settings) size the falloff,
    // as they size the liquify brush; the falloff stays out of undo
    if (QAction *increase = action("increase_brush_size")) {
        m_increaseSizeConnection = connect(increase, &QAction::triggered, this, [this] () { changeFalloffRadius(true); });
    }
    if (QAction *decrease = action("decrease_brush_size")) {
        m_decreaseSizeConnection = connect(decrease, &QAction::triggered, this, [this] () { changeFalloffRadius(false); });
    }
    repaintDecorations();
}

void VPointEditTool::deactivate()
{
    cancelPolygon();
    m_selected.clear();
    m_edits.clear();
    m_drag = Drag::None;
    m_hovering = false;
    m_hoveredHit = Hit();
    disconnect(m_selectionConnection);
    disconnect(m_layerSelectionConnection);
    disconnect(m_increaseSizeConnection);
    disconnect(m_decreaseSizeConnection);
    repaintDecorations();
    KoToolBase::deactivate();
}

QList<QPair<VCurveShape*, QVector<bool>>> VPointEditTool::selectedPointMasks() const
{
    QList<QPair<VCurveShape*, QVector<bool>>> result;
    const QList<VCurveShape*> shapes = targets();
    for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
        if (!shapes.contains(it.key()) || it->size() != it.key()->curves().pointCount()) continue;
        if (!it->contains(true)) continue;
        result << qMakePair(it.key(), it.value());
    }
    return result;
}

bool VPointEditTool::hasSelection()
{
    return !selectedPointMasks().isEmpty();
}

void VPointEditTool::copy() const
{
    QList<KoShape*> copies;
    const auto masks = selectedPointMasks();
    if (masks.isEmpty()) {
        // no points: the selected strokes, as Select Shapes copies them
        const QList<KoShape*> shapes = canvas()->selectedShapesProxy()->selection()->selectedShapes();
        if (shapes.isEmpty()) return;
        KoDrag drag;
        drag.setSvg(shapes);
        drag.addToClipboard();
        return;
    }
    for (const auto &mask : masks) {
        VCurveData picked;
        VPointEdit::splitSelected(mask.first->curves(), mask.second, &picked, nullptr);
        if (picked.isEmpty()) continue;
        VCurveShape *copy = dynamic_cast<VCurveShape*>(mask.first->cloneShape());
        if (!copy) continue;
        // where it is in the document: the clone has no parent
        copy->setTransformation(mask.first->absoluteTransformation());
        copy->setCurves(picked);
        copies << copy;
    }
    if (copies.isEmpty()) {
        const_cast<VPointEditTool*>(this)->showMessage(
            i18n("Select two neighbouring points or more to copy the line between them."));
        return;
    }
    KoDrag drag;
    drag.setSvg(copies);
    drag.addToClipboard();
    qDeleteAll(copies);
}

void VPointEditTool::deleteSelection()
{
    const auto masks = selectedPointMasks();
    if (masks.isEmpty()) {
        QList<KoShape*> shapes;
        Q_FOREACH (KoShape *shape, canvas()->selectedShapesProxy()->selection()->selectedEditableShapes()) {
            shapes << shape;
        }
        if (!shapes.isEmpty()) canvas()->addCommand(canvas()->shapeController()->removeShapes(shapes));
        return;
    }
    m_actions.begin();
    QList<VCurveShape*> removed;
    for (const auto &mask : masks) {
        VCurveData rest;
        VCurveData picked;
        VPointEdit::splitSelected(mask.first->curves(), mask.second, &picked, &rest);
        if (picked.isEmpty()) continue;
        if (rest.isEmpty()) {
            removed << mask.first;
            continue;
        }
        m_actions.touch(mask.first);
        mask.first->setCurvesAndUpdate(rest);
    }
    repaintDecorations();
    m_selected.clear();
    m_actions.commit(kundo2_i18n("Delete Points"), removed);
    repaintDecorations();
}

void VPointEditTool::deselect()
{
    repaintDecorations();
    m_selected.clear();
    repaintDecorations();
}

void VPointEditTool::mousePressEvent(KoPointerEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (!layer()) {
        showMessage(i18n("The point edit tool edits freehand vector layers. Please select a freehand vector layer."));
        event->ignore();
        return;
    }
    if (m_drag == Drag::Polygon) {
        // the next corner; a click on the first one closes the polygon
        const qreal closeDistance = canvas()->viewConverter()->viewToDocumentX(GrabDistance);
        if (m_areaPoints.size() >= 3 && QLineF(event->point, m_areaPoints.first()).length() <= closeDistance) {
            finishPolygon();
        } else {
            m_areaPoints << event->point;
            m_dragNow = event->point;
            repaintDecorations();
        }
        event->accept();
        return;
    }
    dropStaleSelections();

    m_dragStart = m_dragNow = event->point;
    m_addToSelection = event->modifiers() & Qt::ShiftModifier;

    // the handles of a single selected point come before the points
    VCurveShape *single = nullptr;
    int singleIndex = -1;
    if (!m_addToSelection && singleSelectedPoint(&single, &singleIndex) &&
        showsHandles(single->curves(), singleIndex, single->absoluteTransformation() * canvas()->viewConverter()->documentToView())) {
        const QTransform toDocument = single->absoluteTransformation();
        const qreal grab = canvas()->viewConverter()->viewToDocumentX(GrabDistance);
        for (VPointEdit::HandleSide side : {VPointEdit::HandleSide::Out, VPointEdit::HandleSide::In}) {
            if (!VPointEdit::hasHandle(single->curves(), singleIndex, side)) continue;
            const QPointF handle = toDocument.map(VPointEdit::handle(single->curves(), singleIndex, side));
            if (QLineF(handle, event->point).length() > grab) continue;
            m_drag = Drag::Handle;
            m_handleSide = side;
            m_handleIndex = singleIndex;
            m_grabbed = handle;
            m_hovering = false;
            m_edits = {Edit{single, single->curves(), VPointEdit::Weights(), toDocument.inverted()}};
            repaintDecorations();
            event->accept();
            return;
        }
    }

    const Hit hit = hitAt(event->point);

    if (!hit.shape) {
        // an object that isn't selected: select it to show its points
        KoSelection *shapeSelection = canvas()->selectedShapesProxy()->selection();
        KoShape *under = shapeUnder(event->point);
        if (under && !shapeSelection->isSelected(under)) {
            if (!m_addToSelection) {
                shapeSelection->deselectAll();
                m_selected.clear();
            }
            shapeSelection->select(under);
            m_drag = Drag::None;
            repaintDecorations();
            event->accept();
            return;
        }
        m_drag = m_selectionShape == SelectionShape::Polygon ? Drag::Polygon : Drag::Area;
        m_areaPoints = QPolygonF({event->point});
        if (!m_addToSelection) m_selected.clear();
        repaintDecorations();
        event->accept();
        return;
    }

    if (m_addToSelection) {
        QVector<bool> &s = selection(hit.shape);
        s[hit.index] = !s[hit.index];
        m_drag = Drag::None;
    } else {
        if (!isSelected(hit.shape, hit.index)) {
            m_selected.clear();
            selection(hit.shape)[hit.index] = true;
        }
        m_drag = event->modifiers() & Qt::ControlModifier ? Drag::Radius : Drag::Move;
        m_grabbed = hit.shape->absoluteTransformation().map(hit.shape->curves().positions[hit.index]);
        m_hovering = false;
        m_edits = beginEdit();
    }
    repaintDecorations();
    event->accept();
}

QVector<VPointEditTool::Edit> VPointEditTool::beginEdit() const
{
    QVector<Edit> edits;
    for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
        VCurveShape *shape = it.key();
        const QTransform toDocument = shape->absoluteTransformation();
        const VPointEdit::Weights weights =
            VPointEdit::weights(shape->curves(), it.value(), toDocument, m_falloffRadius, m_falloffProfile);
        if (!weights.movesAny()) continue;
        edits << Edit{shape, shape->curves(), weights, toDocument.inverted()};
    }
    return edits;
}

void VPointEditTool::applyEdit(const QPointF &documentDelta)
{
    for (const Edit &edit : m_edits) {
        VCurveData data = edit.before;
        if (m_drag == Drag::Handle) {
            VPointEdit::moveHandle(data, m_handleIndex, m_handleSide, edit.documentToLocal.map(m_grabbed + documentDelta));
        } else if (m_drag == Drag::Move) {
            const QPointF localDelta = edit.documentToLocal.map(documentDelta) - edit.documentToLocal.map(QPointF());
            VPointEdit::move(data, edit.weights, localDelta);
        } else {
            // up makes it wider
            VPointEdit::scaleRadii(data, edit.weights, qExp(-documentDelta.y() / 50.0));
        }
        edit.shape->setCurvesAndUpdate(data);
    }
}

void VPointEditTool::mouseMoveEvent(KoPointerEvent *event)
{
    if (m_drag == Drag::Polygon) {
        // the edge to the cursor follows it between clicks
        repaintDecorations();
        m_dragNow = event->point;
        repaintDecorations();
        event->accept();
        return;
    }
    if (m_drag == Drag::None) {
        // the falloff circle shows at the point under the cursor
        const Hit hit = layer() ? hitAt(event->point) : Hit();
        m_hoveredHit = hit;
        const bool hovering = hit.shape;
        const QPointF hovered = hovering ? hit.shape->absoluteTransformation().map(hit.shape->curves().positions[hit.index])
                                         : QPointF();
        if (hovering != m_hovering || hovered != m_hovered) {
            m_hovering = hovering;
            m_hovered = hovered;
            if (m_falloffRadius > 0) repaintDecorations();
        }
        event->ignore();
        return;
    }
    if (m_drag == Drag::Area) {
        // the area can shrink: repaint where it was too
        repaintDecorations();
        m_dragNow = event->point;
        if (m_selectionShape == SelectionShape::Lasso) m_areaPoints << event->point;
        repaintDecorations();
    } else {
        m_dragNow = event->point;
        applyEdit(m_dragNow - m_dragStart);
        repaintDecorations();
    }
    event->accept();
}

void VPointEditTool::finishEdit(const KUndo2MagicString &text)
{
    QList<VCurveShape*> shapes;
    QList<VCurveData> before;
    QList<VCurveData> after;
    for (const Edit &edit : m_edits) {
        if (edit.shape->curves() == edit.before) continue;
        shapes << edit.shape;
        before << edit.before;
        after << edit.shape->curves();
    }
    m_edits.clear();
    if (!shapes.isEmpty()) {
        canvas()->addCommand(new VCurveEditCommand(shapes, before, after, text));
    }
}

void VPointEditTool::mouseReleaseEvent(KoPointerEvent *event)
{
    if (m_drag == Drag::None) {
        event->ignore();
        return;
    }
    if (m_drag == Drag::Polygon) {
        // corners are added on press; the polygon stays open between clicks
        event->accept();
        return;
    }
    repaintDecorations();
    m_dragNow = event->point;

    if (m_drag == Drag::Area) {
        if (m_selectionShape == SelectionShape::Lasso) m_areaPoints << event->point;
        selectPointsIn(selectionArea(false));
    } else {
        applyEdit(m_dragNow - m_dragStart);
        finishEdit(m_drag == Drag::Move ? kundo2_i18n("Move Points")
                   : m_drag == Drag::Handle ? kundo2_i18n("Move Handle")
                                            : kundo2_i18n("Change Line Width"));
    }
    m_drag = Drag::None;
    repaintDecorations();
    event->accept();
}

void VPointEditTool::mouseDoubleClickEvent(KoPointerEvent *event)
{
    if (m_drag != Drag::Polygon) {
        event->ignore();
        return;
    }
    // the press before the double click added its corner already
    finishPolygon();
    event->accept();
}

void VPointEditTool::keyPressEvent(QKeyEvent *event)
{
    if (m_drag == Drag::Polygon) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishPolygon();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelPolygon();
            event->accept();
            return;
        }
    }
    KoToolBase::keyPressEvent(event);
}

QVector<VPointEditTool::Edit> VPointEditTool::actionTargets()
{
    QVector<Edit> result;
    KisShapeLayer *l = layer();
    if (!l) {
        showMessage(i18n("The point edit tool edits freehand vector layers. Please select a freehand vector layer."));
        return result;
    }
    const QList<VCurveShape*> shapes = m_actions.targets(l, !m_wholeLayer);
    if (shapes.isEmpty()) {
        showMessage(m_wholeLayer ? i18n("The layer has no strokes that can be edited.")
                    : canvas()->selectedShapesProxy()->selection()->selectedShapes().isEmpty()
                        ? i18n("No strokes selected.")
                        : i18n("The selection has no strokes that can be edited."));
        return result;
    }

    dropStaleSelections();
    bool anyPoints = false;
    if (!m_wholeLayer) {
        for (auto it = m_selected.constBegin(); it != m_selected.constEnd(); ++it) {
            anyPoints |= std::find(it->begin(), it->end(), true) != it->end();
        }
    }
    for (VCurveShape *shape : shapes) {
        const QTransform toDocument = shape->absoluteTransformation();
        const QVector<bool> points = anyPoints ? m_selected.value(shape)
                                               : QVector<bool>(shape->curves().pointCount(), true);
        if (points.isEmpty()) continue;
        const VPointEdit::Weights weights = VPointEdit::weights(shape->curves(), points, toDocument,
                                                                anyPoints ? m_falloffRadius : 0.0, m_falloffProfile);
        if (weights.movesAny()) result << Edit{shape, shape->curves(), weights, toDocument.inverted()};
    }
    return result;
}

void VPointEditTool::scaleSelectedRadii(qreal factor)
{
    for (const Edit &edit : actionTargets()) {
        VCurveData data = edit.before;
        VPointEdit::scaleRadii(data, edit.weights, factor);
        m_actions.touch(edit.shape);
        edit.shape->setCurvesAndUpdate(data);
    }
    m_actions.commit(kundo2_i18n("Change Line Width"));
    repaintDecorations();
}

void VPointEditTool::setSelectedRadii()
{
    for (const Edit &edit : actionTargets()) {
        VCurveData data = edit.before;
        const qreal target = m_actionSettings.setRadius / VEditActions::radiusScale(edit.shape);
        for (int i = 0; i < data.pointCount(); i++) {
            if (edit.weights.points[i] > 0) VEditActions::moveRadiusToward(data, i, target, edit.weights.points[i]);
        }
        m_actions.touch(edit.shape);
        edit.shape->setCurvesAndUpdate(data);
    }
    m_actions.commit(kundo2_i18n("Change Line Width"));
    repaintDecorations();
}

void VPointEditTool::applyToSelected(VEditActions::Whole what)
{
    QList<VCurveShape*> shapes;
    QHash<VCurveShape*, QVector<qreal>> weights;
    for (const Edit &edit : actionTargets()) {
        shapes << edit.shape;
        weights.insert(edit.shape, edit.weights.points);
    }
    if (shapes.isEmpty()) return;
    m_actions.applyWhole(what, shapes, [&weights] (VCurveShape *shape) { return weights.value(shape); });
    dropStaleSelections();
    repaintDecorations();
}

void VPointEditTool::changeSelectedOpacity(VEditSettings::OpacityTool tool)
{
    for (const Edit &edit : actionTargets()) {
        VCurveData data = edit.before;
        VEditActions::changeOpacity(data, edit.shape, edit.weights.points, 1.0, tool, m_actionSettings);
        if (data == edit.before) continue;
        m_actions.touch(edit.shape);
        edit.shape->setCurvesAndUpdate(data);
    }
    m_actions.commit(kundo2_i18n("Change Stroke Opacity/Density"));
    repaintDecorations();
}

void VPointEditTool::applyBrushToSelected()
{
    QList<VCurveShape*> shapes;
    for (const Edit &edit : actionTargets()) shapes << edit.shape;
    VEditActions::applyCurrentBrush(canvas(), shapes, m_actionSettings.brushKeepSize);
    repaintDecorations();
}

VPointEditTool::Action VPointEditTool::currentAction() const
{
    return m_action;
}

void VPointEditTool::setCurrentAction(Action action)
{
    m_action = action;
    saveActionSettings();
}

bool VPointEditTool::actsOnWholeLayer() const
{
    return m_wholeLayer;
}

void VPointEditTool::setActsOnWholeLayer(bool wholeLayer)
{
    m_wholeLayer = wholeLayer;
    saveActionSettings();
}

VEditSettings &VPointEditTool::actionSettings()
{
    return m_actionSettings;
}

void VPointEditTool::loadActionSettings()
{
    // Thicker and Thinner went by 25% before they had an Amount
    m_actionSettings.thicknessAmount = 0.25;
    m_actionSettings.load(QStringLiteral("VPointEditTool/actions"));
    KConfigGroup group = KSharedConfig::openConfig()->group("VPointEditTool/actions");
    m_action = Action(qBound(0, group.readEntry("action", int(m_action)), int(Action::Brush)));
    m_wholeLayer = group.readEntry("wholeLayer", m_wholeLayer);
}

void VPointEditTool::saveActionSettings() const
{
    m_actionSettings.save(QStringLiteral("VPointEditTool/actions"));
    KConfigGroup group = KSharedConfig::openConfig()->group("VPointEditTool/actions");
    group.writeEntry("action", int(m_action));
    group.writeEntry("wholeLayer", m_wholeLayer);
}

const qreal VPointEditTool::MinHandleSpacing = 14.0;

bool VPointEditTool::showsHandles(const VCurveData &curves, int index, const QTransform &toView, qreal minSpacing)
{
    return VPointEdit::pointSpacing(curves, index, toView) >= minSpacing;
}

void VPointEditTool::paintPoints(QPainter &painter, const KoViewConverter &converter, VCurveShape *shape,
                                 const std::function<bool(int)> &selected, int singleIndex,
                                 qreal handleRadius, qreal decorationThickness,
                                 qreal minHandleSpacing)
{
    KisHandlePainterHelper helper =
        KoShape::createHandlePainterHelperView(&painter, shape, converter, handleRadius, decorationThickness);
    const VCurveData &d = shape->curves();
    const QTransform toView = shape->absoluteTransformation() * converter.documentToView();

    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int end = d.curveEnd(c);
        QPointF lastDrawn;
        bool drewAny = false;
        for (int i = begin; i < end; i++) {
            const bool isSelected = selected(i);
            const QPointF view = toView.map(d.positions[i]);
            // dense brush strokes: every point would cover the line at low zoom
            const bool endpoint = i == begin || i == end - 1;
            if (!isSelected && !endpoint && drewAny && QLineF(view, lastDrawn).length() < MinPointSpacing) {
                continue;
            }
            helper.setHandleStyle(isSelected ? KisHandleStyle::selectedPrimaryHandles()
                                             : KisHandleStyle::primarySelection());
            helper.drawHandleRect(d.positions[i]);
            lastDrawn = view;
            drewAny = true;
        }
    }

    // the handles of a single selected point, as Edit Shapes shows a path point's
    if (singleIndex < 0 || !showsHandles(d, singleIndex, toView, minHandleSpacing)) return;
    helper.setHandleStyle(KisHandleStyle::secondarySelection());
    for (VPointEdit::HandleSide side : {VPointEdit::HandleSide::In, VPointEdit::HandleSide::Out}) {
        if (!VPointEdit::hasHandle(d, singleIndex, side)) continue;
        const QPointF handle = VPointEdit::handle(d, singleIndex, side);
        helper.drawConnectionLine(d.positions[singleIndex], handle);
        helper.drawHandleCircle(handle);
    }
}

void VPointEditTool::paint(QPainter &painter, const KoViewConverter &converter)
{
    VCurveShape *single = nullptr;
    int singleIndex = -1;
    if (!singleSelectedPoint(&single, &singleIndex)) single = nullptr;

    Q_FOREACH (VCurveShape *shape, targets()) {
        paintPoints(painter, converter, shape, [this, shape] (int i) { return isSelected(shape, i); },
                    shape == single ? singleIndex : -1, handleRadius(), decorationThickness());
    }

    const QPointF center = falloffCenter();
    if (!center.isNull()) {
        // as Blender's proportional editing circle
        painter.save();
        QPen pen(QColor(255, 255, 255, 200));
        pen.setCosmetic(true);
        pen.setWidthF(decorationThickness());
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QSizeF radius = converter.documentToView(QSizeF(m_falloffRadius, m_falloffRadius));
        painter.drawEllipse(converter.documentToView(center), radius.width(), radius.height());
        pen.setColor(QColor(0, 0, 0, 160));
        pen.setDashPattern({4, 4});
        painter.setPen(pen);
        painter.drawEllipse(converter.documentToView(center), radius.width(), radius.height());
        painter.restore();
    }

    if (m_drag == Drag::Area || m_drag == Drag::Polygon) {
        painter.save();
        QPen pen(Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        if (m_drag == Drag::Polygon) {
            // open until finished, as Krita's polygonal selection
            painter.drawPolyline(converter.documentToView().map(QPolygonF(m_areaPoints) << m_dragNow));
        } else {
            painter.drawPath(converter.documentToView().map(selectionArea(false)));
        }
        painter.restore();
    }
}

qreal VPointEditTool::falloffRadius() const
{
    return m_falloffRadius;
}

void VPointEditTool::setFalloffRadius(qreal radius)
{
    // the old circle and the new one
    repaintDecorations();
    m_falloffRadius = qMax(0.0, radius);
    if (m_falloffSpinBox) {
        QSignalBlocker blocker(m_falloffSpinBox);
        m_falloffSpinBox->setValue(m_falloffRadius);
    }
    repaintDecorations();
}

void VPointEditTool::changeFalloffRadius(bool increase)
{
    setFalloffRadius(increase ? s_falloffSteps.increaseBrushSize(m_falloffRadius)
                              : s_falloffSteps.decreaseBrushSize(m_falloffRadius));
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->showFloatingMessage(i18n("Falloff: %1 pt", m_falloffRadius), QIcon(), 1000,
                                                      KisFloatingMessage::High,
                                                      Qt::AlignLeft | Qt::TextWordWrap | Qt::AlignVCenter);
    }
}

VPointEdit::Profile VPointEditTool::falloffProfile() const
{
    return m_falloffProfile;
}

void VPointEditTool::setFalloffProfile(VPointEdit::Profile profile)
{
    m_falloffProfile = profile;
}

VPointEditTool::SelectionShape VPointEditTool::selectionShape() const
{
    return m_selectionShape;
}

void VPointEditTool::setSelectionShape(SelectionShape shape)
{
    cancelPolygon();
    m_selectionShape = shape;
}

QWidget *VPointEditTool::createOptionWidget()
{
    QWidget *widget = new QWidget();
    QFormLayout *layout = new QFormLayout(widget);

    QDoubleSpinBox *falloff = VEditActions::doubleSlider(widget, 0.0, 2000.0, m_falloffRadius, 2, i18n(" pt"), QString());
    falloff->setToolTip(i18n("Points this close to the selected ones follow them, less the farther they are. 0 moves the selected points only. The brush size shortcuts change it too."));
    connect(falloff, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] (double value) {
        setFalloffRadius(value);
    });
    layout->addRow(i18n("Falloff:"), falloff);
    m_falloffSpinBox = static_cast<KisDoubleSliderSpinBox*>(falloff);

    QComboBox *profile = new QComboBox(widget);
    const QList<QPair<QString, VPointEdit::Profile>> profiles = {
        {i18nc("falloff shape", "Smooth"), VPointEdit::Profile::Smooth},
        {i18nc("falloff shape", "Sphere"), VPointEdit::Profile::Sphere},
        {i18nc("falloff shape", "Root"), VPointEdit::Profile::Root},
        {i18nc("falloff shape", "Inverse Square"), VPointEdit::Profile::InverseSquare},
        {i18nc("falloff shape", "Sharp"), VPointEdit::Profile::Sharp},
        {i18nc("falloff shape", "Linear"), VPointEdit::Profile::Linear},
        {i18nc("falloff shape", "Constant"), VPointEdit::Profile::Constant},
    };
    for (const auto &entry : profiles) {
        profile->addItem(entry.first, int(entry.second));
    }
    profile->setCurrentIndex(profile->findData(int(m_falloffProfile)));
    profile->setToolTip(i18n("How the points within the falloff follow: the shape of the bump a drag makes."));
    connect(profile, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, profile] (int) {
        setFalloffProfile(VPointEdit::Profile(profile->currentData().toInt()));
    });
    layout->addRow(i18n("Falloff shape:"), profile);

    QComboBox *selectionShape = new QComboBox(widget);
    const QList<std::tuple<QString, QString, SelectionShape>> shapes = {
        {i18nc("point selection shape", "Rectangle"), QStringLiteral("tool_rect_selection"), SelectionShape::Rectangle},
        {i18nc("point selection shape", "Ellipse"), QStringLiteral("tool_elliptical_selection"), SelectionShape::Ellipse},
        {i18nc("point selection shape", "Lasso"), QStringLiteral("tool_outline_selection"), SelectionShape::Lasso},
        {i18nc("point selection shape", "Polygon"), QStringLiteral("tool_polygonal_selection"), SelectionShape::Polygon},
    };
    for (const auto &entry : shapes) {
        selectionShape->addItem(KisIconUtils::loadIcon(std::get<1>(entry)), std::get<0>(entry), int(std::get<2>(entry)));
    }
    selectionShape->setCurrentIndex(selectionShape->findData(int(m_selectionShape)));
    selectionShape->setToolTip(i18n("The shape that selects points when you drag on empty canvas. Polygon: click the corners, then double-click, press Enter or click the first corner to finish; Escape cancels."));
    connect(selectionShape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, selectionShape] (int) {
        setSelectionShape(SelectionShape(selectionShape->currentData().toInt()));
    });
    layout->addRow(i18n("Selection shape:"), selectionShape);

    QLabel *hint = new QLabel(i18n("Click a stroke to show its points. Shift+click adds points. Ctrl+drag a point up or down to change the line width there."), widget);
    hint->setWordWrap(true);
    layout->addRow(hint);
    layout->addRow(createActionsWidget(widget));

    widget->setObjectName(i18n("Point Edit"));
    return widget;
}

QWidget *VPointEditTool::createActionsWidget(QWidget *parent)
{
    QGroupBox *box = new QGroupBox(i18n("Actions"), parent);
    box->setObjectName("actionsBox");
    QVBoxLayout *outer = new QVBoxLayout(box);
    QFormLayout *shared = new QFormLayout();
    outer->addLayout(shared);
    const std::function<void()> changed = [this] () { saveActionSettings(); };

    QComboBox *scope = new QComboBox(box);
    scope->setObjectName("cmbActionScope");
    scope->addItem(i18nc("what an action works on", "Selection"), false);
    scope->addItem(i18nc("what an action works on", "Whole layer"), true);
    scope->setCurrentIndex(scope->findData(m_wholeLayer));
    scope->setToolTip(i18n("Selection: the selected points, with the falloff, or with none the selected strokes. Whole layer: every stroke on the layer."));
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, scope] (int) {
        setActsOnWholeLayer(scope->currentData().toBool());
    });
    shared->addRow(i18n("Apply to:"), scope);

    QComboBox *action = new QComboBox(box);
    action->setObjectName("cmbAction");
    action->addItem(i18nc("edit shapes action", "Thickness"), int(Action::LineWidth)); // changing display name for consistency with Edit Shapes tool naming
    action->addItem(i18nc("edit shapes action", "Smooth"), int(Action::Smooth));
    action->addItem(i18nc("edit shapes action", "Simplify"), int(Action::Simplify));
    action->addItem(i18nc("edit shapes action", "Subdivide"), int(Action::Subdivide));
    action->addItem(i18nc("edit shapes action", "Opacity/Density"), int(Action::Opacity));
    action->addItem(i18nc("edit shapes action", "Brush"), int(Action::Brush));
    action->setCurrentIndex(action->findData(int(m_action)));
    shared->addRow(i18n("Action:"), action);

    QDoubleSpinBox *strength = VEditActions::doubleSlider(box, 0.01, 1, m_actionSettings.strength, 2, QString(), QString());
    strength->setToolTip(i18n("Smooth: how far the points go. Simplify, Progressive: the share of the points that goes."));
    connect(strength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] (double v) {
        m_actionSettings.strength = v;
        saveActionSettings();
    });
    QLabel *strengthLabel = new QLabel(i18n("Strength:"), box);
    shared->addRow(strengthLabel, strength);

    auto button = [box] (const QString &text, const QString &name) {
        QPushButton *result = new QPushButton(text, box);
        result->setObjectName(name);
        return result;
    };

    QStackedWidget *pages = new QStackedWidget(box);
    pages->setObjectName("actionPages");
    outer->addWidget(pages);

    // Line width (the pages are in the order of Action)
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QWidget *row = new QWidget(page);
        QHBoxLayout *buttons = new QHBoxLayout(row);
        buttons->setContentsMargins(0, 0, 0, 0);
        QPushButton *thinner = button(i18n("Thinner"), "btnThinner");
        QPushButton *thicker = button(i18n("Thicker"), "btnThicker");
        // Thicker and Thinner undo each other: times and divided by 1 + Amount
        connect(thinner, &QPushButton::clicked, this, [this] () {
            scaleSelectedRadii(1.0 / (1.0 + m_actionSettings.thicknessAmount));
        });
        connect(thicker, &QPushButton::clicked, this, [this] () {
            scaleSelectedRadii(1.0 + m_actionSettings.thicknessAmount);
        });
        buttons->addWidget(thinner);
        buttons->addWidget(thicker);
        form->addRow(row);
        QSpinBox *amount = VEditActions::intSlider(page, 1, 100, qRound(m_actionSettings.thicknessAmount * 100), i18n("%"),
                                                   i18n("Thicker multiplies the width by 1 + this, Thinner divides "
                                                        "it by the same, so one undoes the other."));
        connect(amount, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] (int v) {
            m_actionSettings.thicknessAmount = v / 100.0;
            saveActionSettings();
        });
        form->addRow(i18n("Amount:"), amount);
        QDoubleSpinBox *radius = VEditActions::doubleSlider(page, 0.01, 1000, m_actionSettings.setRadius, 2, i18n(" pt"), QString());
        radius->setToolTip(i18n("The line's radius (half its width). On brush strokes it is the radius at full pressure, so their pressure tapers stay."));
        connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] (double v) {
            m_actionSettings.setRadius = v;
            saveActionSettings();
        });
        form->addRow(i18n("Radius:"), radius);
        QPushButton *set = button(i18n("Set radius"), "btnSetRadius");
        connect(set, &QPushButton::clicked, this, [this] () { setSelectedRadii(); });
        form->addRow(set);
        pages->addWidget(page);
    }
    // Smooth, Simplify and Subdivide: the Vector Edit brushes' options, with their own values
    const QList<QPair<QWidget*, VEditActions::Whole>> wholePages = {
        {VEditActions::smoothPage(pages, &m_actionSettings, changed, false), VEditActions::Whole::Smooth},
        {VEditActions::simplifyPage(pages, &m_actionSettings, changed), VEditActions::Whole::Simplify},
        {VEditActions::subdividePage(pages, &m_actionSettings, changed), VEditActions::Whole::Subdivide},
    };
    for (const auto &entry : wholePages) {
        QPushButton *apply = button(i18n("Apply"), "btnApply");
        const VEditActions::Whole what = entry.second;
        connect(apply, &QPushButton::clicked, this, [this, what] () { applyToSelected(what); });
        static_cast<QFormLayout*>(entry.first->layout())->addRow(apply);
        pages->addWidget(entry.first);
    }
    // Opacity/Density
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QCheckBox *opacity = new QCheckBox(i18n("Opacity"), page);
        opacity->setChecked(m_actionSettings.affectOpacity);
        opacity->setToolTip(i18n("The line's opacity: it fades the line as drawn, as a layer's opacity would."));
        connect(opacity, &QCheckBox::toggled, this, [this] (bool on) {
            m_actionSettings.affectOpacity = on;
            saveActionSettings();
        });
        form->addRow(opacity);
        QCheckBox *density = new QCheckBox(i18n("Density"), page);
        density->setChecked(m_actionSettings.affectDensity);
        density->setToolTip(i18n("Brush strokes: the density of the brush's dabs (their opacity and flow), on top of what pressure gave them."));
        connect(density, &QCheckBox::toggled, this, [this] (bool on) {
            m_actionSettings.affectDensity = on;
            saveActionSettings();
        });
        form->addRow(density);
        QWidget *row = new QWidget(page);
        QHBoxLayout *buttons = new QHBoxLayout(row);
        buttons->setContentsMargins(0, 0, 0, 0);
        QPushButton *decrease = button(i18n("Decrease"), "btnOpacityDecrease");
        QPushButton *increase = button(i18n("Increase"), "btnOpacityIncrease");
        connect(decrease, &QPushButton::clicked, this, [this] () { changeSelectedOpacity(VEditSettings::OpacityTool::Decrease); });
        connect(increase, &QPushButton::clicked, this, [this] () { changeSelectedOpacity(VEditSettings::OpacityTool::Increase); });
        buttons->addWidget(decrease);
        buttons->addWidget(increase);
        form->addRow(row);
        QSpinBox *amount = VEditActions::intSlider(page, 0, 100, qRound(m_actionSettings.changeAmount * 100), i18n("%"),
                                                   i18n("How far each click of Increase or Decrease moves the opacity "
                                                        "or density toward 100% or 0%."));
        connect(amount, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] (int v) {
            m_actionSettings.changeAmount = v / 100.0;
            saveActionSettings();
        });
        form->addRow(i18n("Amount:"), amount);
        QSpinBox *value = VEditActions::intSlider(page, 0, 100, qRound(m_actionSettings.setOpacity * 100), i18n("%"), QString());
        connect(value, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] (int v) {
            m_actionSettings.setOpacity = v / 100.0;
            saveActionSettings();
        });
        form->addRow(i18n("Set to:"), value);
        QPushButton *set = button(i18n("Set"), "btnOpacitySet");
        connect(set, &QPushButton::clicked, this, [this] () { changeSelectedOpacity(VEditSettings::OpacityTool::Set); });
        form->addRow(set);
        pages->addWidget(page);
    }
    // Brush
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QLabel *hint = new QLabel(i18n("Draws the brush strokes again with the brush selected in Brush Presets, "
                                       "using their recorded pressure, tilt, width and opacity edits. What wasn't "
                                       "recorded, such as speed, takes the brush's default."), page);
        hint->setWordWrap(true);
        form->addRow(hint);
        QCheckBox *keepSize = new QCheckBox(i18n("Keep stroke size"), page);
        keepSize->setObjectName("chkBrushKeepSize");
        keepSize->setChecked(m_actionSettings.brushKeepSize);
        keepSize->setToolTip(i18n("On: the strokes keep their width. Off: they take the brush's size, "
                                  "and their width changes with it."));
        connect(keepSize, &QCheckBox::toggled, this, [this] (bool on) {
            m_actionSettings.brushKeepSize = on;
            saveActionSettings();
        });
        form->addRow(keepSize);
        QPushButton *apply = button(i18n("Apply current brush"), "btnApplyBrush");
        connect(apply, &QPushButton::clicked, this, [this] () { applyBrushToSelected(); });
        form->addRow(apply);
        pages->addWidget(page);
    }

    auto showAction = [pages, strength, strengthLabel] (Action a) {
        pages->setCurrentIndex(int(a));
        // the box as tall as this page, not the tallest one
        for (int i = 0; i < pages->count(); i++) {
            const QSizePolicy::Policy policy = i == int(a) ? QSizePolicy::Preferred : QSizePolicy::Ignored;
            pages->widget(i)->setSizePolicy(policy, policy);
        }
        const bool usesStrength = a == Action::Smooth || a == Action::Simplify;
        strength->setVisible(usesStrength);
        strengthLabel->setVisible(usesStrength);
    };
    showAction(m_action);
    connect(action, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, action, showAction] (int) {
        setCurrentAction(Action(action->currentData().toInt()));
        showAction(m_action);
    });
    return box;
}

VPointEditToolFactory::VPointEditToolFactory()
    : KoToolFactoryBase("VPointEditTool")
{
    setToolTip(i18n("Freehand Point Edit Tool: moves the points of brush strokes and curves on freehand vector layers"));
    setSection(ToolBoxSection::Shape);
    setIconName(koIconNameCStr("pathshape"));
    setPriority(3);
    // the Edit Shapes button stands for this tool on freehand vector layers
    // (VStrokeToolSwitcher); see VStrokeToolFactory for the ids
    setActivationShapeId(QStringLiteral("vstroke/no-button,") + KRITA_TOOL_ACTIVATION_ID + QStringLiteral(",vstroke/no-button"));
}

QList<QAction *> VPointEditToolFactory::createActionsImpl()
{
    // The brush size actions belong to the paint tools; the tool manager
    // disables them for every tool that doesn't declare them. Declaring the
    // same names joins the existing actions (KoToolFactoryBase::createActions
    // keeps the first one), so the shortcut stays one remappable action.
    KisActionRegistry *actionRegistry = KisActionRegistry::instance();
    QList<QAction *> actions;

    KisAction *increaseBrushSize = new KisAction(i18n("Increase Brush Size"), this);
    increaseBrushSize->setObjectName("increase_brush_size");
    increaseBrushSize->setShortcut(Qt::Key_BracketRight);
    actionRegistry->propertizeAction("increase_brush_size", increaseBrushSize);
    actions << increaseBrushSize;

    KisAction *decreaseBrushSize = new KisAction(i18n("Decrease Brush Size"), this);
    decreaseBrushSize->setObjectName("decrease_brush_size");
    decreaseBrushSize->setShortcut(Qt::Key_BracketLeft);
    actionRegistry->propertizeAction("decrease_brush_size", decreaseBrushSize);
    actions << decreaseBrushSize;

    return actions;
}

KoToolBase *VPointEditToolFactory::createTool(KoCanvasBase *canvas)
{
    VStrokeToolSwitcher::start();
    return new VPointEditTool(canvas);
}
