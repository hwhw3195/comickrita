/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VEditTool.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KoCanvasBase.h>
#include <KoCanvasController.h>
#include <KoIcon.h>
#include <KoPointerEvent.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeController.h>
#include <KoShapeGroup.h>
#include <KoToolManager.h>
#include <KoViewConverter.h>
#include <commands/KoShapeBackgroundCommand.h>
#include <KisViewManager.h>
#include <kis_action.h>
#include <kis_action_registry.h>
#include <kis_canvas2.h>
#include <kis_cubic_curve.h>
#include <kis_curve_widget.h>
#include <kis_floating_message.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_shape_layer.h>
#include <kis_slider_spin_box.h>
#include <kis_tool.h>
#include <kis_tool_utils.h>
#include <klocalizedstring.h>

#include "VCurveOpacity.h"
#include "VCurveShape.h"
#include "VCurveSimplify.h"
#include "VCurveWarp.h"
#include "VFreehandLayer.h"
#include "VGeomCurveShape.h"
#include "VInkSize.h"
#include "VStrokeCurves.h"
#include "VStrokeShape.h"
#include "VStrokeToolSwitcher.h"

namespace {
/// the brush size shortcuts step the size as they step brush sizes
KisToolUtils::StandardBrushSizes s_sizeSteps(1, 2000);
/// below 1 pt, where brush sizes have no steps, the shortcuts go by tenths down to 0
const qreal SmallSizeStep = 0.1;

const QList<VEditTool::Mode> AllModes = {VEditTool::Mode::Smooth, VEditTool::Mode::Thickness, VEditTool::Mode::Opacity,
                                         VEditTool::Mode::Trim, VEditTool::Mode::Simplify, VEditTool::Mode::Subdivide};

qreal distanceToPolyline(const QPointF &p, const QPolygonF &line)
{
    if (line.isEmpty()) return std::numeric_limits<qreal>::max();
    qreal best = QLineF(p, line.first()).length();
    for (int i = 0; i + 1 < line.size(); i++) {
        const QPointF a = line[i];
        const QPointF ab = line[i + 1] - a;
        const qreal l2 = QPointF::dotProduct(ab, ab);
        const qreal t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0.0;
        best = qMin(best, QLineF(p, a + t * ab).length());
    }
    return best;
}
}

VEditTool::VEditTool(KoCanvasBase *canvas)
    : KoToolBase(canvas),
      m_actions(canvas, &m_settings)
{
    loadSettings();
}

VEditTool::~VEditTool()
{
}

QString VEditTool::modeName(Mode mode)
{
    switch (mode) {
    case Mode::Smooth: return QStringLiteral("smooth");
    case Mode::Thickness: return QStringLiteral("thickness");
    case Mode::Trim: return QStringLiteral("trim");
    case Mode::Simplify: return QStringLiteral("simplify");
    case Mode::Subdivide: return QStringLiteral("subdivide");
    case Mode::Opacity: return QStringLiteral("opacity");
    }
    return QString();
}

VEditTool::Settings &VEditTool::settings()
{
    return m_settings;
}

void VEditTool::loadSettings()
{
    m_settings.load(QStringLiteral("VEditTool"));
    KConfigGroup shared = KSharedConfig::openConfig()->group("VEditTool");
    m_mode = Mode(qBound(0, shared.readEntry("mode", int(m_mode)), int(Mode::Opacity)));
    // each mode's size, starting from the size all modes shared before
    const qreal sharedSize = m_settings.size;
    for (Mode mode : AllModes) {
        KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("VEditTool/") + modeName(mode));
        m_modeSizes[int(mode)] = qBound(0.0, group.readEntry("brushSize", sharedSize), 2000.0);
    }
    m_settings.size = m_modeSizes[int(m_mode)];
}

void VEditTool::saveSettings() const
{
    m_settings.save(QStringLiteral("VEditTool"));
    KConfigGroup shared = KSharedConfig::openConfig()->group("VEditTool");
    shared.writeEntry("mode", int(m_mode));
    for (Mode mode : AllModes) {
        KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("VEditTool/") + modeName(mode));
        group.writeEntry("brushSize", mode == m_mode ? m_settings.size : m_modeSizes[int(mode)]);
    }
}

KisShapeLayer *VEditTool::layer() const
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    if (!kisCanvas || !kisCanvas->viewManager()) return nullptr;
    KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(kisCanvas->viewManager()->activeNode().data());
    return VFreehandLayer::isFreehand(layer) ? layer : nullptr;
}

QList<VCurveShape*> VEditTool::targets() const
{
    return m_actions.targets(layer(), m_settings.selectedOnly);
}

void VEditTool::showMessage(const QString &message)
{
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->showFloatingMessage(message, KisIconUtils::loadIcon("object-locked"));
    }
}

VEditTool::Mode VEditTool::mode() const
{
    return m_mode;
}

