/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VEditActions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTransform>
#include <QFormLayout>
#include <QLabel>
#include <QLineF>
#include <QSpinBox>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KoCanvasBase.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeController.h>
#include <KoShapeGroup.h>
#include <KoViewConverter.h>
#include <KisViewManager.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_canvas2.h>
#include <kis_image.h>
#include <kis_resources_snapshot.h>
#include <kundo2command.h>
#include <commands/KoShapeBackgroundCommand.h>
#include <kis_cubic_curve.h>
#include <kis_curve_widget.h>
#include <kis_shape_layer.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

#include "VCurveOpacity.h"
#include "VCurveShape.h"
#include "VCurveSimplify.h"
#include "VCurveWarp.h"
#include "VGeomCurveShape.h"
#include "VInkSize.h"
#include "VStrokeCurves.h"
#include "VStrokeShape.h"
#include "VStrokeTool.h"

void VEditSettings::load(const QString &group)
{
    KConfigGroup shared = KSharedConfig::openConfig()->group(group);
    size = shared.readEntry("size", size);
    strength = shared.readEntry("strength", strength);
    profile = VPointEdit::Profile(shared.readEntry("profile", int(profile)));
    selectedOnly = shared.readEntry("selectedOnly", selectedOnly);

    KConfigGroup smooth = KSharedConfig::openConfig()->group(group + QStringLiteral("/smooth"));
    // "repeat", not the old "iterations": the two-neighbour smooth needed 2, the Gaussian one 1
    smoothIterations = qBound(1, smooth.readEntry("repeat", smoothIterations), 20);
    smoothReach = qBound(0.01, smooth.readEntry("reach", smoothReach), 2.0);
    smoothDistance = qBound(0.1, smooth.readEntry("distance", smoothDistance), 500.0);
    smoothKeepShape = smooth.readEntry("keepShape", smoothKeepShape);
    smoothPosition = smooth.readEntry("position", smoothPosition);
    smoothRadius = smooth.readEntry("radius", smoothRadius);
    smoothPressure = smooth.readEntry("pressure", smoothPressure);

    KConfigGroup thickness = KSharedConfig::openConfig()->group(group + QStringLiteral("/thickness"));
    thicknessTool = ThicknessTool(qBound(0, thickness.readEntry("tool", int(thicknessTool)),
                                           int(ThicknessTool::Set)));
    setRadius = thickness.readEntry("radius", setRadius);
    thicknessAmount = qBound(0.01, thickness.readEntry("amount", thicknessAmount), 1.0);

    KConfigGroup brush = KSharedConfig::openConfig()->group(group + QStringLiteral("/brush"));
    brushKeepSize = brush.readEntry("keepSize", brushKeepSize);

    KConfigGroup opacity = KSharedConfig::openConfig()->group(group + QStringLiteral("/opacity"));
    opacityTool = OpacityTool(qBound(0, opacity.readEntry("tool", int(opacityTool)),
                                                int(OpacityTool::Set)));
    setOpacity = qBound(0.0, opacity.readEntry("opacity", setOpacity), 1.0);
    changeAmount = qBound(0.0, opacity.readEntry("amount", changeAmount), 1.0);
    affectOpacity = opacity.readEntry("affectOpacity", affectOpacity);
    affectDensity = opacity.readEntry("affectDensity", affectDensity);

    KConfigGroup trim = KSharedConfig::openConfig()->group(group + QStringLiteral("/trim"));
    trimMode = VCurveTrim::Selector::Mode(trim.readEntry("mode", int(trimMode)));
    trimErase = VCurveTrim::Options::Erase(qBound(0, trim.readEntry("erase", int(trimErase)),
                                                             int(VCurveTrim::Options::Area)));
    keepCaps = trim.readEntry("keepCaps", keepCaps);
    keepFill = trim.readEntry("keepFill", keepFill);
    trimAllLayers = trim.readEntry("allLayers", trimAllLayers);
    trimEndsOnly = trim.readEntry("endsOnly", trimEndsOnly);
    trimLeeway = qBound(0.0, trim.readEntry("leeway", trimLeeway), 20.0);

    KConfigGroup simplify = KSharedConfig::openConfig()->group(group + QStringLiteral("/simplify"));
    simplifyMethod = VEditSettings::SimplifyMethod(qBound(0, simplify.readEntry("method", int(simplifyMethod)),
                                                      int(VEditSettings::SimplifyMethod::Progressive)));
    simplifyTolerance = simplify.readEntry("tolerance", simplifyTolerance);
    attributeTolerance = simplify.readEntry("attributeTolerance", attributeTolerance);
    joinEnds = simplify.readEntry("joinEnds", joinEnds);
    joinGap = simplify.readEntry("joinGap", joinGap);
    joinAngle = simplify.readEntry("joinAngle", joinAngle);
    joinTransition = simplify.readEntry("joinTransition", joinTransition);
    mergeOverlapping = simplify.readEntry("mergeOverlapping", mergeOverlapping);
    mergeDistance = simplify.readEntry("mergeDistance", mergeDistance);
    mergeFitError = simplify.readEntry("mergeFitError", mergeFitError);
    mergeAdjustWidth = simplify.readEntry("mergeAdjustWidth", mergeAdjustWidth);
    mergeRadius = simplify.readEntry("mergeRadius", mergeRadius);
    mergeBias = simplify.readEntry("mergeBias", mergeBias);
    mergeTaper = simplify.readEntry("mergeTaper", mergeTaper);

    KConfigGroup subdivide = KSharedConfig::openConfig()->group(group + QStringLiteral("/subdivide"));
    subdivideMethod = SubdivideMethod(qBound(0, subdivide.readEntry("method", int(subdivideMethod)),
                                             int(SubdivideMethod::Steps)));
    subdivideSpacing = subdivide.readEntry("spacing", subdivideSpacing);
    subdivideSteps = qBound(1, subdivide.readEntry("steps", subdivideSteps), VCurveData::MaxSubdivideSteps);
    subdivideSmooth = subdivide.readEntry("smooth", subdivideSmooth);
}

