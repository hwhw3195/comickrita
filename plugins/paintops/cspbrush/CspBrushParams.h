/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_PARAMS_H
#define CSP_BRUSH_PARAMS_H

#include <QImage>
#include <QMap>
#include <QVector>

#include "CspBrushData.h"
#include "CspColorMath.h"
#include "CspEffector.h"

class KisPropertiesConfiguration;

/**
 * Typed view of CspBrushData for the paintop. Units are converted:
 * lengths in px, percentages as fractions, angles in degrees (CCW).
 */
struct KRITACSPBRUSHPAINTOP_EXPORT CspBrushParams
{
    // CSP enumerations, values as stored in the .sut file
    enum SpacingType { SpacingFixed = 0, SpacingWide = 1, SpacingNormal = 2, SpacingNarrow = 3 };
    enum RepeatMethod { Repeat = 0, Reverse = 1, NoRepeat = 2, RandomOrder = 3, OneTimeOnly = 4, OneRandomCycle = 5 };
    enum AngleBits { AnglePenDirection = 0x10, AnglePenRotation = 0x20, AngleLineDirection = 0x40, AngleRandom = 0x80 };

    struct Tip {
        QString name;
        QImage image;       ///< ARGB32; gray tips have r == g == b
        bool isGray {true};
    };

    // Brush size
    qreal size {20.0};
    CspEffector sizeDynamics;
    bool atLeast1Pixel {false};

    // Ink
    qreal opacity {1.0};
    CspEffector opacityDynamics;
    int compositeMode {0};
    CspColorMath::Mode mixingMode {CspColorMath::Mode::Perceptual};

    // Brush tip
    bool useMaterial {false};
    QVector<Tip> tips;
    qreal hardness {1.0};
    int antiAlias {2};
    qreal thickness {1.0};
    CspEffector thicknessDynamics;
    bool verticalThickness {false};
    qreal angle {0.0};
    int angleInputs {0};
    qreal angleRandom {1.0};   ///< fraction of +-180 deg
    qreal density {1.0};
    CspEffector densityDynamics;
    bool adjustDensityByGap {false};
    int flipHorizontal {0};
    int flipVertical {0};
    bool swapMainSub {false}; ///< engine option: white -> main, black -> sub

    // Stroke
    int spacingType {SpacingNormal};
    qreal fixedSpacing {0.1}; ///< fraction of brush size
    CspEffector spacingDynamics;
    bool continuousSpray {false};
    bool ribbon {false};
    bool useSpray {false};
    int repeatMethod {Repeat};
    bool blendTipsByDarken {false};

    // Spraying effect
    enum SprayDirectionBits { SprayLineDirection = 0x40, SprayRandom = 0x80, SprayWhole = 0x100, SprayTowardCenter = 0x200,
                              SprayDirectionMask = SprayLineDirection | SprayWhole | SprayTowardCenter };
    qreal spraySize {2.0};       ///< particle size, px
    CspEffector spraySizeDynamics;
    bool spraySizeFollowsBrush {false};
    int sprayDensity {1};        ///< particles per plot
    CspEffector sprayDensityDynamics;
    qreal sprayBias {0.0};       ///< -100..100, higher = closer to the centre
    qreal sprayAngle {0.0};      ///< degrees
    int sprayDirection {0};
    qreal sprayRandom {1.0};     ///< fraction of +-180 deg

    // Texture: CSP's settings; the image is a Krita pattern (Texture/Pattern/..., CspTextureStage::loadPattern)
    enum TextureMode { TexNormal = 0, TexMultiply, TexSubtract, TexCompare, TexOutline, TexOverlay,
                       TexColorDodge, TexHardMix, TexHeight, TextureModeCount };
    qreal textureDensity {0.5};
    CspEffector textureDensityDynamics;
    qreal textureScale {1.0};
    qreal textureRotation {0.0}; ///< degrees
    int textureMode {TexNormal};
    bool textureInvert {false};
    bool textureEmphasize {false};
    qreal textureBrightness {0.0}; ///< -1..1
    qreal textureContrast {0.0};   ///< -1..1

    // Color mixing ("Ink > Color mixing")
    enum MixingType { MixBlend = 0, MixRunningColor = 1, MixSmear = 2 };
    bool colorMixing {false};
    int mixingType {MixBlend};
    qreal paintAmount {1.0};           ///< "Amount of paint"
    CspEffector paintAmountDynamics;
    qreal paintDensity {1.0};          ///< "Density of paint"
    CspEffector paintDensityDynamics;
    qreal colorStretch {0.5};          ///< "Color stretch"
    qreal mixBlur {0.0};               ///< px, or a fraction of the brush size when linked
    bool mixBlurFollowsSize {false};

    // Color jitter ("Change brush tip color" per dab, "Change color for each stroke")
    bool jitterPerDab {false};
    qreal hueChange {0.0};        ///< degrees
    CspEffector hueChangeDynamics;
    qreal saturationChange {0.0}; ///< fraction
    CspEffector saturationChangeDynamics;
    qreal valueChange {0.0};      ///< fraction
    CspEffector valueChangeDynamics;
    qreal subColorChange {0.0};   ///< share of the sub color
    CspEffector subColorChangeDynamics;
    bool jitterPerStroke {false};
    qreal strokeHueChange {0.0};
    qreal strokeSaturationChange {0.0};
    qreal strokeValueChange {0.0};
    qreal strokeSubColorChange {0.0};

    // Watercolor edge
    bool waterEdge {false};
    qreal waterEdgeRadius {3.0};      ///< px
    qreal waterEdgeAlphaPower {0.2};  ///< "Opacity effect", 0..1
    qreal waterEdgeValuePower {0.0};  ///< "Darkness effect", 0..1

    // Velocity calibration (px/ms), see dynamics.py of the sut2kpp converter (a separate Krita Python plugin)
    qreal velocityFast {2.0};
    qreal velocityKnee {0.3};

    static CspBrushParams fromData(const CspBrushData &data);
    static CspBrushParams fromSettings(const KisPropertiesConfiguration *settings);

    /// CSP draws a ribbon only with material tips and without spray
    bool ribbonActive() const { return ribbon && useMaterial && !tips.isEmpty() && !useSpray; }
    /// A ribbon doesn't mix: it stamps like a brush without mixing
    bool mixingActive() const { return colorMixing && !ribbonActive(); }

    /// Spacing as a fraction of the brush size
    qreal spacingFraction() const;
};

#endif // CSP_BRUSH_PARAMS_H
