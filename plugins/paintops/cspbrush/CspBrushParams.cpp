/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushParams.h"

#include <QByteArray>
#include <QStringList>
#include <utility>

namespace {
// Automatic spacing of CSP's Gap presets, as a fraction of the brush
// size. First guesses from sut2kpp; to be calibrated (plan, Session 1).
const qreal autoSpacing[] = {0.1, 0.25, 0.1, 0.05};

// Physical units are stored at CSP's fixed 10 px per mm (raw x 100)
qreal lengthPx(const CspBrushData &d, const QString &key, qreal defaultValue)
{
    const qreal raw = d.real(key, defaultValue);
    return d.integer(key + "Unit", 0) ? raw * 100.0 : raw;
}

bool isGrayImage(const QImage &image)
{
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const QRgb c = line[x];
            if (qAlpha(c) && (qRed(c) != qGreen(c) || qGreen(c) != qBlue(c))) return false;
        }
    }
    return true;
}
}

CspBrushParams CspBrushParams::fromData(const CspBrushData &d)
{
    CspBrushParams p;

    p.size = qMax(0.01, lengthPx(d, "BrushSize", 20.0));
    p.atLeast1Pixel = d.boolean("BrushAtLeast1Pixel");

    p.opacity = d.real("Opacity", 100) / 100.0;
    p.compositeMode = d.integer("CompositeMode");
    // 0 = Standard (Ver. 1), 1 = Perceptual (inferred; brushes without the column mix perceptually)
    p.mixingMode = d.integer("BrushColorMixingMode", 1) == 0
        ? CspColorMath::Mode::Standard : CspColorMath::Mode::Perceptual;

    p.useMaterial = d.boolean("BrushUsePatternImage");
    const int tipCount = d.integer("Tips/Count");
    for (int i = 0; i < tipCount; i++) {
        Tip tip;
        tip.name = d.string(QString("Tips/%1/Name").arg(i));
        const QByteArray png = QByteArray::fromBase64(d.string(QString("Tips/%1/Png").arg(i)).toLatin1());
        tip.image = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32);
        if (tip.image.isNull()) continue;
        tip.isGray = isGrayImage(tip.image);
        p.tips << tip;
    }
    p.hardness = qBound(0.01, d.real("BrushHardness", 100) / 100.0, 1.0);
    p.antiAlias = d.integer("AntiAlias", 2);
    p.thickness = qBound(0.01, d.real("BrushThickness", 100) / 100.0, 1.0);
    p.verticalThickness = d.boolean("BrushVerticalThicknes");
    p.angle = d.real("BrushRotation");
    p.angleInputs = d.integer("BrushRotationEffector") & 0xF0;
    p.angleRandom = d.real("BrushRotationRandomScale", 100) / 100.0;
    p.density = d.real("BrushFlow", 100) / 100.0;
    p.adjustDensityByGap = d.boolean("BrushAdjustFlowByInterval");
    p.flipHorizontal = d.integer("BrushPatternReverseHorizontal");
    p.flipVertical = d.integer("BrushPatternReverseVertical");
    p.swapMainSub = d.boolean("x-SwapMainSub");

    p.spacingType = d.integer("BrushAutoIntervalType", SpacingNormal);
    p.fixedSpacing = d.real("BrushInterval", 10.0) / 100.0;
    p.continuousSpray = d.boolean("BrushContinuousPlot");
    p.ribbon = d.boolean("BrushRibbon");
    p.useSpray = d.boolean("BrushUseSpray");
    p.spraySize = qMax(0.01, lengthPx(d, "BrushSpraySize", 2.0));
    p.spraySizeFollowsBrush = d.boolean("BrushSpraySizeSyncBrushSize");
    p.sprayDensity = qMax(1, d.integer("BrushSprayDensity", 1));
    p.sprayBias = qBound(-100.0, d.real("BrushSprayBias"), 100.0);
    p.sprayAngle = d.real("BrushRotationInSpray");
    p.sprayDirection = d.integer("BrushRotationEffectorInSpray") & 0x3F0;
    p.sprayRandom = d.real("BrushRotationRandomInSpray", 100) / 100.0;
    p.repeatMethod = d.integer("BrushPatternOrderType2", d.integer("BrushPatternOrderType"));
    p.blendTipsByDarken = d.boolean("BrushBlendPatternByDarken");

    p.textureDensity = d.real("TextureDensity", 50) / 100.0;
    p.textureScale = qMax(0.01, d.real("TextureScale2", d.real("TextureScale", 100)) / 100.0);
    p.textureRotation = d.real("TextureRotate");
    p.textureMode = qBound(0, d.integer("TextureCompositeMode"), int(TextureModeCount) - 1);
    p.textureInvert = d.boolean("TextureReverseDensity");
    p.textureEmphasize = d.boolean("TextureStressDensity");
    p.textureBrightness = d.real("TextureBrightness") / 100.0;
    p.textureContrast = d.real("TextureContrast") / 100.0;

    p.colorMixing = d.boolean("BrushUseWaterColor");
    p.mixingType = d.integer("BrushWaterColor");
    p.paintAmount = d.real("BrushMixColor", 100) / 100.0;
    p.paintDensity = d.real("BrushMixAlpha", 100) / 100.0;
    p.colorStretch = qBound(0.0, d.real("BrushMixColorExtension", 50) / 100.0, 1.0);
    p.mixBlurFollowsSize = d.boolean("BrushBlurLinkSize");
    p.mixBlur = p.mixBlurFollowsSize ? d.real("BrushBlur") / 100.0 : lengthPx(d, "BrushBlur", 0.0);

    p.jitterPerDab = d.boolean("BrushChangePatternColor");
    p.hueChange = d.real("BrushHueChange");
    p.saturationChange = d.real("BrushSaturationChange") / 100.0;
    p.valueChange = d.real("BrushValueChange") / 100.0;
    p.subColorChange = d.real("BrushSubColor") / 100.0;
    p.jitterPerStroke = d.boolean("BrushChangeStrokeColor");
    p.strokeHueChange = d.real("BrushStrokeHueChange");
    p.strokeSaturationChange = d.real("BrushStrokeSaturationChange") / 100.0;
    p.strokeValueChange = d.real("BrushStrokeValueChange") / 100.0;
    p.strokeSubColorChange = d.real("BrushStrokeSubColor") / 100.0;

    p.waterEdge = d.boolean("BrushUseWaterEdge");
    p.waterEdgeRadius = qMax(0.1, lengthPx(d, "BrushWaterEdgeRadius", 3.0));
    p.waterEdgeAlphaPower = qBound(0.0, d.real("BrushWaterEdgeAlphaPower", 20) / 100.0, 1.0);
    p.waterEdgeValuePower = qBound(0.0, d.real("BrushWaterEdgeValuePower", 0) / 100.0, 1.0);

    // the UI minimums; a knee of 0 makes the velocity input NaN
    p.velocityFast = qMax(0.1, d.real("x-VelocityFast", 2.0));
    p.velocityKnee = qMax(0.01, d.real("x-VelocityKnee", 0.3));

    const std::pair<CspEffector *, QString> effectors[] = {
        {&p.sizeDynamics, "BrushSize"},          {&p.opacityDynamics, "BrushOpacity"},
        {&p.thicknessDynamics, "BrushThickness"}, {&p.densityDynamics, "BrushFlow"},
        {&p.spacingDynamics, "BrushInterval"},    {&p.spraySizeDynamics, "BrushSpraySize"},
        {&p.sprayDensityDynamics, "BrushSprayDensity"},
        {&p.paintAmountDynamics, "BrushMixColor"}, {&p.paintDensityDynamics, "BrushMixAlpha"},
        {&p.textureDensityDynamics, "TextureDensity"},
        {&p.hueChangeDynamics, "BrushHueChange"}, {&p.saturationChangeDynamics, "BrushSaturationChange"},
        {&p.valueChangeDynamics, "BrushValueChange"}, {&p.subColorChangeDynamics, "BrushSubColor"}};
    for (const auto &[effector, key] : effectors) {
        *effector = CspEffector::fromString(d.string(key + "Effector"));
        effector->setVelocityCalibration(p.velocityFast, p.velocityKnee);
    }
    return p;
}

CspBrushParams CspBrushParams::fromSettings(const KisPropertiesConfiguration *settings)
{
    CspBrushData data;
    data.read(settings);
    return fromData(data);
}

qreal CspBrushParams::spacingFraction() const
{
    if (spacingType == SpacingFixed) return qMax(0.005, fixedSpacing);
    return autoSpacing[qBound(0, spacingType, 3)];
}