void VEditTool::setMode(Mode mode)
{
    if (mode != m_mode) {
        // the brush size goes with the mode
        repaintDecorations();
        m_modeSizes[int(m_mode)] = m_settings.size;
        m_settings.size = m_modeSizes[int(mode)];
        if (m_sizeSpinBox) {
            QSignalBlocker sizeBlocker(m_sizeSpinBox);
            m_sizeSpinBox->setValue(m_settings.size);
        }
    }
    m_mode = mode;
    if (m_modeCombo) {
        QSignalBlocker blocker(m_modeCombo);
        m_modeCombo->setCurrentIndex(m_modeCombo->findData(int(mode)));
    }
    if (m_pages) m_pages->setCurrentIndex(int(mode));
    m_path.clear();
    repaintDecorations();
}

VEditTool::ThicknessTool VEditTool::thicknessToolFor(Qt::KeyboardModifiers modifiers) const
{
    const ThicknessTool tool = m_settings.thicknessTool;
    if (!(modifiers & Qt::ControlModifier) || tool == ThicknessTool::Set) return tool;
    return tool == ThicknessTool::Thicken ? ThicknessTool::Thin : ThicknessTool::Thicken;
}

VEditTool::OpacityTool VEditTool::opacityToolFor(Qt::KeyboardModifiers modifiers) const
{
    const OpacityTool tool = m_settings.opacityTool;
    if (!(modifiers & Qt::ControlModifier) || tool == OpacityTool::Set) return tool;
    return tool == OpacityTool::Increase ? OpacityTool::Decrease : OpacityTool::Increase;
}

void VEditTool::setNextActivationMode(Mode mode)
{
    m_nextModeSet = true;
    m_nextMode = mode;
}

void VEditTool::setBrushSize(qreal size)
{
    repaintDecorations();
    m_settings.size = qBound(0.0, size, 2000.0);
    m_modeSizes[int(m_mode)] = m_settings.size;
    if (m_sizeSpinBox) {
        QSignalBlocker blocker(m_sizeSpinBox);
        m_sizeSpinBox->setValue(m_settings.size);
    }
    repaintDecorations();
}

qreal VEditTool::brushSize(Mode mode) const
{
    return mode == m_mode ? m_settings.size : m_modeSizes[int(mode)];
}

qreal VEditTool::steppedBrushSize(qreal size, bool increase)
{
    const qreal tenths = size / SmallSizeStep;
    if (increase && size < 1.0) return (std::floor(tenths + 1e-6) + 1) * SmallSizeStep;
    if (!increase && size <= 1.0) return qMax(0.0, (std::ceil(tenths - 1e-6) - 1) * SmallSizeStep);
    return increase ? s_sizeSteps.increaseBrushSize(size) : s_sizeSteps.decreaseBrushSize(size);
}

void VEditTool::changeBrushSize(bool increase)
{
    setBrushSize(steppedBrushSize(m_settings.size, increase));
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->showFloatingMessage(i18n("Size: %1 pt", m_settings.size), QIcon(), 1000,
                                                      KisFloatingMessage::High,
                                                      Qt::AlignLeft | Qt::TextWordWrap | Qt::AlignVCenter);
    }
}

void VEditTool::activate(const QSet<KoShape*> &shapes)
{
    KoToolBase::activate(shapes);
    useCursor(Qt::CrossCursor);
    if (m_nextModeSet) {
        m_nextModeSet = false;
        setMode(m_nextMode);
    }
    if (QAction *increase = action("increase_brush_size")) {
        m_increaseSizeConnection = connect(increase, &QAction::triggered, this, [this] () { changeBrushSize(true); });
    }
    if (QAction *decrease = action("decrease_brush_size")) {
        m_decreaseSizeConnection = connect(decrease, &QAction::triggered, this, [this] () { changeBrushSize(false); });
    }
    repaintDecorations();
}

void VEditTool::deactivate()
{
    if (m_stroking) finishStroke();
    disconnect(m_increaseSizeConnection);
    disconnect(m_decreaseSizeConnection);
    m_cursorInside = false;
    repaintDecorations();
    saveSettings();
    KoToolBase::deactivate();
}

QRectF VEditTool::decorationsRect() const
{
    const qreal pad = canvas()->viewConverter()->viewToDocumentX(decorationThickness() + 2);
    QRectF rect;
    const qreal r = m_settings.size / 2;
    const bool brushShown = !(m_mode == Mode::Trim && m_settings.trimMode == VCurveTrim::Selector::Lasso);
    if (m_cursorInside && brushShown) {
        rect |= QRectF(m_cursor - QPointF(r, r), QSizeF(2 * r, 2 * r));
    }
    if (!m_path.isEmpty()) {
        rect |= m_path.boundingRect().adjusted(-r, -r, r, r);
    }
    return rect.isNull() ? rect : rect.adjusted(-pad, -pad, pad, pad);
}

