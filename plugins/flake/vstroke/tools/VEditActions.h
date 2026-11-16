/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VEDIT_ACTIONS_H
#define VEDIT_ACTIONS_H

#include <QHash>
#include <QList>
#include <QString>
#include <QVector>

#include <functional>

#include <kis_types.h>
#include <kundo2magicstring.h>

#include "VCurveData.h"
#include "VCurveTrim.h"
#include "VPointEdit.h"
#include "kritavstroke_export.h"

class KUndo2Command;
class KisDoubleSliderSpinBox;
class KisShapeLayer;
class KisSliderSpinBox;
class KoCanvasBase;
class KoShape;
class QWidget;
class VCurveShape;
class VStrokeData;

/// the settings of the Vector Edit brushes and of the Edit Shapes actions, each with their own
struct KRITAVSTROKE_EXPORT VEditSettings {
    /// the sub-tools of Thickness
    enum class ThicknessTool { Thicken, Thin, Set };
    /// the sub-tools of Opacity
    enum class OpacityTool { Increase, Decrease, Set };
    /// how Simplify picks the points to go
    enum class SimplifyMethod {
        Tolerance,   ///< all that may go within the tolerances
        Progressive  ///< a share per dab or Apply, no tolerance, down to a line (as Clip Studio)
    };

    qreal size = 40.0;              ///< brush diameter, document points
    qreal strength = 0.5;
    VPointEdit::Profile profile = VPointEdit::Profile::Smooth;
    bool selectedOnly = false;

    int smoothIterations = 1;       ///< how many times each dab or Apply smooths
    qreal smoothReach = 0.2;        ///< the brush: how far along the line Smooth averages, a share of the brush size
    qreal smoothDistance = 5.0;     ///< Edit Shapes' Apply: the same in document points
    bool smoothKeepShape = false;   ///< long curves hardly shrink (VCurveWarp::smoothAlong)
    bool smoothPosition = true;
    bool smoothRadius = false;
    bool smoothPressure = false;

    ThicknessTool thicknessTool = ThicknessTool::Thicken;
    qreal setRadius = 2.0;          ///< document points, for ThicknessTool::Set
    /// how much a dab of Thicken or Thin changes the radius: by Amount * strength * falloff (10% by default);
    /// Edit Shapes' Thicker multiplies by 1 + Amount and Thinner divides by it (25% there)
    qreal thicknessAmount = 0.1;

    /// Apply Current Brush: the strokes keep their brush size (widths stay), else take the preset's
    bool brushKeepSize = true;

    OpacityTool opacityTool = OpacityTool::Decrease;
    qreal setOpacity = 0.5;         ///< 0 to 1, for OpacityTool::Set
    qreal changeAmount = 0.1;       ///< 0 to 1: how far toward 100% or 0% a dab or click of Increase/Decrease goes
    bool affectOpacity = true;      ///< the line's opacity (a mask over it)
    bool affectDensity = false;     ///< brush strokes: the density of their dabs

    VCurveTrim::Selector::Mode trimMode = VCurveTrim::Selector::Drag;
    VCurveTrim::Options::Erase trimErase = VCurveTrim::Options::ToCrossings;
    bool keepCaps = false;
    bool keepFill = true;
    bool trimAllLayers = false;
    bool trimEndsOnly = false;
    qreal trimLeeway = 0.0;         ///< image pixels: a stroke ending this far short of another still cuts it

    SimplifyMethod simplifyMethod = SimplifyMethod::Tolerance;
    qreal simplifyTolerance = 1.0;  ///< screen pixels
    qreal attributeTolerance = 0.1; ///< share of the width, or of the 0 to 1 range of other values; 0: off
    bool joinEnds = false;
    qreal joinGap = 10.0;           ///< document points
    qreal joinAngle = 45.0;         ///< degrees
    qreal joinTransition = 0.1;
    bool mergeOverlapping = false;
    qreal mergeDistance = 6.0;      ///< document points
    qreal mergeFitError = 1.0;      ///< document points
    bool mergeAdjustWidth = false;
    qreal mergeRadius = 2.0;        ///< document points
    qreal mergeBias = 0.5;          ///< 0 to 1: how far the merged radius goes toward mergeRadius
    QString mergeTaper = QStringLiteral("0,1;1,1;"); ///< KisCubicCurve: the width along the line, start to end

    /// how Subdivide picks the new points
    enum class SubdivideMethod {
        Distance,  ///< no segment stays longer than the spacing
        Steps      ///< every segment is halved, a number of times
    };
    SubdivideMethod subdivideMethod = SubdivideMethod::Distance;
    qreal subdivideSpacing = 5.0;   ///< document points
    int subdivideSteps = 1;         ///< 1 to VCurveData::MaxSubdivideSteps
    bool subdivideSmooth = true;    ///< new points of lines on a Catmull-Rom spline

    /// Subdivide on \p data by these settings, on the segments flagged in \p mask (empty: all)
    VCurveData subdivide(const VCurveData &data, const QTransform &toDocument, const QVector<bool> &mask) const;

    /// \p group is the config group, with sub-groups for the modes
    void load(const QString &group);
    void save(const QString &group) const;
};

/**
 * What the Vector Edit brushes and the Edit Shapes actions share: changing
 * curves as one undo step, across shapes too, and the options of Smooth,
 * Simplify and Subdivide.
 */