void VEditSettings::save(const QString &group) const
{
    KConfigGroup shared = KSharedConfig::openConfig()->group(group);
    shared.writeEntry("size", size);
    shared.writeEntry("strength", strength);
    shared.writeEntry("profile", int(profile));
    shared.writeEntry("selectedOnly", selectedOnly);

    KConfigGroup smooth = KSharedConfig::openConfig()->group(group + QStringLiteral("/smooth"));
    smooth.writeEntry("repeat", smoothIterations);
    smooth.writeEntry("reach", smoothReach);
    smooth.writeEntry("distance", smoothDistance);
    smooth.writeEntry("keepShape", smoothKeepShape);
    smooth.writeEntry("position", smoothPosition);
    smooth.writeEntry("radius", smoothRadius);
    smooth.writeEntry("pressure", smoothPressure);

    KConfigGroup thickness = KSharedConfig::openConfig()->group(group + QStringLiteral("/thickness"));
    thickness.writeEntry("tool", int(thicknessTool));
    thickness.writeEntry("radius", setRadius);
    thickness.writeEntry("amount", thicknessAmount);

    KConfigGroup brush = KSharedConfig::openConfig()->group(group + QStringLiteral("/brush"));
    brush.writeEntry("keepSize", brushKeepSize);

    KConfigGroup opacity = KSharedConfig::openConfig()->group(group + QStringLiteral("/opacity"));
    opacity.writeEntry("tool", int(opacityTool));
    opacity.writeEntry("opacity", setOpacity);
    opacity.writeEntry("amount", changeAmount);
    opacity.writeEntry("affectOpacity", affectOpacity);
    opacity.writeEntry("affectDensity", affectDensity);

    KConfigGroup trim = KSharedConfig::openConfig()->group(group + QStringLiteral("/trim"));
    trim.writeEntry("mode", int(trimMode));
    trim.writeEntry("erase", int(trimErase));
    trim.writeEntry("keepCaps", keepCaps);
    trim.writeEntry("keepFill", keepFill);
    trim.writeEntry("allLayers", trimAllLayers);
    trim.writeEntry("endsOnly", trimEndsOnly);
    trim.writeEntry("leeway", trimLeeway);

    KConfigGroup simplify = KSharedConfig::openConfig()->group(group + QStringLiteral("/simplify"));
    simplify.writeEntry("method", int(simplifyMethod));
    simplify.writeEntry("tolerance", simplifyTolerance);
    simplify.writeEntry("attributeTolerance", attributeTolerance);
    simplify.writeEntry("joinEnds", joinEnds);
    simplify.writeEntry("joinGap", joinGap);
    simplify.writeEntry("joinAngle", joinAngle);
    simplify.writeEntry("joinTransition", joinTransition);
    simplify.writeEntry("mergeOverlapping", mergeOverlapping);
    simplify.writeEntry("mergeDistance", mergeDistance);
    simplify.writeEntry("mergeFitError", mergeFitError);
    simplify.writeEntry("mergeAdjustWidth", mergeAdjustWidth);
    simplify.writeEntry("mergeRadius", mergeRadius);
    simplify.writeEntry("mergeBias", mergeBias);
    simplify.writeEntry("mergeTaper", mergeTaper);

    KConfigGroup subdivide = KSharedConfig::openConfig()->group(group + QStringLiteral("/subdivide"));
    subdivide.writeEntry("method", int(subdivideMethod));
    subdivide.writeEntry("spacing", subdivideSpacing);
    subdivide.writeEntry("steps", subdivideSteps);
    subdivide.writeEntry("smooth", subdivideSmooth);
}

VCurveData VEditSettings::subdivide(const VCurveData &data, const QTransform &toDocument, const QVector<bool> &mask) const
{
    if (subdivideMethod == SubdivideMethod::Steps) return data.subdividedSteps(subdivideSteps, mask, subdivideSmooth);
    return data.subdivided(subdivideSpacing, toDocument, mask, subdivideSmooth);
}

namespace {
/// the curves @p shape stands for: a group stands for the ones in it
void collectCurves(KoShape *shape, QList<VCurveShape*> *curves)
{
    if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) {
        if (curve->isVisible() && curve->isShapeEditable() && !curves->contains(curve)) *curves << curve;
    } else if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
        Q_FOREACH (KoShape *child, group->shapes()) collectCurves(child, curves);
    }
}
}

namespace {
/// builds the rows of an option page, reporting every change of the settings
struct OptionRows {
    QWidget *parent;
    std::function<void()> changed;

