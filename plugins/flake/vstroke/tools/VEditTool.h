/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VEDIT_TOOL_H
#define VEDIT_TOOL_H

#include <QHash>
#include <QPointer>
#include <QPolygonF>

#include <functional>

#include <kundo2magicstring.h>

#include <KoToolBase.h>
#include <KoToolFactoryBase.h>

#include "VCurveData.h"
#include "VEditActions.h"
#include "VCurveTrim.h"
#include "VPointEdit.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KoToolManager;
class KisDoubleSliderSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QStackedWidget;
class VCurveShape;

/**
 * The Vector Edit tool: edits the curves of a freehand vector layer in
 * one of six modes, switched in the tool options or by shortcut.
 *
 * - Smooth (brush): Blender's smooth, of position, radius or pressure.
 * - Thickness (brush): Thicken, Thin, or Set to a radius (at full
 *   pressure); Ctrl swaps Thicken and Thin.
 * - Opacity/Density (brush): Increase, Decrease, or Set the per-point
 *   opacity, the density of brush strokes' dabs, or both; Ctrl swaps
 *   Increase and Decrease.
 * - Trim (drag, lasso or click): Blender's trim, cutting at crossings;
 *   or the whole strokes touched; or by point, only what is inside.
 * - Simplify (brush): fewer points, within a tolerance or a
 *   share at a time down to a line, and optionally joining strokes whose
 *   ends meet and merging overlapping strokes into one.
 * - Subdivide (brush): no segment longer than a spacing, or each segment
 *   halved a number of times, optionally smoothing as it goes.
 *
 * Each mode has its own brush size.
 *
 * The brushes change only the strokes under them, and only the part
 * inside. Each drag is one undo step. What takes one click on whole strokes
 * or selected points is in Edit Shapes (VPointEditTool).
 */
class KRITAVSTROKE_EXPORT VEditTool : public KoToolBase
{
    Q_OBJECT
public:
    enum class Mode { Smooth, Thickness, Trim, Simplify, Subdivide, Opacity };
    using ThicknessTool = VEditSettings::ThicknessTool;
    using OpacityTool = VEditSettings::OpacityTool;
    using SimplifyMethod = VEditSettings::SimplifyMethod;
    using Settings = VEditSettings;

    explicit VEditTool(KoCanvasBase *canvas);
    ~VEditTool() override;

    void paint(QPainter &painter, const KoViewConverter &converter) override;
    void mousePressEvent(KoPointerEvent *event) override;
    void mouseMoveEvent(KoPointerEvent *event) override;
    void mouseReleaseEvent(KoPointerEvent *event) override;
    QRectF decorationsRect() const override;

    void activate(const QSet<KoShape*> &shapes) override;
    void deactivate() override;

    QWidget *createOptionWidget() override;

    Mode mode() const;
    void setMode(Mode mode);
    /// the Thickness sub-tool a press with \p modifiers uses: Ctrl swaps Thicken and Thin
    ThicknessTool thicknessToolFor(Qt::KeyboardModifiers modifiers) const;
    /// the Opacity sub-tool a press with \p modifiers uses: Ctrl swaps Increase and Decrease
    OpacityTool opacityToolFor(Qt::KeyboardModifiers modifiers) const;
    /// the mode the next activation starts in (the mode shortcuts)
    void setNextActivationMode(Mode mode);

    /// the settings, public for the options and tests
    Settings &settings();

    /// the brush size of the current mode
    void setBrushSize(qreal size);
    /// the brush size of \p mode, which it gets back when it is chosen again
    qreal brushSize(Mode mode) const;
    /// one step up or down the brush size steps, as the brush size shortcuts
    void changeBrushSize(bool increase);
    /// the size one shortcut step up or down from \p size: brush size steps from 1 pt, tenths below, down to 0
    static qreal steppedBrushSize(qreal size, bool increase);

    static QString modeName(Mode mode);

private:
    KisShapeLayer *layer() const;
    /// the curves the brush works on
    QList<VCurveShape*> targets() const;
    void showMessage(const QString &message);

    void beginStroke(const QPointF &pos, Qt::KeyboardModifiers modifiers);
    void continueStroke(const QPointF &pos);
    void dab(const QPointF &pos);
    void finishStroke();
    void trimAt(const VCurveTrim::Selector &selector);

    /// curves of other visible freehand layers, in document coordinates, for Trim
    /// the curves on the other visible freehand layers, in document coordinates, their radii the ink radii in document units
    QVector<VCurveData> otherLayerCurves() const;

    void loadSettings();
    void saveSettings() const;

    Settings m_settings;
    VEditActions m_actions;
    Mode m_mode = Mode::Smooth;
    bool m_nextModeSet = false;
    Mode m_nextMode = Mode::Smooth;

    bool m_stroking = false;
    bool m_invert = false;     ///< Ctrl held at the press: the opposite sub-tool
    QPointF m_lastDab;
    QPolygonF m_path;          ///< the drag or lasso, document points
    QPointF m_cursor;
    bool m_cursorInside = false;
    QList<VCurveShape*> m_strokeTargets;
    /// one brush size per Mode, in the order of Mode; m_settings.size is the current one's
    qreal m_modeSizes[int(Mode::Opacity) + 1];
    /// Subdivide: each curve as the drag found it and the segments the drag has covered,
    /// so a segment is subdivided once per drag however many dabs touch it
    QHash<VCurveShape*, QPair<VCurveData, QVector<bool>>> m_subdivideBase;

    QMetaObject::Connection m_increaseSizeConnection;
    QMetaObject::Connection m_decreaseSizeConnection;
    QPointer<QComboBox> m_modeCombo;
    QPointer<QStackedWidget> m_pages;
    QPointer<KisDoubleSliderSpinBox> m_sizeSpinBox;
};

class KRITAVSTROKE_EXPORT VEditToolFactory : public KoToolFactoryBase
{
public:
    VEditToolFactory();
    KoToolBase *createTool(KoCanvasBase *canvas) override;
    QList<QAction *> createActionsImpl() override;

    /// the shortcut action of each mode
    static QString actionName(VEditTool::Mode mode);

private:
    void activateMode(KoToolManager *toolManager, VEditTool::Mode mode);
};

#endif // VEDIT_TOOL_H
