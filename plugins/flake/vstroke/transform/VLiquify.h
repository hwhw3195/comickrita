/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VLIQUIFY_H
#define VLIQUIFY_H

#include <QHash>
#include <QVector>

#include <KisShapeLayerHooks.h>

#include "VCurveData.h"
#include "VCurveWarp.h"
#include "VMoveSession.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class VCurveShape;

/**
 * The transform tool's Liquify on a freehand vector layer: the Move and
 * Offset dabs move the points and handles of its curves themselves, so
 * the preview is the result. Shapes that aren't freehand curves stay.
 *
 * Each stroke of a layer has a VMoveSession, from its first command or
 * preview to its end (LiquifyPreviewDone). With the live preview
 * (VLiquify::preview), the curves move in the GUI thread as the dabs come,
 * and the stroke's commands only become the session's last one. Without
 * it, a command moves the curves by its dabs from where they were before
 * the stroke. When the stroke ends keeping its commands, its last command
 * holds the change: the curves go to its dabs, and its undo and redo swap
 * them from then on.
 */
class KRITAVSTROKE_EXPORT VLiquifyCommand : public VMoveCommand
{
public:
    /// \p stroke numbers the transform stroke (KisShapeLayerHooks::LiquifyCommand); a stroke's commands share its session anyway
    VLiquifyCommand(KisShapeLayer *layer, const QVector<KisShapeLayerHooks::LiquifyDab> &dabs, quint64 stroke);

    const QVector<VCurveWarp::Dab> &dabs() const { return m_dabs; }

protected:
    void move() override;

private:
    QVector<VCurveWarp::Dab> m_dabs;
};

namespace VLiquify
{
/// sets the Liquify hooks of KisShapeLayerHooks for freehand layers
KRITAVSTROKE_EXPORT void installHooks();

/**
 * The live preview (KisShapeLayerHooks::LiquifyPreview), in the GUI thread:
 * moves the curves of \p layer to where \p dabs, all the dabs so far, take
 * them from where they were when the stroke started. When \p dabs only adds
 * to the last list, only the new dabs are applied; otherwise (an in-tool
 * undo) the curves start over. \p fast: the transform tool's Fast preview
 * (brush strokes render when the moves pause).
 */
KRITAVSTROKE_EXPORT void preview(KisShapeLayer *layer, const QVector<KisShapeLayerHooks::LiquifyDab> &dabs, bool fast = false);
}

#endif // VLIQUIFY_H