    QDoubleSpinBox *spin(qreal min, qreal max, qreal value, int decimals, const QString &suffix, const QString &tip) const
    {
        return VEditActions::doubleSlider(parent, min, max, value, decimals, suffix, tip);
    }
    void onValue(QDoubleSpinBox *box, qreal *target) const
    {
        const std::function<void()> report = changed;
        QObject::connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), parent, [report, target] (double v) {
            *target = v;
            report();
        });
    }
    QCheckBox *check(const QString &text, bool *target, const QString &tip) const
    {
        QCheckBox *box = new QCheckBox(text, parent);
        box->setChecked(*target);
        box->setToolTip(tip);
        const std::function<void()> report = changed;
        QObject::connect(box, &QCheckBox::toggled, parent, [report, target] (bool on) {
            *target = on;
            report();
        });
        return box;
    }
};
}

KisDoubleSliderSpinBox *VEditActions::doubleSlider(QWidget *parent, qreal min, qreal max, qreal value, int decimals,
                                                   const QString &suffix, const QString &tip)
{
    KisDoubleSliderSpinBox *box = new KisDoubleSliderSpinBox(parent);
    box->setRange(min, max, decimals, false);
    box->setFastSliderStep(max <= 1.0 ? 0.01 : decimals == 0 ? 1.0 : 0.5);
    // the arrow keys and the wheel: Qt's step of 1 would cross a range of 0 to 1 at once
    box->setSingleStep(max <= 1.0 ? 0.01 : 1.0);
    // wide ranges (sizes, spacings) give the small values more of the slider, as brush sizes
    if (max >= 100.0 && decimals > 0) box->setExponentRatio(3.0);
    box->setValue(value);
    box->setSuffix(suffix);
    box->setToolTip(tip);
    return box;
}

KisSliderSpinBox *VEditActions::intSlider(QWidget *parent, int min, int max, int value, const QString &suffix,
                                          const QString &tip)
{
    KisSliderSpinBox *box = new KisSliderSpinBox(parent);
    box->setRange(min, max, false);
    box->setFastSliderStep(1);
    box->setValue(value);
    box->setSuffix(suffix);
    box->setToolTip(tip);
    return box;
}

VEditActions::VEditActions(KoCanvasBase *canvas, const VEditSettings *settings)
    : m_canvas(canvas),
      m_settings(settings)
{
}

QList<VCurveShape*> VEditActions::targets(KisShapeLayer *l, bool selectedOnly) const
{
    QList<VCurveShape*> result;
    if (!l) return result;

    KoSelection *selection = m_canvas->selectedShapesProxy()->selection();
    if (selectedOnly) {
        Q_FOREACH (KoShape *shape, selection->selectedEditableShapes()) {
            KoShape *top = shape;
            while (dynamic_cast<KoShapeGroup*>(top->parent())) top = top->parent();
            if (l->shapes().contains(top)) collectCurves(shape, &result);
        }
        return result;
    }

    Q_FOREACH (KoShape *shape, l->shapes()) collectCurves(shape, &result);
    return result;
}

void VEditActions::begin()
{
    m_before.clear();
}

void VEditActions::touch(VCurveShape *shape)
{
    if (!m_before.contains(shape)) m_before.insert(shape, shape->curves());
}

qreal VEditActions::radiusScale(const VCurveShape *shape)
{
    return VInkSize::radiusScale(shape);
}

bool VEditActions::compatible(const VCurveShape *a, const VCurveShape *b)
{
    if (a == b) return true;
    const VStrokeShape *sa = dynamic_cast<const VStrokeShape*>(a);
    const VStrokeShape *sb = dynamic_cast<const VStrokeShape*>(b);
    if (sa || sb) {
        // brush strokes join only when drawn with the same brush
        if (!sa || !sb) return false;
        const VStrokeData &da = sa->data();
        const VStrokeData &db = sb->data();
        const bool samePreset = (da.presetDefinition && da.presetDefinition == db.presetDefinition) ||
                                (!da.presetMd5.isEmpty() && da.presetMd5 == db.presetMd5);
        return samePreset && da.brushSize == db.brushSize && da.opacity == db.opacity && da.compositeOp == db.compositeOp &&
               sa->color() == sb->color();
    }
    const VGeomCurveShape *ga = dynamic_cast<const VGeomCurveShape*>(a);
    const VGeomCurveShape *gb = dynamic_cast<const VGeomCurveShape*>(b);
    return ga && gb && ga->lineColor() == gb->lineColor() && !ga->background() && !gb->background();
}

