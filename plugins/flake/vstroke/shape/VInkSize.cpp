/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VInkSize.h"

#include <QStringList>

#include <cmath>
#include <memory>

#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_cubic_curve.h>
#include <kis_paint_information.h>
#include <kis_properties_configuration.h>

#include <KisCurveOption.h>
#include <KisKritaSensorPack.h>
#include <KisPaintOpOptionUtils.h>
#include <KisSizeOptionData.h>

#include "VCurveShape.h"
#include "VStrokeCurves.h"
#include "VStrokePresetStore.h"
#include "VStrokeShape.h"

namespace {

/// one input of a CSP effect source: lerp(minimum, 1, curve(x)); as CspEffector, which lives in the brush's plugin
struct CspInput {
    bool enabled = false;
    qreal minimum = 0.0;
    QVector<qreal> transfer; ///< empty: linear

    qreal value(qreal x) const
    {
        x = qBound(0.0, x, 1.0);
        const qreal y = transfer.isEmpty() ? x : KisCubicCurve::interpolateLinear(x, transfer);
        return minimum + (1.0 - minimum) * qBound(0.0, y, 1.0);
    }
};

/// the pressure ("p") and tilt ("t") inputs of a CSP effector string ("on=p;p=0.62@0,0.07 0.2,0.4 1,1;t=0")
void parseCspEffector(const QString &text, CspInput *pressure, CspInput *tilt)
{
    for (const QString &part : text.split(';', Qt::SkipEmptyParts)) {
        const int eq = part.indexOf('=');
        if (eq < 0) continue;
        const QString key = part.left(eq).trimmed();
        const QString value = part.mid(eq + 1).trimmed();
        if (key == "on") {
            for (const QString &name : value.split(',', Qt::SkipEmptyParts)) {
                if (name.trimmed() == "p") pressure->enabled = true;
                if (name.trimmed() == "t") tilt->enabled = true;
            }
            continue;
        }
        CspInput *input = key == "p" ? pressure : key == "t" ? tilt : nullptr;
        if (!input) continue;
        const int at = value.indexOf('@');
        input->minimum = (at < 0 ? value : value.left(at)).toDouble();
        if (at < 0) continue;
        QList<KisCubicCurvePoint> points;
        for (const QString &pt : value.mid(at + 1).split(' ', Qt::SkipEmptyParts)) {
            const QStringList xy = pt.split(',');
            if (xy.size() >= 2) points << KisCubicCurvePoint(xy[0].toDouble(), xy[1].toDouble(), xy.size() > 2 && xy[2] == "c");
        }
        std::stable_sort(points.begin(), points.end(),
                         [] (const KisCubicCurvePoint &a, const KisCubicCurvePoint &b) { return a.x() < b.x(); });
        if (!points.isEmpty() && !KisCubicCurve(points).isIdentity()) input->transfer = KisCubicCurve(points).floatTransfer(256);
    }
}

}

VInkSize::Response VInkSize::response(const QString &paintopId, const KisPropertiesConfiguration *settings)
{
    if (!settings) return Response();
    if (paintopId == "cspbrush") {
        // KisCspBrushPaintOp::geometry(): size × BrushSizeEffector, at least a pixel when asked
        CspInput pressure;
        CspInput tilt;
        parseCspEffector(settings->getString("CSP/BrushSizeEffector"), &pressure, &tilt);
        return [pressure, tilt] (const KisPaintInformation &pi) {
            qreal value = 1.0;
            if (pressure.enabled) value *= pressure.value(pi.pressure());
            // CspEffector::input(): 0 upright, 1 flat
            if (tilt.enabled) value *= tilt.value(1.0 - KisPaintInformation::tiltElevation(pi, 60.0, 60.0, true));
            return value;
        };
    }
    if (paintopId == "paintbrush") {
        // KisBrushOp's KisSizeOption, with only the pen's own sensors
        KisSizeOptionData data = KisPaintOpOptionUtils::loadOptionData<KisSizeOptionData>(settings);
        KisKritaSensorData &s = data.sensorStruct();
        for (KisSensorData *off : {&s.sensorSpeed, static_cast<KisSensorData *>(&s.sensorDrawingAngle),
                                   static_cast<KisSensorData *>(&s.sensorDistance), static_cast<KisSensorData *>(&s.sensorTime),
                                   &s.sensorFuzzyPerDab, &s.sensorFuzzyPerStroke, static_cast<KisSensorData *>(&s.sensorFade),
                                   &s.sensorPerspective, &s.sensorTangentialPressure}) {
            off->isActive = false;
        }
        std::shared_ptr<KisCurveOption> option = std::make_shared<KisCurveOption>(data);
        return [option] (const KisPaintInformation &pi) {
            return option->isChecked() ? option->computeSizeLikeValue(pi) : 1.0;
        };
    }
    return Response();
}

QVector<qreal> VInkSize::radii(const VCurveData &curves, qreal brushSize, const Response &response)
{
    if (!response) return curves.radii;
    QVector<qreal> result(curves.pointCount());
    for (int i = 0; i < curves.pointCount(); i++) {
        const KisPaintInformation pi = VStrokeCurves::paintInfo(curves, i, brushSize);
        result[i] = 0.5 * brushSize * qMax(0.0, response(pi)) * pi.sizeMultiplier();
    }
    return result;
}

QVector<qreal> VInkSize::Cache::documentRadii(const VCurveShape *shape, int subPath)
{
    return documentRadii(shape, shape->curves().curve(subPath));
}

QVector<qreal> VInkSize::Cache::documentRadii(const VCurveShape *shape, const VCurveData &curves)
{
    const qreal scale = VInkSize::radiusScale(shape);
    QVector<qreal> result = curves.radii;
    if (const VStrokeShape *stroke = dynamic_cast<const VStrokeShape *>(shape)) {
        const VStrokeData &d = stroke->data();
        const KisPaintOpPresetSP preset = d.preset ? d.preset : d.presetDefinition ? d.presetDefinition->preset : KisPaintOpPresetSP();
        if (preset) {
            auto it = m_responses.find(preset.data());
            if (it == m_responses.end()) {
                it = m_responses.insert(preset.data(), response(preset->paintOp().id(), preset->settings().data()));
            }
            result = radii(curves, d.brushSize, *it);
        }
    }
    for (qreal &r : result) r *= scale;
    return result;
}

qreal VInkSize::radiusScale(const VCurveShape *shape)
{
    if (const VStrokeShape *stroke = dynamic_cast<const VStrokeShape*>(shape)) {
        // brush strokes keep their radius in image pixels
        return stroke->resolution() > 0 ? 1.0 / stroke->resolution() : 1.0;
    }
    return std::sqrt(std::abs(shape->absoluteTransformation().determinant()));
}