void VEditTool::paint(QPainter &painter, const KoViewConverter &converter)
{
    painter.save();
    QPen pen(QColor(255, 255, 255, 200));
    pen.setCosmetic(true);
    pen.setWidthF(decorationThickness());
    painter.setBrush(Qt::NoBrush);

    const bool lasso = m_mode == Mode::Trim && m_settings.trimMode == VCurveTrim::Selector::Lasso;
    if (m_cursorInside && !lasso) {
        const QSizeF radius = converter.documentToView(QSizeF(m_settings.size / 2, m_settings.size / 2));
        const QPointF center = converter.documentToView(m_cursor);
        painter.setPen(pen);
        painter.drawEllipse(center, radius.width(), radius.height());
        QPen dashed = pen;
        dashed.setColor(QColor(0, 0, 0, 160));
        dashed.setDashPattern({4, 4});
        painter.setPen(dashed);
        painter.drawEllipse(center, radius.width(), radius.height());
    }
    if (m_mode == Mode::Trim && !lasso && m_settings.trimErase == VCurveTrim::Options::Area && !m_path.isEmpty() &&
        m_settings.trimMode == VCurveTrim::Selector::Drag) {
        // by point: the area that will be erased
        QPainterPathStroker stroker;
        stroker.setWidth(m_settings.size);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        QPainterPath path;
        path.addPolygon(m_path);
        if (m_path.size() == 1) path.lineTo(m_path.first());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 70));
        painter.drawPath(converter.documentToView().map(stroker.createStroke(path).simplified()));
        painter.setBrush(Qt::NoBrush);
    } else if (m_mode == Mode::Trim && m_path.size() > 1) {
        // what will be cut
        QPen dashed(Qt::DashLine);
        dashed.setCosmetic(true);
        painter.setPen(dashed);
        const QPolygonF view = converter.documentToView().map(m_path);
        if (lasso) {
            painter.drawPolygon(view);
        } else {
            painter.drawPolyline(view);
        }
    }
    painter.restore();
}

void VEditTool::mousePressEvent(KoPointerEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (!layer()) {
        showMessage(i18n("The vector edit tool edits freehand vector layers. Please select a freehand vector layer."));
        event->ignore();
        return;
    }
    beginStroke(event->point, event->modifiers());
    event->accept();
}

void VEditTool::mouseMoveEvent(KoPointerEvent *event)
{
    repaintDecorations();
    m_cursor = event->point;
    m_cursorInside = true;
    if (m_stroking) {
        continueStroke(event->point);
        event->accept();
    } else {
        event->ignore();
    }
    repaintDecorations();
}

void VEditTool::mouseReleaseEvent(KoPointerEvent *event)
{
    if (!m_stroking) {
        event->ignore();
        return;
    }
    continueStroke(event->point);
    finishStroke();
    event->accept();
}

void VEditTool::beginStroke(const QPointF &pos, Qt::KeyboardModifiers modifiers)
{
    m_stroking = true;
    // Ctrl+click reaches a KoToolBase as a plain press with Ctrl (the
    // input manager's Alternate Invocation turns it into an alternate
    // action for KisTools only), so the modifier is read here
    m_invert = modifiers & Qt::ControlModifier;
    m_actions.begin();
    m_path = QPolygonF({pos});
    m_strokeTargets = targets();
    m_lastDab = pos;
    m_subdivideBase.clear();
    if (m_mode != Mode::Trim) dab(pos);
}

void VEditTool::continueStroke(const QPointF &pos)
{
    if (m_mode == Mode::Trim) {
        repaintDecorations();
        if (m_path.isEmpty() || m_path.last() != pos) m_path << pos;
        repaintDecorations();
        return;
    }
    m_path << pos;
    // dabs a fifth of the brush apart along the drag
    const qreal spacing = qMax(m_settings.size * 0.2, canvas()->viewConverter()->viewToDocumentX(1.0));
    QLineF line(m_lastDab, pos);
    while (line.length() >= spacing) {
        const QPointF next = line.pointAt(spacing / line.length());
        dab(next);
        m_lastDab = next;
        line = QLineF(m_lastDab, pos);
    }
}