void VEditActions::acrossShapes(const QList<VCurveShape*> &shapes,
                             const std::function<QVector<VDocStroke>(const QVector<VDocStroke> &)> &op,
                             QList<VCurveShape*> *removed, QList<VCurveShape*> *changed)
{
    QVector<VDocStroke> strokes;
    for (int owner = 0; owner < shapes.size(); owner++) {
        const VCurveShape *shape = shapes[owner];
        const QTransform toDocument = shape->absoluteTransformation();
        const qreal scale = radiusScale(shape);
        for (int c = 0; c < shape->curves().curveCount(); c++) {
            VCurveData curve = shape->curves().curve(c);
            curve.transform(toDocument);
            for (qreal &r : curve.radii) r *= scale;
            strokes << VDocStroke{curve, owner, false, c};
        }
    }

    const QVector<VDocStroke> result = op(strokes);

    QVector<QVector<const VDocStroke*>> byOwner(shapes.size());
    QVector<bool> touched(shapes.size(), false);
    QVector<int> counts(shapes.size(), 0);
    for (const VDocStroke &s : result) {
        if (s.owner < 0 || s.owner >= shapes.size()) continue;
        touched[s.owner] |= s.changed;
        if (!s.curve.isEmpty()) byOwner[s.owner] << &s;
    }
    for (const VDocStroke &s : strokes) counts[s.owner]++;
    for (int owner = 0; owner < shapes.size(); owner++) {
        // strokes that went to another shape count as a change too
        if (byOwner[owner].size() != counts[owner]) touched[owner] = true;
    }

    for (int owner = 0; owner < shapes.size(); owner++) {
        if (!touched[owner]) continue;
        VCurveShape *shape = shapes[owner];
        if (byOwner[owner].isEmpty()) {
            touch(shape);
            *removed << shape;
            continue;
        }
        const QTransform toLocal = shape->absoluteTransformation().inverted();
        const qreal scale = radiusScale(shape);
        VCurveData data;
        for (const VDocStroke *s : byOwner[owner]) {
            if (!s->changed && s->source >= 0 && shapes[s->owner] == shape) {
                // untouched sub-paths stay bit-exact
                data.append(shape->curves().curve(s->source));
                continue;
            }
            VCurveData local = s->curve;
            local.transform(toLocal);
            for (qreal &r : local.radii) r /= scale;
            data.append(local);
        }
        if (data == shape->curves()) continue;
        touch(shape);
        shape->setCurvesAndUpdate(data);
        if (changed) *changed << shape;
    }
}

void VEditActions::runJoins(const QList<VCurveShape*> &shapes, const std::function<bool(const QPointF &)> &inArea,
                         QList<VCurveShape*> *removed)
{
    if (!m_settings->joinEnds && !m_settings->mergeOverlapping) return;
    const QList<VCurveShape*> owners = shapes;
    auto ownersCompatible = [owners] (int a, int b) { return compatible(owners[a], owners[b]); };

    if (m_settings->mergeOverlapping) {
        VCurveSimplify::MergeOptions options;
        options.distance = m_settings->mergeDistance;
        options.fitError = m_settings->mergeFitError;
        options.adjustWidth = m_settings->mergeAdjustWidth;
        options.radius = m_settings->mergeRadius;
        options.bias = m_settings->mergeBias;
        const KisCubicCurve taper(m_settings->mergeTaper);
        if (!taper.isConstant(1.0)) {
            options.taper = [taper] (qreal t) { return taper.value(t); };
        }
        auto eligible = [inArea] (const VDocStroke &s) {
            if (!inArea) return true;
            return std::any_of(s.curve.positions.begin(), s.curve.positions.end(), inArea);
        };
        acrossShapes(owners, [&] (const QVector<VDocStroke> &strokes) {
            return VCurveSimplify::mergeOverlapping(strokes, options, ownersCompatible, eligible);
        }, removed);
    }
    if (m_settings->joinEnds) {
        VCurveSimplify::JoinOptions options;
        options.gap = m_settings->joinGap;
        options.angleTolerance = qDegreesToRadians(m_settings->joinAngle);
        options.transition = m_settings->joinTransition;
        QList<VCurveShape*> left;
        for (VCurveShape *shape : owners) {
            if (!removed->contains(shape)) left << shape;
        }
        auto leftCompatible = [left] (int a, int b) { return compatible(left[a], left[b]); };
        acrossShapes(left, [&] (const QVector<VDocStroke> &strokes) {
            return VCurveSimplify::joinEnds(strokes, options, leftCompatible, inArea);
        }, removed);
    }
}

void VEditActions::commit(const KUndo2MagicString &text, const QList<VCurveShape*> &removed,
                       const QList<VCurveShape*> &dropFill)
{
    QList<VCurveShape*> shapes;
    QList<VCurveData> before;
    QList<VCurveData> after;
    for (auto it = m_before.constBegin(); it != m_before.constEnd(); ++it) {
        if (it.key()->curves() == it.value()) continue;
        shapes << it.key();
        before << it.value();
        after << it.key()->curves();
    }
    m_before.clear();
    if (shapes.isEmpty() && removed.isEmpty() && dropFill.isEmpty()) return;

    KUndo2Command *parent = new KUndo2Command(text);
    if (!shapes.isEmpty()) new VCurveEditCommand(shapes, before, after, text, parent);
    for (VCurveShape *shape : dropFill) {
        new KoShapeBackgroundCommand(shape, QSharedPointer<KoShapeBackground>(), parent);
    }
    if (!removed.isEmpty()) {
        QList<KoShape*> list;
        for (VCurveShape *shape : removed) list << shape;
        m_canvas->shapeController()->removeShapes(list, parent);
    }
    m_canvas->addCommand(parent);
}

