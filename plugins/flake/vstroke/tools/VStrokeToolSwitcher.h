/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_TOOL_SWITCHER_H
#define VSTROKE_TOOL_SWITCHER_H

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <kis_types.h>

#include "kritavstroke_export.h"

class KisView;
class KoCanvasBase;
class KoCanvasController;
class KoSelection;
class KoCanvasResourceProvider;
class KoToolManager;
class QWidget;

/**
 * One button for each pair of tools that do the same job on different
 * layer kinds; the toolbox finds no button for the plugin tool, so the
 * switcher checks the stock one (updateToolBoxButtons()).
 *
 * - The brush: with the brush tool active, selecting a freehand vector
 *   layer switches to the vector brush tool, and selecting any other layer
 *   switches back.
 * - Edit Shapes: on a freehand vector layer it switches to the point edit
 *   tool, except while only stock paths are selected (paths the layer keeps
 *   stock, such as dashed ones), which stock Edit Shapes edits.
 *
 * It also enables the Vector Edit button on freehand vector layers only,
 * and leaves that tool for what stock Krita gives elsewhere.
 */
class KRITAVSTROKE_EXPORT VStrokeToolSwitcher : public QObject
{
    Q_OBJECT
public:
    /**
     * Creates the switcher, once, owned by KisPart. Called when the vector
     * brush tool is created for a canvas: the plugin object is deleted right
     * after loading, and creating KisPart while plugins load would build the
     * resource servers before the resource database is up.
     */
    static void start();

    static const QString BrushToolId;
    static const QString VectorToolId;
    static const QString ShapeSelectToolId;
    static const QString EditShapesToolId;
    static const QString PointEditToolId;
    static const QString VectorEditToolId;

    /**
     * The tool to switch to from \p activeToolId with \p node current, or
     * empty to stay. \p stockPathsSelected: the selection holds stock paths
     * and no freehand curves.
     */
    static QString switchTarget(const QString &activeToolId, KisNodeSP node, bool stockPathsSelected = false);

    /**
     * The tool to judge the switch by. Clicking a vector layer with the brush
     * active makes Krita switch to the shape select tool first
     * (KisNodeManager::slotUiActivatedNode), so a brush-to-shape-select
     * change (\p brushReplaced) together with a layer change
     * (\p nodeChanged) counts as the brush.
     */
    static QString effectiveToolId(const QString &activeToolId, bool brushReplaced, bool nodeChanged);

    /// the stock tool whose toolbox button stands for \p toolId, or empty
    static QString standInToolId(const QString &toolId);

    /**
     * Checks the toolbox button for \p toolId in \p window, or the button of
     * its stand-in tool, and gives every toolbox button the highlight color
     * KoToolBox gives the checked one.
     */
    static void updateToolBoxButtons(QWidget *window, const QString &toolId);

private:
    explicit VStrokeToolSwitcher(QObject *parent);
    void slotViewAdded(KisView *view);
    void slotToolChanged(KoToolManager *manager, KoCanvasController *controller);
    void followSelection(KoCanvasBase *canvas);
    void scheduleCheck();
    void check();
    /// enables the Vector Edit buttons of the window of \p canvas
    void updateVectorEditButtons(KoCanvasBase *canvas, bool enabled);

private:
    QList<QPointer<KoToolManager>> m_managers; ///< one per window
    QPointer<KoToolManager> m_manager;         ///< of the last tool change
    QString m_lastToolId;                      ///< after the last tool change
    bool m_brushReplaced = false;              ///< brush to shape select since the last check
    bool m_nodeChanged = false;                ///< layer changed since the last check
    QPointer<KoCanvasResourceProvider> m_resources;
    QMetaObject::Connection m_resourcesConnection;
    QPointer<KoSelection> m_selection;
    QMetaObject::Connection m_selectionConnection;
    bool m_checkScheduled = false;
};

#endif // VSTROKE_TOOL_SWITCHER_H