void VEditTool::dab(const QPointF &pos)
{
    const qreal radius = m_settings.size / 2;
    const QRectF area(pos - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
    for (VCurveShape *shape : m_strokeTargets) {
        const QTransform toDocument = shape->absoluteTransformation();
        // grown by the brush: a level line has flat bounds, which QRectF::intersects() never hits
        if (!toDocument.mapRect(shape->curves().controlBounds()).adjusted(-radius, -radius, radius, radius).intersects(area)) {
            continue;
        }

        VCurveData data = shape->curves();
        const QVector<qreal> weights = VCurveWarp::brushWeights(data, toDocument, pos, radius, m_settings.profile);
        if (!std::any_of(weights.begin(), weights.end(), [] (qreal w) { return w > 0; })) continue;

        switch (m_mode) {
        case Mode::Smooth: {
            VEditActions::smooth(data, toDocument, weights, m_settings.strength, m_settings.smoothReach * m_settings.size,
                                 m_settings);
            break;
        }
        case Mode::Thickness: {
            const ThicknessTool tool = thicknessToolFor(m_invert ? Qt::ControlModifier : Qt::NoModifier);
            if (tool != ThicknessTool::Set) {
                VEditActions::changeThickness(data, weights, m_settings.strength, tool == ThicknessTool::Thicken,
                                              m_settings.thicknessAmount);
                break;
            }
            const qreal target = m_settings.setRadius / VEditActions::radiusScale(shape);
            for (int i = 0; i < data.pointCount(); i++) {
                const qreal w = weights[i] * m_settings.strength;
                if (w > 0) VEditActions::moveRadiusToward(data, i, target, w);
            }
            break;
        }
        case Mode::Opacity:
            VEditActions::changeOpacity(data, shape, weights, m_settings.strength,
                                        opacityToolFor(m_invert ? Qt::ControlModifier : Qt::NoModifier), m_settings);
            break;
        case Mode::Simplify: {
            if (m_settings.simplifyMethod == SimplifyMethod::Progressive) {
                // dabs are a fifth of the brush apart: a pass takes about 40% at strength 0.5
                data = VCurveSimplify::progressive(data, toDocument, 0.2 * m_settings.strength, weights);
                break;
            }
            QVector<bool> mask(weights.size());
            for (int i = 0; i < weights.size(); i++) mask[i] = weights[i] > 0;
            const qreal tolerance = canvas()->viewConverter()->viewToDocumentX(m_settings.simplifyTolerance);
            data = VCurveSimplify::thin(data, toDocument, tolerance, m_settings.attributeTolerance, mask);
            break;
        }
        case Mode::Subdivide: {
            // the segments of the curve as the drag found it whose middle has been
            // under the brush, subdivided from that curve again: with Steps, dabs
            // landing on the new segments would otherwise halve them again and again
            auto base = m_subdivideBase.find(shape);
            if (base == m_subdivideBase.end()) {
                base = m_subdivideBase.insert(shape, qMakePair(data, QVector<bool>(data.pointCount(), false)));
            }
            const VCurveData &original = base->first;
            QVector<bool> &mask = base->second;
            bool grew = false;
            for (int i = 0; i < original.pointCount(); i++) {
                if (mask[i]) continue;
                const int next = original.nextIndex(i);
                if (next < 0) continue;
                const VCurvePoint a = original.point(i, QStringList());
                const VCurvePoint b = original.point(next, QStringList());
                if (QLineF(toDocument.map(VCurvePoint::segmentPoint(a, b, 0.5)), pos).length() < radius) {
                    mask[i] = true;
                    grew = true;
                }
            }
            if (!grew) continue;
            data = m_settings.subdivide(original, toDocument, mask);
            break;
        }
        case Mode::Trim:
            break;
        }
        if (data == shape->curves()) continue;
        m_actions.touch(shape);
        shape->setCurvesAndUpdate(data);
    }
}

void VEditTool::finishStroke()
{
    m_stroking = false;
    const QPolygonF path = m_path;
    m_path.clear();
    repaintDecorations();

    switch (m_mode) {
    case Mode::Smooth:
        m_actions.commit(kundo2_i18n("Smooth Strokes"));
        break;
    case Mode::Thickness:
        switch (thicknessToolFor(m_invert ? Qt::ControlModifier : Qt::NoModifier)) {
        case ThicknessTool::Thicken: m_actions.commit(kundo2_i18n("Thicken Strokes")); break;
        case ThicknessTool::Thin: m_actions.commit(kundo2_i18n("Thin Strokes")); break;
        case ThicknessTool::Set: m_actions.commit(kundo2_i18n("Set Stroke Radius")); break;
        }
        break;
    case Mode::Opacity:
        switch (opacityToolFor(m_invert ? Qt::ControlModifier : Qt::NoModifier)) {
        case OpacityTool::Increase: m_actions.commit(kundo2_i18n("Increase Stroke Opacity/Density")); break;
        case OpacityTool::Decrease: m_actions.commit(kundo2_i18n("Decrease Stroke Opacity/Density")); break;
        case OpacityTool::Set: m_actions.commit(kundo2_i18n("Set Stroke Opacity/Density")); break;
        }
        break;
    case Mode::Subdivide:
        m_actions.commit(kundo2_i18n("Subdivide Strokes"));
        break;
    case Mode::Simplify: {
        // the joins only where the brush went
        const qreal radius = m_settings.size / 2;
        QList<VCurveShape*> removed;
        m_actions.runJoins(m_strokeTargets, [path, radius] (const QPointF &p) { return distanceToPolyline(p, path) <= radius; },
                 &removed);
        m_actions.commit(kundo2_i18n("Simplify Strokes"), removed);
        break;
    }
    case Mode::Trim: {
        VCurveTrim::Selector selector;
        selector.mode = m_settings.trimMode;
        selector.path = path;
        selector.radius = m_settings.size / 2;
        selector.point = path.isEmpty() ? QPointF() : path.last();
        if (selector.mode == VCurveTrim::Selector::Click && m_settings.trimErase != VCurveTrim::Options::Area) {
            // by point, a click erases under the brush
            selector.radius = canvas()->viewConverter()->viewToDocumentX(8.0);
        }
        if (selector.mode == VCurveTrim::Selector::Lasso && path.size() < 3) break;
        trimAt(selector);
        break;
    }
    }
    m_strokeTargets.clear();
    m_subdivideBase.clear();
}

QVector<VCurveData> VEditTool::otherLayerCurves() const
{
    QVector<VCurveData> result;
    VInkSize::Cache ink;
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    KisShapeLayer *current = layer();
    if (!kisCanvas || !kisCanvas->image()) return result;
    KisLayerUtils::recursiveApplyNodes(kisCanvas->image()->root(), [&] (KisNodeSP node) {
        KisShapeLayer *shapeLayer = dynamic_cast<KisShapeLayer*>(node.data());
        if (!shapeLayer || shapeLayer == current || !VFreehandLayer::isFreehand(shapeLayer) || !shapeLayer->visible(true)) {
            return;
        }
        Q_FOREACH (KoShape *shape, shapeLayer->shapes()) {
            VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
            if (!curve || !curve->isVisible()) continue;
            VCurveData data = curve->curves();
            data.transform(curve->absoluteTransformation());
            for (int c = 0; c < data.curveCount(); c++) {
                const QVector<qreal> radii = ink.documentRadii(curve, c);
                std::copy(radii.begin(), radii.end(), data.radii.begin() + data.curveBegin(c));
            }
            result << data;
        }
    });
    return result;
}

void VEditTool::trimAt(const VCurveTrim::Selector &selector)
{
    const QList<VCurveShape*> shapes = m_strokeTargets;
    if (shapes.isEmpty()) return;
    VCurveTrim::Options options;
    options.keepCaps = m_settings.keepCaps;
    options.endsOnly = m_settings.trimEndsOnly;
    options.erase = m_settings.trimErase;
    options.tolerance = canvas()->viewConverter()->viewToDocumentX(0.25);
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    const qreal pixelsPerPoint = kisCanvas && kisCanvas->image() && kisCanvas->image()->xRes() > 0 ? kisCanvas->image()->xRes() : 1.0;
    options.leeway = m_settings.trimLeeway / pixelsPerPoint;
    const bool cutAtOthers = m_settings.trimAllLayers && options.erase == VCurveTrim::Options::ToCrossings;
    const QVector<VCurveData> cutters = cutAtOthers ? otherLayerCurves() : QVector<VCurveData>();

    QList<VCurveShape*> removed;
    QList<VCurveShape*> changed;
    m_actions.acrossShapes(shapes, [&] (const QVector<VDocStroke> &strokes) {
        // Trim goes by the ink the brush paints, wider or narrower than the stored radius; the stored radius
        // rides along as an attribute, cut and interpolated with the rest, and comes back afterwards
        static const QString StoredRadius = QStringLiteral("trim:stored-radius");
        VInkSize::Cache ink;
        QVector<VDocStroke> inked = strokes;
        for (VDocStroke &s : inked) {
            if (s.source < 0 || s.owner < 0 || s.owner >= shapes.size()) continue;
            const QVector<qreal> radii = ink.documentRadii(shapes[s.owner], s.source);
            if (radii.size() != s.curve.pointCount()) continue;
            s.curve.attributes.insert(StoredRadius, s.curve.radii);
            s.curve.radii = radii;
        }
        QVector<VDocStroke> result = VCurveTrim::trim(inked, cutters, selector, options);
        for (VDocStroke &s : result) {
            auto it = s.curve.attributes.find(StoredRadius);
            if (it == s.curve.attributes.end()) continue;
            s.curve.radii = *it;
            s.curve.attributes.erase(it);
        }
        return result;
    }, &removed, &changed);

    QList<VCurveShape*> dropFill;
    if (!m_settings.keepFill) {
        for (VCurveShape *shape : changed) {
            if (shape->background()) dropFill << shape;
        }
    }
    m_actions.commit(kundo2_i18n("Trim Strokes"), removed, dropFill);
}

QWidget *VEditTool::createOptionWidget()
{
    QWidget *widget = new QWidget();
    QVBoxLayout *outer = new QVBoxLayout(widget);
    outer->setContentsMargins(0, 0, 0, 0);
    QFormLayout *shared = new QFormLayout();
    outer->addLayout(shared);

    auto spin = [widget] (qreal min, qreal max, qreal value, int decimals, const QString &suffix, const QString &tip) {
        return VEditActions::doubleSlider(widget, min, max, value, decimals, suffix, tip);
    };
    auto onValue = [this] (QDoubleSpinBox *box, qreal *target) {
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, target] (double v) {
            *target = v;
            saveSettings();
        });
    };
    auto check = [this, widget] (const QString &text, bool *target, const QString &tip) {
        QCheckBox *box = new QCheckBox(text, widget);
        box->setChecked(*target);
        box->setToolTip(tip);
        connect(box, &QCheckBox::toggled, this, [this, target] (bool on) {
            *target = on;
            saveSettings();
        });
        return box;
    };

    QComboBox *mode = new QComboBox(widget);
    mode->addItem(i18nc("vector edit mode", "Smooth"), int(Mode::Smooth));
    mode->addItem(i18nc("vector edit mode", "Thickness"), int(Mode::Thickness));
    mode->addItem(i18nc("vector edit mode", "Opacity/Density"), int(Mode::Opacity));
    mode->addItem(i18nc("vector edit mode", "Trim"), int(Mode::Trim));
    mode->addItem(i18nc("vector edit mode", "Simplify"), int(Mode::Simplify));
    mode->addItem(i18nc("vector edit mode", "Subdivide"), int(Mode::Subdivide));
    mode->setCurrentIndex(mode->findData(int(m_mode)));
    shared->addRow(i18n("Mode:"), mode);
    m_modeCombo = mode;

    QDoubleSpinBox *size = spin(0, 2000, m_settings.size, 1, i18n(" pt"),
                                i18n("The brush diameter. Each mode keeps its own. The brush size shortcuts change it too. "
                                     "At 0, Trim takes only the lines the pointer itself goes over (by point it erases nothing), "
                                     "and the other modes change nothing."));
    size->setObjectName("dblSize");
    connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] (double v) {
        setBrushSize(v);
        saveSettings();
    });
    shared->addRow(i18n("Size:"), size);
    m_sizeSpinBox = static_cast<KisDoubleSliderSpinBox*>(size);

    QDoubleSpinBox *strength = spin(0.01, 1, m_settings.strength, 2, QString(), i18n("How much each dab changes."));
    onValue(strength, &m_settings.strength);
    shared->addRow(i18n("Strength:"), strength);

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
    for (const auto &entry : profiles) profile->addItem(entry.first, int(entry.second));
    profile->setCurrentIndex(profile->findData(int(m_settings.profile)));
    connect(profile, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, profile] (int) {
        m_settings.profile = VPointEdit::Profile(profile->currentData().toInt());
        saveSettings();
    });
    shared->addRow(i18n("Falloff:"), profile);
    shared->addRow(check(i18n("Selected strokes only"), &m_settings.selectedOnly,
                         i18n("The brush changes only the selected strokes.")));

    QStackedWidget *pages = new QStackedWidget(widget);
    outer->addWidget(pages);
    m_pages = pages;
    const std::function<void()> changed = [this] () { saveSettings(); };
    pages->addWidget(VEditActions::smoothPage(pages, &m_settings, changed, true));
    // Thickness
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QComboBox *tool = new QComboBox(page);
        tool->addItem(i18nc("thickness tool", "Thicken"), int(ThicknessTool::Thicken));
        tool->addItem(i18nc("thickness tool", "Thin"), int(ThicknessTool::Thin));
        tool->addItem(i18nc("thickness tool", "Set"), int(ThicknessTool::Set));
        tool->setCurrentIndex(tool->findData(int(m_settings.thicknessTool)));
        form->addRow(i18n("Tool:"), tool);
        QSpinBox *amount = VEditActions::intSlider(page, 1, 100, qRound(m_settings.thicknessAmount * 100), i18n("%"),
                                                   i18n("How much each dab of Thicken or Thin changes the width, "
                                                        "times the strength and the falloff."));
        connect(amount, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] (int v) {
            m_settings.thicknessAmount = v / 100.0;
            saveSettings();
        });
        amount->setEnabled(m_settings.thicknessTool != ThicknessTool::Set);
        form->addRow(i18n("Amount:"), amount);
        QDoubleSpinBox *radius = spin(0.01, 1000, m_settings.setRadius, 2, i18n(" pt"),
                                      i18n("Set brings the line's radius (half its width) under the brush to this. On brush "
                                           "strokes it is the radius at full pressure, so their pressure "
                                           "tapers stay."));
        onValue(radius, &m_settings.setRadius);
        radius->setEnabled(m_settings.thicknessTool == ThicknessTool::Set);
        form->addRow(i18n("Radius:"), radius);
        connect(tool, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, tool, radius, amount] (int) {
            m_settings.thicknessTool = ThicknessTool(tool->currentData().toInt());
            radius->setEnabled(m_settings.thicknessTool == ThicknessTool::Set);
            amount->setEnabled(m_settings.thicknessTool != ThicknessTool::Set);
            saveSettings();
        });
        QLabel *hint = new QLabel(i18n("Holding down Ctrl swaps Thicken and Thin."), page);
        hint->setWordWrap(true);
        form->addRow(hint);
        pages->addWidget(page);
    }
    // Trim
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QComboBox *trimMode = new QComboBox(page);
        trimMode->addItem(i18nc("trim mode", "Drag"), int(VCurveTrim::Selector::Drag));
        trimMode->addItem(i18nc("trim mode", "Lasso"), int(VCurveTrim::Selector::Lasso));
        trimMode->addItem(i18nc("trim mode", "Click"), int(VCurveTrim::Selector::Click));
        trimMode->setCurrentIndex(trimMode->findData(int(m_settings.trimMode)));
        connect(trimMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, trimMode] (int) {
            m_settings.trimMode = VCurveTrim::Selector::Mode(trimMode->currentData().toInt());
            saveSettings();
            repaintDecorations();
        });
        form->addRow(i18n("Trim by:"), trimMode);
        QComboBox *erase = new QComboBox(page);
        erase->addItem(i18nc("trim erase", "To crossings"), int(VCurveTrim::Options::ToCrossings));
        erase->addItem(i18nc("trim erase", "Whole stroke"), int(VCurveTrim::Options::WholeStroke));
        erase->addItem(i18nc("trim erase", "By point"), int(VCurveTrim::Options::Area));
        erase->setCurrentIndex(erase->findData(int(m_settings.trimErase)));
        erase->setToolTip(i18n("To crossings: the part of a stroke between the places it crosses strokes goes.\n"
                               "Whole stroke: every stroke touched goes.\n"
                               "By point: only what is under the brush or inside the lasso goes; the line is cut "
                               "where it leaves the area. A click erases under the brush."));
        form->addRow(i18n("Erase:"), erase);
        form->addRow(check(i18n("Keep caps"), &m_settings.keepCaps, i18n("Cut ends keep the line's caps instead of ending flat.")));
        form->addRow(check(i18n("Keep fill"), &m_settings.keepFill, i18n("A filled shape cut open keeps its fill.")));
        QCheckBox *allLayers = check(i18n("Cut at other layers"), &m_settings.trimAllLayers,
                                     i18n("Crossings with strokes on other visible freehand layers cut too."));
        form->addRow(allLayers);
        QCheckBox *endsOnly = check(i18n("Ends only"), &m_settings.trimEndsOnly,
                                    i18n("Only the first and last piece of a stroke can be trimmed."));
        form->addRow(endsOnly);
        QDoubleSpinBox *leeway = spin(0.0, 20.0, m_settings.trimLeeway, 1, i18n(" px"),
                                      i18n("A stroke that ends this far short of another one still cuts it, "
                                           "as the fill tool's Close Gap closes gaps in lines."));
        onValue(leeway, &m_settings.trimLeeway);
        form->addRow(i18n("Crossing leeway:"), leeway);
        auto showErase = [allLayers, endsOnly, leeway, form] (VCurveTrim::Options::Erase e) {
            allLayers->setEnabled(e == VCurveTrim::Options::ToCrossings);
            endsOnly->setEnabled(e == VCurveTrim::Options::ToCrossings);
            leeway->setEnabled(e == VCurveTrim::Options::ToCrossings);
            if (QWidget *label = form->labelForField(leeway)) label->setEnabled(e == VCurveTrim::Options::ToCrossings);
        };
        showErase(m_settings.trimErase);
        connect(erase, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, erase, showErase] (int) {
            m_settings.trimErase = VCurveTrim::Options::Erase(erase->currentData().toInt());
            showErase(m_settings.trimErase);
            saveSettings();
            repaintDecorations();
        });
        pages->addWidget(page);
    }
    pages->addWidget(VEditActions::simplifyPage(pages, &m_settings, changed));
    pages->addWidget(VEditActions::subdividePage(pages, &m_settings, changed));
    // Opacity/Density (the pages are in the order of Mode)
    {
        QWidget *page = new QWidget(pages);
        QFormLayout *form = new QFormLayout(page);
        QComboBox *tool = new QComboBox(page);
        tool->addItem(i18nc("opacity tool", "Increase"), int(OpacityTool::Increase));
        tool->addItem(i18nc("opacity tool", "Decrease"), int(OpacityTool::Decrease));
        tool->addItem(i18nc("opacity tool", "Set"), int(OpacityTool::Set));
        tool->setCurrentIndex(tool->findData(int(m_settings.opacityTool)));
        form->addRow(i18n("Tool:"), tool);
        // one slider: the target for Set, how far each dab goes for Increase and Decrease
        QSpinBox *value = VEditActions::intSlider(page, 0, 100, 0, i18n("%"), QString());
        QLabel *valueLabel = new QLabel(page);
        form->addRow(valueLabel, value);
        auto showTool = [this, value, valueLabel] () {
            const bool set = m_settings.opacityTool == OpacityTool::Set;
            const QSignalBlocker blocker(value);
            value->setValue(qRound((set ? m_settings.setOpacity : m_settings.changeAmount) * 100));
            valueLabel->setText(set ? i18n("Set to:") : i18n("Amount:"));
            value->setToolTip(set ? i18n("Set brings the opacity or density under the brush to this.")
                                  : i18n("How far each dab moves the opacity or density toward 100% (Increase) "
                                         "or 0% (Decrease), times the strength."));
        };
        showTool();
        connect(value, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] (int v) {
            (m_settings.opacityTool == OpacityTool::Set ? m_settings.setOpacity : m_settings.changeAmount) = v / 100.0;
            saveSettings();
        });
        form->addRow(check(i18n("Opacity"), &m_settings.affectOpacity,
                           i18n("The line's opacity: it fades the line as drawn, as a layer's opacity would.")));
        form->addRow(check(i18n("Density"), &m_settings.affectDensity,
                           i18n("Brush strokes: the density of the brush's dabs (their opacity and flow), on top of "
                                "what pressure gave them. At 100% a stroke drawn lightly becomes solid.")));
        connect(tool, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, tool, showTool] (int) {
            m_settings.opacityTool = OpacityTool(tool->currentData().toInt());
            showTool();
            saveSettings();
        });
        QLabel *hint = new QLabel(i18n("Ctrl+drag swaps Increase and Decrease."), page);
        hint->setWordWrap(true);
        form->addRow(hint);
        pages->addWidget(page);
    }
    pages->setCurrentIndex(int(m_mode));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, mode] (int) {
        setMode(Mode(mode->currentData().toInt()));
        saveSettings();
    });
    outer->addStretch();

    widget->setObjectName(i18n("Vector Edit"));
    return widget;
}