void VEditActions::applyWhole(Whole what, const QList<VCurveShape*> &shapes,
                              const std::function<QVector<qreal>(VCurveShape *)> &pointWeights)
{
    m_before.clear();
    QList<VCurveShape*> removed;
    KUndo2MagicString text;

    for (VCurveShape *shape : shapes) {
        VCurveData data = shape->curves();
        const QTransform toDocument = shape->absoluteTransformation();
        const QVector<qreal> weights = pointWeights ? pointWeights(shape) : QVector<qreal>();
        QVector<bool> pointMask;
        if (!weights.isEmpty()) {
            pointMask.resize(weights.size());
            for (int i = 0; i < weights.size(); i++) pointMask[i] = weights[i] > 0;
        }
        switch (what) {
        case Whole::Smooth: {
            smooth(data, toDocument, weights, m_settings->strength, m_settings->smoothDistance, *m_settings);
            text = kundo2_i18n("Smooth Strokes");
            break;
        }
        case Whole::Simplify: {
            if (m_settings->simplifyMethod == VEditSettings::SimplifyMethod::Progressive) {
                data = VCurveSimplify::progressive(data, toDocument, 0.5 * m_settings->strength, weights);
            } else {
                data = VCurveSimplify::thin(data, toDocument,
                                            m_canvas->viewConverter()->viewToDocumentX(m_settings->simplifyTolerance),
                                            m_settings->attributeTolerance, pointMask);
            }
            text = kundo2_i18n("Simplify Strokes");
            break;
        }
        case Whole::Subdivide: {
            // the segments between points with a weight
            QVector<bool> segmentMask;
            if (!pointMask.isEmpty()) {
                segmentMask.resize(pointMask.size());
                for (int i = 0; i < pointMask.size(); i++) {
                    const int next = data.nextIndex(i);
                    segmentMask[i] = pointMask[i] && next >= 0 && pointMask[next];
                }
            }
            data = m_settings->subdivide(data, toDocument, segmentMask);
            text = kundo2_i18n("Subdivide Strokes");
            break;
        }
        }
        if (data == shape->curves()) continue;
        touch(shape);
        shape->setCurvesAndUpdate(data);
    }
    if (what == Whole::Simplify) runJoins(shapes, std::function<bool(const QPointF &)>(), &removed);
    commit(text, removed);
}

void VEditActions::moveRadiusToward(VCurveData &data, int i, qreal target, qreal amount)
{
    // the radius at full pressure: a brush stroke's radius is half the brush size
    // times the pressure (VStrokeCurves::baseRadius), and its brush makes light
    // tips thinner on its own, so the size multiplier stays the same along the
    // stroke; drawn shapes have no pressure. The whole way at full strength and weight
    const qreal pressureTarget = target * data.attribute(QStringLiteral("pressure"), i, 1.0);
    data.radii[i] += (pressureTarget - data.radii[i]) * amount;
}

namespace {

/// Apply Current Brush: each stroke's brush settings, and its curves when the size changed
class VStrokeBrushCommand : public KUndo2Command
{
public:
    struct Change {
        VStrokeShape *shape;
        VStrokeData before;
        VStrokeData after;
        VCurveData curvesBefore;
        VCurveData curvesAfter;
    };

    VStrokeBrushCommand(const std::vector<Change> &changes, const KUndo2MagicString &text)
        : KUndo2Command(text), m_changes(changes) {}

    void redo() override {
        for (const Change &c : m_changes) {
            c.shape->setData(c.after);
            if (!(c.shape->curves() == c.curvesAfter)) c.shape->setCurvesAndUpdate(c.curvesAfter);
        }
    }
    void undo() override {
        for (const Change &c : m_changes) {
            c.shape->setData(c.before);
            if (!(c.shape->curves() == c.curvesBefore)) c.shape->setCurvesAndUpdate(c.curvesBefore);
        }
    }

private:
    std::vector<Change> m_changes;
};

} // namespace

VStrokeData VEditActions::withBrush(const VStrokeData &data, KisPaintOpPresetSP preset, KisPaintOpPresetSP original,
                                    bool keepSize)
{
    VStrokeData result(data);
    // a copy: the stroke's size goes into it, and the stroke keeps it apart from the docker's preset
    KisPaintOpPresetSP copy = preset->clone().dynamicCast<KisPaintOpPreset>();
    if (keepSize) copy->settings()->setPaintOpSize(data.brushSize);
    result.preset = copy;
    result.presetDefinition.reset();
    if (!original) original = preset;
    result.presetName = original->name();
    result.presetFileName = original->filename();
    result.presetMd5 = original->md5Sum(false);
    result.brushSize = copy->settings()->paintOpSize();
    // kept as drawn: the color (the shape's background), blend mode, tool opacity, seeds, start angle,
    // background color, and every point's pressure, tilt, rotation, radius, opacity and density
    return result;
}

KUndo2Command *VEditActions::brushCommand(const QList<VCurveShape*> &shapes, KisPaintOpPresetSP preset,
                                          KisPaintOpPresetSP original, bool keepSize, int *count)
{
    if (count) *count = 0;
    if (!preset) return nullptr;
    std::vector<VStrokeBrushCommand::Change> changes;
    for (VCurveShape *curveShape : shapes) {
        VStrokeShape *shape = dynamic_cast<VStrokeShape*>(curveShape);
        if (!shape) continue;
        VStrokeBrushCommand::Change c{shape, shape->data(), withBrush(shape->data(), preset, original, keepSize),
                                      shape->curves(), shape->curves()};
        const qreal factor = c.before.brushSize > 0 ? c.after.brushSize / c.before.brushSize : 1.0;
        if (!keepSize && factor > 0 && !qFuzzyCompare(factor, 1.0)) {
            for (qreal &r : c.curvesAfter.radii) r *= factor;
        }
        changes.push_back(c);
    }
    if (count) *count = int(changes.size());
    if (changes.empty()) return nullptr;
    return new VStrokeBrushCommand(changes, kundo2_i18n("Change Stroke Brush"));
}

