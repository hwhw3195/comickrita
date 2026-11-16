/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisShapeLayerHooks.h"

#include <kis_selection.h>
#include <kis_shape_layer.h>

#include <QAtomicInteger>

namespace {
KisShapeLayerHooks::ConvertShapes s_convertShapes;
KisShapeLayerHooks::NeedsNewLayer s_needsNewLayer;
KisShapeLayerHooks::InitNewLayer s_initNewLayer;
KisShapeLayerHooks::ClearCommand s_clearCommand;
KisShapeLayerHooks::CutWithinSelection s_cutWithinSelection;
KisShapeLayerHooks::SelectionMove s_selectionMove;
KisShapeLayerHooks::LayerIcon s_layerIcon;
KisShapeLayerHooks::KeepWithinSelection s_keepWithinSelection;
KisShapeLayerHooks::HasBrushSizes s_hasBrushSizes;
KisShapeLayerHooks::ScaleBrushSizes s_scaleBrushSizes;
KisShapeLayerHooks::IsLiquifyTarget s_isLiquifyTarget;
KisShapeLayerHooks::LiquifyCommand s_liquifyCommand;
KisShapeLayerHooks::LiquifyPreview s_liquifyPreview;
KisShapeLayerHooks::LiquifyPreviewDone s_liquifyPreviewDone;
KisShapeLayerHooks::IsDeformTarget s_isDeformTarget;
KisShapeLayerHooks::DeformCommand s_deformCommand;
KisShapeLayerHooks::DeformDone s_deformDone;
}

void KisShapeLayerHooks::setConvertShapes(ConvertShapes hook)
{
    s_convertShapes = hook;
}

void KisShapeLayerHooks::setNeedsNewLayer(NeedsNewLayer hook)
{
    s_needsNewLayer = hook;
}

void KisShapeLayerHooks::setInitNewLayer(InitNewLayer hook)
{
    s_initNewLayer = hook;
}

void KisShapeLayerHooks::setClearCommand(ClearCommand hook)
{
    s_clearCommand = hook;
}

void KisShapeLayerHooks::setLayerIcon(LayerIcon hook)
{
    s_layerIcon = hook;
}

void KisShapeLayerHooks::setKeepWithinSelection(KeepWithinSelection hook)
{
    s_keepWithinSelection = hook;
}

void KisShapeLayerHooks::convertShapes(QList<KoShape*> &shapes, KoShapeContainer *parent)
{
    if (s_convertShapes) s_convertShapes(shapes, parent);
}

bool KisShapeLayerHooks::needsNewLayer(const QList<KoShape*> &shapes, KisShapeLayer *activeLayer)
{
    return s_needsNewLayer && s_needsNewLayer(shapes, activeLayer);
}

void KisShapeLayerHooks::initNewLayer(KisShapeLayer *layer, const QList<KoShape*> &shapes)
{
    if (s_initNewLayer) s_initNewLayer(layer, shapes);
}

KUndo2Command *KisShapeLayerHooks::clearCommand(KisShapeLayer *layer, KisSelectionSP selection)
{
    return s_clearCommand ? s_clearCommand(layer, layer->shapeController(), selection) : nullptr;
}

QString KisShapeLayerHooks::layerIcon(const KisShapeLayer *layer)
{
    return s_layerIcon ? s_layerIcon(layer) : QString();
}

bool KisShapeLayerHooks::keepWithinSelection(KisShapeLayer *copy, KisSelectionSP selection)
{
    return s_keepWithinSelection && s_keepWithinSelection(copy, selection);
}

void KisShapeLayerHooks::setCutWithinSelection(CutWithinSelection hook)
{
    s_cutWithinSelection = hook;
}

KUndo2Command *KisShapeLayerHooks::cutWithinSelection(KisShapeLayer *layer, KisSelectionSP selection)
{
    return s_cutWithinSelection ? s_cutWithinSelection(layer, layer->shapeController(), selection) : nullptr;
}

