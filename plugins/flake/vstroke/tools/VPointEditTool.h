/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VPOINT_EDIT_TOOL_H
#define VPOINT_EDIT_TOOL_H

#include <QHash>
#include <QPainterPath>
#include <QPointer>
#include <QScopedPointer>
#include <QTransform>

#include <functional>

#include <KisHandleStyle.h>

#include <kundo2magicstring.h>

#include <KoToolBase.h>
#include <KoToolFactoryBase.h>

#include "VCurveData.h"
#include "VEditActions.h"
#include "VPointEdit.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KisDoubleSliderSpinBox;
class QDoubleSpinBox;
class QMenu;
class VCurveShape;

/**
 * Edits the points of the selected brush strokes and curves on a freehand
 * vector layer. Points move as they are (nothing is resampled or
 * smoothed), with the points within the falloff radius following, as in
 * Blender's proportional editing; a circle shows the falloff radius.
 *
 * - Click a stroke to select it and show its points (Shift adds it). The
 *   click picks the stroke whose line is under the cursor, not the one
 *   whose bounding box is.
 * - Click a point to select it, Shift+click to add or remove it; drag on
 *   empty canvas to select the points in a rectangle, an ellipse or a
 *   lasso, or click the corners of a polygon (Shift adds).
 * - Drag a selected point to move the selection.
 * - Ctrl+drag a selected point up or down to make the line wider or
 *   thinner there (its radius).
 * - With exactly one point selected, its bezier handles show (as in Edit
 *   Shapes for paths); drag one to bend the segment. Smooth and symmetric
 *   points keep the other handle in line.
 * - Right-click for the point types of the selected points: Corner,
 *   Smooth, Symmetric, Make curve point (line segments become beziers, so
 *   they get handles) and Make line point.
 * - The brush size shortcuts (Increase / Decrease Brush Size, [ and ] by
 *   default) change the falloff radius. It's a tool setting, not part of
 *   any undo step.
 *
 * Each drag is one undo step.
 *
 * The panel also has what takes one click and no brush: line width, Smooth,
 * Simplify, Subdivide and opacity, on the selected points (with the falloff),
 * or with none the selected strokes, or the whole layer. Each is one undo step.
 */
class KRITAVSTROKE_EXPORT VPointEditTool : public KoToolBase
{
    Q_OBJECT
public:
    explicit VPointEditTool(KoCanvasBase *canvas);
    ~VPointEditTool() override;