int VEditActions::applyCurrentBrush(KoCanvasBase *canvas, const QList<VCurveShape*> &shapes, bool keepSize)
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas);
    if (!kisCanvas || !kisCanvas->viewManager()) return 0;
    KisViewManager *view = kisCanvas->viewManager();
    // the preset as the vector brush would draw with it
    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(kisCanvas->image(), view->activeNode(),
                                                                canvas->resourceManager());
    KisPaintOpPresetSP original = canvas->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset)
                                      .value<KisPaintOpPresetSP>();
    const QString reason = VStrokeTool::unsupportedPresetReason(resources->currentPaintOpPreset());
    if (!reason.isEmpty()) {
        view->showFloatingMessage(reason, QIcon());
        return 0;
    }
    bool anyStroke = false;
    for (VCurveShape *shape : shapes) anyStroke |= dynamic_cast<VStrokeShape*>(shape) != nullptr;
    if (!anyStroke) {
        view->showFloatingMessage(i18n("Select brush strokes to change their brush."), QIcon());
        return 0;
    }
    int changed = 0;
    KUndo2Command *command = brushCommand(shapes, VStrokeTool::vectorPreset(resources->currentPaintOpPreset()),
                                          original, keepSize, &changed);
    if (command) canvas->addCommand(command);
    view->showFloatingMessage(i18np("Changed the brush of 1 stroke to %2.", "Changed the brush of %1 strokes to %2.",
                                    changed, original ? original->name() : QString()), QIcon());
    return changed;
}

void VEditActions::changeThickness(VCurveData &data, const QVector<qreal> &weights, qreal strength, bool thicken,
                                   qreal amount)
{
    for (int i = 0; i < data.pointCount(); i++) {
        const qreal w = weights[i] * strength;
        if (w <= 0) continue;
        // Thin at 100% and full weight goes to 0, which setCurves() lifts to VCurveData::MinRadius
        data.radii[i] *= thicken ? 1.0 + amount * w : qMax(0.0, 1.0 - amount * w);
    }
}

void VEditActions::changeOpacity(VCurveData &data, VCurveShape *shape, const QVector<qreal> &weights, qreal strength,
                                 VEditSettings::OpacityTool tool, const VEditSettings &settings)
{
    // how far a dab goes toward the target (1 for Increase, 0 for Decrease): the Amount for those
    auto step = [&] (qreal w) { return tool == VEditSettings::OpacityTool::Set ? w : settings.changeAmount * w; };
    const qreal target = tool == VEditSettings::OpacityTool::Set ? settings.setOpacity
                       : tool == VEditSettings::OpacityTool::Increase ? 1.0 : 0.0;
    if (settings.affectOpacity) {
        QVector<qreal> opacity = data.attributes.value(VCurveOpacity::Attribute,
                                                       QVector<qreal>(data.pointCount(), 1.0));
        bool opaque = true;
        for (int i = 0; i < data.pointCount(); i++) {
            const qreal w = weights[i] * strength;
            if (w > 0) {
                // Increase and Decrease: the Amount times the strength per dab (10% by default, as Thickness)
                opacity[i] += (target - opacity[i]) * step(w);
                // close enough to opaque is opaque, so the mask can go
                if (opacity[i] > 0.999) opacity[i] = 1.0;
            }
            opaque &= opacity[i] >= 1.0;
        }
        if (opaque) {
            data.attributes.remove(VCurveOpacity::Attribute);
        } else {
            data.attributes.insert(VCurveOpacity::Attribute, opacity);
        }
    }
    if (settings.affectDensity && dynamic_cast<VStrokeShape*>(shape)) {
        // the brush's density d becomes scale * d + offset; each dab moves that toward
        // the target as the opacity above: scale * (1 - a), offset * (1 - a) + a * target
        QVector<qreal> scale = data.attributes.value(VStrokeCurves::DensityScale,
                                                     QVector<qreal>(data.pointCount(), 1.0));
        QVector<qreal> offset = data.attributes.value(VStrokeCurves::DensityOffset,
                                                      QVector<qreal>(data.pointCount(), 0.0));
        bool asDrawn = true;
        for (int i = 0; i < data.pointCount(); i++) {
            const qreal w = weights[i] * strength;
            if (w > 0) {
                const qreal a = step(w);
                scale[i] *= 1.0 - a;
                offset[i] = offset[i] * (1.0 - a) + a * target;
                if (scale[i] < 0.001) scale[i] = 0.0;
                if (offset[i] > 0.999) offset[i] = 1.0;
            }
            asDrawn &= scale[i] == 1.0 && offset[i] == 0.0;
        }
        if (asDrawn) {
            data.attributes.remove(VStrokeCurves::DensityScale);
            data.attributes.remove(VStrokeCurves::DensityOffset);
        } else {
            data.attributes.insert(VStrokeCurves::DensityScale, scale);
            data.attributes.insert(VStrokeCurves::DensityOffset, offset);
        }
    }
}

void VEditActions::smooth(VCurveData &data, const QTransform &toDocument, const QVector<qreal> &weights, qreal strength,
                          qreal documentDistance, const VEditSettings &settings)
{
    VCurveWarp::AffectFlags affect;
    if (settings.smoothPosition) affect |= VCurveWarp::Affect::Position;
    if (settings.smoothRadius) affect |= VCurveWarp::Affect::Radius;
    if (settings.smoothPressure) affect |= VCurveWarp::Affect::Pressure;
    // the curves' units per document point
    const qreal scale = std::sqrt(std::abs(toDocument.determinant()));
    if (scale <= 0) return;
    VCurveWarp::smoothAlong(data, weights, strength, documentDistance / scale, settings.smoothIterations, affect,
                            settings.smoothKeepShape);
}