void KisShapeLayerHooks::setSelectionMove(SelectionMove hook)
{
    s_selectionMove = hook;
}

void KisShapeLayerHooks::SelectionMoves::setSelection(KisSelectionSP selection)
{
    m_selection = selection ? new KisSelection(*selection) : nullptr;
}

KUndo2Command *KisShapeLayerHooks::SelectionMoves::command(KisShapeLayer *layer, const QTransform &transform, bool scaleBrushSizes)
{
    if (!m_selection || !s_selectionMove) return nullptr;
    QMutexLocker locker(&m_mutex);
    return s_selectionMove(layer, m_selection, transform, scaleBrushSizes, m_sessions[layer]);
}

void KisShapeLayerHooks::setBrushSizeHooks(HasBrushSizes hasBrushSizes, ScaleBrushSizes scaleBrushSizes)
{
    s_hasBrushSizes = hasBrushSizes;
    s_scaleBrushSizes = scaleBrushSizes;
}

bool KisShapeLayerHooks::hasBrushSizes(const KisNode *node)
{
    return s_hasBrushSizes && s_hasBrushSizes(node);
}

KUndo2Command *KisShapeLayerHooks::scaleBrushSizes(KisShapeLayer *layer, const QTransform &transform)
{
    return s_scaleBrushSizes ? s_scaleBrushSizes(layer, transform) : nullptr;
}

void KisShapeLayerHooks::setLiquifyHooks(IsLiquifyTarget isTarget, LiquifyCommand command)
{
    s_isLiquifyTarget = isTarget;
    s_liquifyCommand = command;
}

bool KisShapeLayerHooks::isLiquifyTarget(const KisNode *node)
{
    return node && s_isLiquifyTarget && s_liquifyCommand && s_isLiquifyTarget(node);
}

KUndo2Command *KisShapeLayerHooks::liquifyCommand(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, quint64 stroke)
{
    return s_liquifyCommand ? s_liquifyCommand(layer, dabs, stroke) : nullptr;
}

void KisShapeLayerHooks::setLiquifyPreview(LiquifyPreview preview, LiquifyPreviewDone done)
{
    s_liquifyPreview = preview;
    s_liquifyPreviewDone = done;
}

bool KisShapeLayerHooks::hasLiquifyPreview()
{
    return s_liquifyPreview && s_liquifyPreviewDone;
}

void KisShapeLayerHooks::liquifyPreview(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, bool fast)
{
    if (s_liquifyPreview) s_liquifyPreview(layer, dabs, fast);
}

void KisShapeLayerHooks::liquifyPreviewDone(KisShapeLayer *layer, bool keep)
{
    if (s_liquifyPreviewDone) s_liquifyPreviewDone(layer, keep);
}

quint64 KisShapeLayerHooks::newLiquifyStroke()
{
    static QAtomicInteger<quint64> s_lastStroke(0);
    return s_lastStroke.fetchAndAddRelaxed(1) + 1;
}

void KisShapeLayerHooks::setDeformHooks(IsDeformTarget isTarget, DeformCommand command, DeformDone done)
{
    s_isDeformTarget = isTarget;
    s_deformCommand = command;
    s_deformDone = done;
}

bool KisShapeLayerHooks::isDeformTarget(const KisNode *node)
{
    return node && s_isDeformTarget && s_deformCommand && s_isDeformTarget(node);
}

KUndo2Command *KisShapeLayerHooks::Deforms::command(KisShapeLayer *layer, const DeformMap &map)
{
    if (!s_deformCommand) return nullptr;
    QMutexLocker locker(&m_mutex);
    return s_deformCommand(layer, map, m_sessions[layer]);
}

void KisShapeLayerHooks::Deforms::finish(bool keep)
{
    QHash<KisShapeLayer*, std::shared_ptr<void>> sessions;
    {
        QMutexLocker locker(&m_mutex);
        sessions.swap(m_sessions);
    }
    if (!s_deformDone) return;
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        if (it.value()) s_deformDone(it.key(), it.value(), keep);
    }
}