    void paint(QPainter &painter, const KoViewConverter &converter) override;
    void mousePressEvent(KoPointerEvent *event) override;
    void mouseMoveEvent(KoPointerEvent *event) override;
    void mouseReleaseEvent(KoPointerEvent *event) override;
    void mouseDoubleClickEvent(KoPointerEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QMenu *popupActionsMenu() override;
    QRectF decorationsRect() const override;

    void activate(const QSet<KoShape*> &shapes) override;
    void deactivate() override;

    /**
     * Copy, Cut and Delete: with points selected, the stretches between them
     * (VPointEdit::splitSelected), each copied as a stroke of its own that a
     * paste adds to the layer. With no point selected, the selected strokes
     * whole, as Select Shapes does.
     */
    bool hasSelection() override;
    void copy() const override;
    void deleteSelection() override;
    void deselect() override;

    QWidget *createOptionWidget() override;

    /// in document points; 0 moves the selected points only
    qreal falloffRadius() const;
    void setFalloffRadius(qreal radius);
    /// one step up or down the brush size steps, as the brush size shortcuts
    void changeFalloffRadius(bool increase);

    VPointEdit::Profile falloffProfile() const;
    void setFalloffProfile(VPointEdit::Profile profile);

    /// how a drag on empty canvas selects points
    enum class SelectionShape { Rectangle, Ellipse, Lasso, Polygon };
    SelectionShape selectionShape() const;
    void setSelectionShape(SelectionShape shape);

    /// the selected points of \p shape (for tests)
    QVector<int> selectedPoints(VCurveShape *shape) const;
    /// selects point \p index of \p shape alone (for tests)
    void selectPoint(VCurveShape *shape, int index);
    /// adds the points \p indices of \p shape to the selection (for tests)
    void selectPoints(VCurveShape *shape, const QVector<int> &indices);
    /// the one selected point, if exactly one is selected: its handles show where its points are sparse
    bool singleSelectedPoint(VCurveShape **shape, int *index) const;

    /// on screen, in pixels: the single selected point's handles show where its points are this far apart
    static const qreal MinHandleSpacing;
    /**
     * Whether the handles of point \p index show, and can be grabbed: where its
     * points are at least \p minSpacing apart through \p toView (a dense brush
     * stroke's handles would only clutter the line).
     */
    static bool showsHandles(const VCurveData &curves, int index, const QTransform &toView, qreal minSpacing = MinHandleSpacing);
    /**
     * Draws the points of \p shape as paint() does: \p selected tells the
     * selected ones, and point \p singleIndex (-1: none) gets its handles
     * where showsHandles() says so.
     */
    static void paintPoints(QPainter &painter, const KoViewConverter &converter, VCurveShape *shape,
                            const std::function<bool(int)> &selected, int singleIndex,
                            qreal handleRadius, qreal decorationThickness,
                            qreal minHandleSpacing = MinHandleSpacing);
    /// the point type of the context menu on every selected point, one undo step
    void setSelectedPointType(VPointEdit::PointType type);

    /// scales the radius of what the actions work on, one undo step
    void scaleSelectedRadii(qreal factor);

    /// The actions take one click and work on the selected points, or with none
    /// the selected strokes, or the whole layer. The panel shows one at a time
    enum class Action { LineWidth, Smooth, Simplify, Subdivide, Opacity, Brush };
    Action currentAction() const;
    void setCurrentAction(Action action);
    /// false: what is selected; true: every stroke on the layer
    bool actsOnWholeLayer() const;
    void setActsOnWholeLayer(bool wholeLayer);
    /// the settings of the actions, public for the options and tests
    VEditSettings &actionSettings();

    /// Apply Current Brush on the brush strokes the actions work on (VEditActions::applyCurrentBrush())
    void applyBrushToSelected();
    /// the radius of what the actions work on to VEditSettings::setRadius
    void setSelectedRadii();
    /// Smooth, Simplify or Subdivide on what the actions work on
    void applyToSelected(VEditActions::Whole what);
    /// Opacity/Density on what the actions work on
    void changeSelectedOpacity(VEditSettings::OpacityTool tool);

private:
    struct Hit {
        VCurveShape *shape = nullptr;
        int index = -1;
    };

    struct Edit {
        VCurveShape *shape;
        VCurveData before;
        VPointEdit::Weights weights;
        QTransform documentToLocal;
    };

    /// Area: a rectangle, ellipse or lasso being dragged; Polygon: corners
    /// being clicked, across presses
    enum class Drag { None, Move, Radius, Area, Polygon, Handle };

    KisShapeLayer *layer() const;
    /// the selected curves on the current freehand layer
    QList<VCurveShape*> targets() const;
    /// where the falloff circle goes, in document points; null: none
    QPointF falloffCenter() const;
    Hit hitAt(const QPointF &documentPos) const;
    /// the topmost selectable shape whose outline is under \p documentPos
    KoShape *shapeUnder(const QPointF &documentPos) const;
    /// the area selecting points, in document points; \p preview adds the
    /// cursor to an unfinished polygon
    QPainterPath selectionArea(bool preview) const;
    void selectPointsIn(const QPainterPath &area);
    void finishPolygon();
    void cancelPolygon();
    QVector<bool> &selection(VCurveShape *shape);
    /// the selected points of the shapes edited now, skipping stale selections
    QList<QPair<VCurveShape*, QVector<bool>>> selectedPointMasks() const;
    bool isSelected(VCurveShape *shape, int index) const;
    void dropStaleSelections();
    QVector<Edit> beginEdit() const;
    void applyEdit(const QPointF &documentDelta);
    void finishEdit(const KUndo2MagicString &text);
    void showMessage(const QString &message);
    /// what the actions work on: the strokes, and how far each of their points goes
    QVector<Edit> actionTargets();
    void loadActionSettings();
    void saveActionSettings() const;
    QWidget *createActionsWidget(QWidget *parent);

    VEditSettings m_actionSettings;
    VEditActions m_actions;
    Action m_action = Action::LineWidth;
    bool m_wholeLayer = false;

    QHash<VCurveShape*, QVector<bool>> m_selected;
    QVector<Edit> m_edits;
    Drag m_drag = Drag::None;
    QPointF m_dragStart;
    QPointF m_dragNow;
    /// the lasso path or the polygon corners, in document points
    QPolygonF m_areaPoints;
    SelectionShape m_selectionShape = SelectionShape::Rectangle;
    /// the grabbed point at the start of a drag, in document points
    QPointF m_grabbed;
    /// the point under the cursor while no drag happens, in document points
    QPointF m_hovered;
    bool m_hovering = false;
    /// the point under the cursor, for the context menu
    Hit m_hoveredHit;
    /// Drag::Handle: which handle of the single selected point
    VPointEdit::HandleSide m_handleSide = VPointEdit::HandleSide::Out;
    int m_handleIndex = -1;
    QScopedPointer<QMenu> m_contextMenu;
    QMetaObject::Connection m_selectionConnection;
    QMetaObject::Connection m_layerSelectionConnection;
    QMetaObject::Connection m_increaseSizeConnection;
    QMetaObject::Connection m_decreaseSizeConnection;
    QPointer<KisDoubleSliderSpinBox> m_falloffSpinBox;
    bool m_addToSelection = false;
    qreal m_falloffRadius = 0.0;
    VPointEdit::Profile m_falloffProfile = VPointEdit::Profile::Smooth;
};

class KRITAVSTROKE_EXPORT VPointEditToolFactory : public KoToolFactoryBase
{
public:
    VPointEditToolFactory();
    KoToolBase *createTool(KoCanvasBase *canvas) override;
    QList<QAction *> createActionsImpl() override;
};

#endif // VPOINT_EDIT_TOOL_H