QWidget *VEditActions::smoothPage(QWidget *parent, VEditSettings *s, const std::function<void()> &changed, bool brush)
{
    OptionRows options{parent, changed};
    QWidget *page = new QWidget(parent);
    QFormLayout *form = new QFormLayout(page);
    const QString distanceTip = i18n("How far along the line Smooth averages. Wobbles shorter than this go; "
                                     "it works the same however densely the points lie.");
    if (brush) {
        QSpinBox *reach = intSlider(page, 1, 200, qRound(s->smoothReach * 100), i18n("%"),
                                    distanceTip + QLatin1Char(' ') + i18n("A share of the brush size."));
        QObject::connect(reach, QOverload<int>::of(&QSpinBox::valueChanged), parent, [s, changed] (int v) {
            s->smoothReach = v / 100.0;
            changed();
        });
        form->addRow(i18n("Distance:"), reach);
    } else {
        QDoubleSpinBox *distance = options.spin(0.1, 500.0, s->smoothDistance, 1, i18n(" pt"), distanceTip);
        options.onValue(distance, &s->smoothDistance);
        form->addRow(i18n("Distance:"), distance);
    }
    QSpinBox *iterations = intSlider(page, 1, 20, s->smoothIterations, QString(),
                                     i18n("How many times each dab or Apply smooths."));
    QObject::connect(iterations, QOverload<int>::of(&QSpinBox::valueChanged), parent, [s, changed] (int v) {
        s->smoothIterations = v;
        changed();
    });
    form->addRow(i18n("Iterations:"), iterations);
    form->addRow(options.check(i18n("Keep shape"), &s->smoothKeepShape,
                               i18n("Long curves hardly shrink: only wobbles shorter than the distance go.")));
    form->addRow(options.check(i18n("Position"), &s->smoothPosition, i18n("Smooth the line.")));
    form->addRow(options.check(i18n("Radius"), &s->smoothRadius, i18n("Smooth the line width.")));
    form->addRow(options.check(i18n("Pressure"), &s->smoothPressure, i18n("Smooth the brush pressure.")));
    return page;
}

QWidget *VEditActions::simplifyPage(QWidget *parent, VEditSettings *s, const std::function<void()> &changed)
{
    OptionRows options{parent, changed};
    QWidget *page = new QWidget(parent);
    QFormLayout *form = new QFormLayout(page);
    QComboBox *method = new QComboBox(page);
    method->addItem(i18nc("simplify method", "Tolerance"), int(VEditSettings::SimplifyMethod::Tolerance));
    method->addItem(i18nc("simplify method", "Progressive"), int(VEditSettings::SimplifyMethod::Progressive));
    method->setCurrentIndex(method->findData(int(s->simplifyMethod)));
    method->setToolTip(i18n("Tolerance: takes every point that can go while the line stays within the tolerances.\n"
                            "Progressive: takes a share of the points with each dab or Apply, the least "
                            "important first, with no tolerance. Going over a stroke again keeps simplifying it, "
                            "down to a straight line. Strength sets the share."));
    form->addRow(i18n("Method:"), method);
    QDoubleSpinBox *tolerance = options.spin(0.05, 50, s->simplifyTolerance, 2, i18n(" px"),
                                     i18n("How far, on screen, the line may move."));
    options.onValue(tolerance, &s->simplifyTolerance);
    form->addRow(i18n("Tolerance:"), tolerance);
    QSpinBox *attributes = intSlider(page, 0, 100, qRound(s->attributeTolerance * 100), i18n("%"), QString());
    attributes->setSpecialValueText(i18nc("width/pressure tolerance", "Off"));
    attributes->setToolTip(i18n("A point also stays when removing it would change the line's width there by more "
                                "than this share of the width, or its pressure (or tilt, rotation, opacity) by more "
                                "than this share of the full range. Off: only the line's position counts."));
    QObject::connect(attributes, QOverload<int>::of(&QSpinBox::valueChanged), parent, [s, changed] (int v) {
        s->attributeTolerance = v / 100.0;
        changed();
    });
    form->addRow(i18n("Width/pressure:"), attributes);
    auto showMethod = [tolerance, attributes] (VEditSettings::SimplifyMethod m) {
        tolerance->setEnabled(m == VEditSettings::SimplifyMethod::Tolerance);
        attributes->setEnabled(m == VEditSettings::SimplifyMethod::Tolerance);
    };
    showMethod(s->simplifyMethod);
    QObject::connect(method, QOverload<int>::of(&QComboBox::currentIndexChanged), parent, [s, changed, method, showMethod] (int) {
        s->simplifyMethod = VEditSettings::SimplifyMethod(method->currentData().toInt());
        showMethod(s->simplifyMethod);
        changed();
    });

    form->addRow(options.check(i18n("Join ends"), &s->joinEnds,
                       i18n("Strokes whose ends meet in about the same direction become one.")));
    QDoubleSpinBox *gap = options.spin(0.1, 500, s->joinGap, 1, i18n(" pt"), i18n("Ends this close join."));
    options.onValue(gap, &s->joinGap);
    form->addRow(i18n("Gap:"), gap);
    QDoubleSpinBox *angle = options.spin(0, 180, s->joinAngle, 0, i18n("°"), i18n("How far the line may bend at a join."));
    options.onValue(angle, &s->joinAngle);
    form->addRow(i18n("Angle:"), angle);
    QDoubleSpinBox *transition = options.spin(0, 0.5, s->joinTransition, 2, QString(),
                                      i18n("The part of each stroke that bends to meet the other. 0 bridges the gap with a straight line."));
    options.onValue(transition, &s->joinTransition);
    form->addRow(i18n("Transition:"), transition);

    form->addRow(options.check(i18n("Merge overlapping"), &s->mergeOverlapping,
                       i18n("Overlapping, sketchy strokes become one clean stroke along their middle.")));
    QDoubleSpinBox *distance = options.spin(0.1, 200, s->mergeDistance, 1, i18n(" pt"),
                                    i18n("Strokes about this close overlap."));
    options.onValue(distance, &s->mergeDistance);
    form->addRow(i18n("Distance:"), distance);
    QDoubleSpinBox *fit = options.spin(0.05, 50, s->mergeFitError, 2, i18n(" pt"),
                               i18n("How close the new line stays to the middle of the strokes."));
    options.onValue(fit, &s->mergeFitError);
    form->addRow(i18n("Fit:"), fit);

    form->addRow(options.check(i18n("Adjust width"), &s->mergeAdjustWidth,
                       i18n("Merged lines get a width pulled toward a radius, then tapered from start to end.")));
    QDoubleSpinBox *mergeRadius = options.spin(0.01, 1000, s->mergeRadius, 2, i18n(" pt"),
                                       i18n("The radius (half the width) merged lines are pulled toward."));
    options.onValue(mergeRadius, &s->mergeRadius);
    form->addRow(i18n("Radius:"), mergeRadius);
    QDoubleSpinBox *bias = options.spin(0, 1, s->mergeBias, 2, QString(),
                                i18n("How far merged lines go toward the radius: 0 keeps the averaged width of the "
                                     "merged strokes, 1 gives the radius exactly."));
    options.onValue(bias, &s->mergeBias);
    form->addRow(i18n("Bias:"), bias);
    KisCurveWidget *taper = new KisCurveWidget(page);
    taper->setCurve(KisCubicCurve(s->mergeTaper));
    taper->setMinimumHeight(80);
    taper->setToolTip(i18n("The width along merged lines, from the start (left) to the end (right), "
                           "as a share of the width set above."));
    QObject::connect(taper, &KisCurveWidget::modified, parent, [s, changed, taper] () {
        s->mergeTaper = taper->curve().toString();
        changed();
    });
    form->addRow(i18n("Taper:"), taper);
    return page;
}