VEditToolFactory::VEditToolFactory()
    : KoToolFactoryBase("VEditTool")
{
    setToolTip(i18n("Vector Edit Tool: smooth, thicken, fade, trim, simplify and subdivide the strokes of freehand vector layers"));
    setSection(ToolBoxSection::Main);
    setIconName(koIconNameCStr("snap-intersection"));
    setPriority(3);
    // always shown; VStrokeToolSwitcher enables the button on freehand layers only
    setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
}

QString VEditToolFactory::actionName(VEditTool::Mode mode)
{
    switch (mode) {
    case VEditTool::Mode::Smooth: return QStringLiteral("VEditToolSmooth");
    case VEditTool::Mode::Thickness: return QStringLiteral("VEditToolThickness");
    case VEditTool::Mode::Trim: return QStringLiteral("VEditToolTrim");
    case VEditTool::Mode::Simplify: return QStringLiteral("VEditToolSimplify");
    case VEditTool::Mode::Subdivide: return QStringLiteral("VEditToolSubdivide");
    case VEditTool::Mode::Opacity: return QStringLiteral("VEditToolOpacity");
    }
    return QString();
}

QList<QAction *> VEditToolFactory::createActionsImpl()
{
    KisActionRegistry *actionRegistry = KisActionRegistry::instance();
    QList<QAction *> actions;

    // as VPointEditToolFactory: the tool manager disables undeclared tool actions
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

    // one shortcut per mode, from any tool (as the transform tool's modes)
    QPointer<KoToolManager> manager = KoToolManager::instance();
    for (VEditTool::Mode mode : AllModes) {
        QAction *action = actionRegistry->makeQAction(actionName(mode), this);
        action->setProperty("always_enabled", true);
        connect(action, &QAction::triggered, this, [this, manager, mode] () {
            if (manager) activateMode(manager, mode);
        });
        actions << action;
    }
    return actions;
}

void VEditToolFactory::activateMode(KoToolManager *toolManager, VEditTool::Mode mode)
{
    KoCanvasController *controller = toolManager->activeCanvasController();
    if (!controller || !controller->canvas()) return;
    VEditTool *tool = dynamic_cast<VEditTool*>(toolManager->toolById(controller->canvas(), id()));
    if (!tool) return;
    if (toolManager->activeToolId() == id()) {
        tool->setMode(mode);
    } else {
        tool->setNextActivationMode(mode);
        toolManager->switchToolRequested(id());
    }
}

KoToolBase *VEditToolFactory::createTool(KoCanvasBase *canvas)
{
    VStrokeToolSwitcher::start();
    return new VEditTool(canvas);
}