class KRITAVSTROKE_EXPORT VEditActions
{
public:
    enum class Whole { Smooth, Simplify, Subdivide };

    VEditActions(KoCanvasBase *canvas, const VEditSettings *settings);

    /// the curves of \p layer: the selected ones, a group standing for the ones in it, or with \p selectedOnly false all of them
    QList<VCurveShape*> targets(KisShapeLayer *layer, bool selectedOnly) const;

    /// starts a step: forgets what touch() recorded
    void begin();
    /// records the curves of \p shape before the first change of this step
    void touch(VCurveShape *shape);
    /// \p before for each shape, the shapes to remove and the fills to drop, as one undo step
    void commit(const KUndo2MagicString &text, const QList<VCurveShape*> &removed = QList<VCurveShape*>(),
                const QList<VCurveShape*> &dropFill = QList<VCurveShape*>());

    /**
     * Runs \p op on the curves of \p shapes taken out as document strokes,
     * and writes back the shapes whose strokes changed; shapes left with
     * no strokes go to \p removed.
     */
    void acrossShapes(const QList<VCurveShape*> &shapes,
                      const std::function<QVector<VDocStroke>(const QVector<VDocStroke> &)> &op,
                      QList<VCurveShape*> *removed, QList<VCurveShape*> *changed = nullptr);
    /// whether strokes of the two shapes may become one
    static bool compatible(const VCurveShape *a, const VCurveShape *b);
    /// document units per radius unit
    static qreal radiusScale(const VCurveShape *shape);

    void runJoins(const QList<VCurveShape*> &shapes, const std::function<bool(const QPointF &)> &inArea,
                  QList<VCurveShape*> *removed);

    /**
     * Smooth, Simplify or Subdivide on \p shapes with one click, as one undo
     * step. \p pointWeights limits it to the points with a weight on each
     * shape; an empty list means all of them.
     */
    void applyWhole(Whole what, const QList<VCurveShape*> &shapes,
                    const std::function<QVector<qreal>(VCurveShape *)> &pointWeights);

    /// point \p i of \p data toward the radius \p target at full pressure, by \p amount
    static void moveRadiusToward(VCurveData &data, int i, qreal target, qreal amount);
    /**
     * \p data drawn with \p preset (already vector-safe: VStrokeTool::vectorPreset()) instead:
     * everything recorded stays; the preset's name, file and md5 are \p original's. With
     * \p keepSize the preset takes the stroke's brush size, else the stroke takes the preset's.
     */
    static VStrokeData withBrush(const VStrokeData &data, KisPaintOpPresetSP preset, KisPaintOpPresetSP original,
                                 bool keepSize);
    /**
     * The command drawing the brush strokes among \p shapes with \p preset (withBrush()), not yet
     * done; without keepSize their radii scale with the brush size. Null when there are none.
     */
    static KUndo2Command *brushCommand(const QList<VCurveShape*> &shapes, KisPaintOpPresetSP preset,
                                       KisPaintOpPresetSP original, bool keepSize, int *count = nullptr);
    /**
     * Apply Current Brush on \p shapes: the preset selected in \p canvas's window, as the vector
     * brush would draw with it. Shows how it went in the window. Returns how many changed.
     */
    static int applyCurrentBrush(KoCanvasBase *canvas, const QList<VCurveShape*> &shapes, bool keepSize);

    /// Thicken (radius times 1 + Amount * w) or Thin (times 1 - Amount * w), w = weight * \p strength
    static void changeThickness(VCurveData &data, const QVector<qreal> &weights, qreal strength, bool thicken,
                                qreal amount);
    /// the Opacity/Density change on the points of \p shape by weight * \p strength
    static void changeOpacity(VCurveData &data, VCurveShape *shape, const QVector<qreal> &weights, qreal strength,
                              VEditSettings::OpacityTool tool, const VEditSettings &settings);

    /**
     * The slider spin boxes of the vector tools, easy to drag with a
     * tablet. Dragging snaps to 0.01 for values up to 1, to 1 for whole
     * numbers and to 0.5 otherwise (with Ctrl, or without it when
     * "Sliders snap while dragging" is on); double-click to type a value.
     */
    static KisDoubleSliderSpinBox *doubleSlider(QWidget *parent, qreal min, qreal max, qreal value, int decimals,
                                                const QString &suffix, const QString &tip);
    static KisSliderSpinBox *intSlider(QWidget *parent, int min, int max, int value, const QString &suffix,
                                       const QString &tip);

    /// the option pages; \p changed runs after every change of \p settings
    /// \p brush: the Vector Edit brush's page (the distance as a share of the brush size), else Edit Shapes' (in points)
    static QWidget *smoothPage(QWidget *parent, VEditSettings *settings, const std::function<void()> &changed,
                               bool brush);
    /// Smooth on \p data (positions mapped by \p toDocument) by \p settings; \p documentDistance in document points
    static void smooth(VCurveData &data, const QTransform &toDocument, const QVector<qreal> &weights, qreal strength,
                       qreal documentDistance, const VEditSettings &settings);
    static QWidget *simplifyPage(QWidget *parent, VEditSettings *settings, const std::function<void()> &changed);
    static QWidget *subdividePage(QWidget *parent, VEditSettings *settings, const std::function<void()> &changed);

private:
    KoCanvasBase *m_canvas;
    const VEditSettings *m_settings;
    QHash<VCurveShape*, VCurveData> m_before;
};

#endif // VEDIT_ACTIONS_H
