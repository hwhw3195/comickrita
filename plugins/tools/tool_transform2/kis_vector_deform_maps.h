/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VECTOR_DEFORM_MAPS_H
#define KIS_VECTOR_DEFORM_MAPS_H

#include <QPointF>
#include <QVector>

#include <KisBezierTransformMesh.h>
#include <KisShapeLayerHooks.h>
#include <kis_warptransform_worker.h>

/**
 * Where Warp, Cage and Mesh send each point of the image, for the vector
 * layers that move their shapes themselves (KisShapeLayerHooks::DeformMap).
 * Each takes what the stock worker of the mode takes and sends a point
 * where that worker sends its pixel, exactly where the worker is exact
 * (Warp, Cage) and as its grid's samples do for Mesh.
 */
namespace KisVectorDeformMaps
{
/// KisWarpTransformWorker's map: none moves with no points, one point translates
KisShapeLayerHooks::DeformMap warp(KisWarpTransformWorker::WarpType type, const QVector<QPointF> &origPoints,
                                   const QVector<QPointF> &transfPoints, qreal alpha);

/**
 * KisBezierTransformMesh's map: a point goes where the patch over it takes it,
 * placed in the patch as KisBezierPatch::sampleRegularGrid() places its samples
 * (by the transformed edges' lengths). Points beyond the mesh go by its edge
 * patches, continued.
 */
KisShapeLayerHooks::DeformMap mesh(const KisBezierTransformMesh &mesh);

/**
 * KisCageTransformWorker's map, by Green coordinates: only what is inside
 * \p origPoints moves (DeformMap::inside). Points just outside, which the
 * derivative's probes reach at the cut, follow the nearest cage edge. With
 * \p editing (the cage's points are being placed) or fewer than three
 * points, it moves nothing.
 */
KisShapeLayerHooks::DeformMap cage(const QVector<QPointF> &origPoints, const QVector<QPointF> &transfPoints, bool editing);
}

#endif // KIS_VECTOR_DEFORM_MAPS_H