QWidget *VEditActions::subdividePage(QWidget *parent, VEditSettings *s, const std::function<void()> &changed)
{
    OptionRows options{parent, changed};
    QWidget *page = new QWidget(parent);
    QFormLayout *form = new QFormLayout(page);
    QComboBox *method = new QComboBox(page);
    method->setObjectName("cmbSubdivideMethod");
    method->addItem(i18nc("subdivide method", "Distance"), int(VEditSettings::SubdivideMethod::Distance));
    method->addItem(i18nc("subdivide method", "Steps"), int(VEditSettings::SubdivideMethod::Steps));
    method->setCurrentIndex(method->findData(int(s->subdivideMethod)));
    method->setToolTip(i18n("Distance: segments are split until none is longer than the spacing.\n"
                            "Steps: every segment is split in half, once per step."));
    form->addRow(i18n("Method:"), method);
    QDoubleSpinBox *spacing = options.spin(0.1, 500, s->subdivideSpacing, 1, i18n(" pt"),
                                   i18n("No segment stays longer than this."));
    options.onValue(spacing, &s->subdivideSpacing);
    form->addRow(i18n("Spacing:"), spacing);
    QSpinBox *steps = intSlider(page, 1, VCurveData::MaxSubdivideSteps, s->subdivideSteps, QString(),
                                i18n("How many times each segment is split in half: each step doubles the points."));
    steps->setObjectName("spnSubdivideSteps");
    QObject::connect(steps, QOverload<int>::of(&QSpinBox::valueChanged), parent, [s, changed] (int v) {
        s->subdivideSteps = v;
        changed();
    });
    form->addRow(i18n("Steps:"), steps);
    QCheckBox *smooth = options.check(i18n("Subdivide and smooth"), &s->subdivideSmooth,
                                      i18n("New points go on a smooth curve through the points around them "
                                           "(Catmull-Rom), so the line rounds out as it gains points. "
                                           "Off: new points go on the straight line between the old ones."));
    smooth->setObjectName("chkSubdivideSmooth");
    form->addRow(smooth);
    // Only the chosen method's slider shows, with its label
    auto showMethod = [form, spacing, steps] (VEditSettings::SubdivideMethod m) {
        auto showRow = [form] (QWidget *field, bool visible) {
            field->setVisible(visible);
            if (QWidget *label = form->labelForField(field)) label->setVisible(visible);
        };
        showRow(spacing, m == VEditSettings::SubdivideMethod::Distance);
        showRow(steps, m == VEditSettings::SubdivideMethod::Steps);
    };
    showMethod(s->subdivideMethod);
    QObject::connect(method, QOverload<int>::of(&QComboBox::currentIndexChanged), parent, [s, changed, method, showMethod] (int) {
        s->subdivideMethod = VEditSettings::SubdivideMethod(method->currentData().toInt());
        showMethod(s->subdivideMethod);
        changed();
    });
    return page;
}
