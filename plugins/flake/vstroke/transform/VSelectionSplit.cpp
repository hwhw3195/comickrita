/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VSelectionSplit.h"

#include <QApplication>
#include <QThread>
#include <QtMath>

#include <memory>

#include <KoShapeContainer.h>
#include <KoShapeGroup.h>
#include <commands/KoShapeDeleteCommand.h>
#include <kundo2command.h>

#include <KisSafeBlockingQueueConnectionProxy.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_random_accessor_ng.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>

#include "VCurveShape.h"
#include "VCurveTrim.h"
#include "VGuiThreadCommand.h"
#include "VFreehandLayer.h"
#include "VStrokeShape.h"
#include "VSelectionCover.h"

namespace {

using InsideTest = std::function<bool(const QPointF &)>;

/// the test whether a point of \p shape is selected, and the distance of a pixel in its units; a null test when it can't tell
InsideTest selectionTest(const KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels, qreal *step)
{
    const QTransform toPixels = shape->absoluteTransformation() * documentToPixels;
    const qreal pixelsPerUnit = qSqrt(qAbs(toPixels.determinant()));
    if (pixelsPerUnit <= 0) return InsideTest();
    *step = 1.0 / pixelsPerUnit;
    KisRandomConstAccessorSP pixels = selection->projection()->createRandomConstAccessorNG();
    return [toPixels, pixels] (const QPointF &p) mutable {
        const QPoint at = toPixels.map(p).toPoint();
        pixels->moveTo(at.x(), at.y());
        return *pixels->oldRawData() > 127;
    };
}

/// what is cut of one curve shape
struct Part {
    VCurveShape *shape = nullptr;
    QTransform transform;
    VCurveData before;
    VCurveData inside;
    VCurveData outside;
    VCurveShape *piece = nullptr; ///< a line crossing the edge: holds \p inside while it moves, and \p shape the rest
};

/// what a selection holds of a layer
struct Split {
    ~Split()
    {
        if (applied) return; // the layer has the pieces
        for (const Part &part : parts) delete part.piece;
    }

    void read(const QList<KoShape*> &shapes, KisSelectionSP selection, const QTransform &documentToPixels)
    {
        for (KoShape *shape : shapes) {
            if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
                read(group->shapes(), selection, documentToPixels);
            } else if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) {
                readCurve(curve, selection, documentToPixels);
            } else if (VSelectionCover::coveredBySelection(shape, selection, documentToPixels)) {
                others << qMakePair(shape, shape->transformation());
            }
        }
    }

    void readCurve(VCurveShape *shape, KisSelectionSP selection, const QTransform &documentToPixels)
    {
        qreal step = 1.0;
        const InsideTest inside = selectionTest(shape, selection, documentToPixels, &step);
        if (!inside) return;
        const InsideTest outside = [&inside] (const QPointF &p) { return !inside(p); };

        Part part;
        part.shape = shape;
        part.transform = shape->transformation();
        part.before = shape->curves();
        for (int c = 0; c < part.before.curveCount(); c++) {
            const VCurveData curve = part.before.curve(c);
            const QVector<VCurveData> in = VCurveTrim::keepInside(curve, inside, step);
            const bool allIn = in.size() == 1 && in.first() == curve;
            const QVector<VCurveData> out = allIn ? QVector<VCurveData>() : VCurveTrim::keepInside(curve, outside, step);
            for (const VCurveData &piece : in) part.inside.append(piece);
            for (const VCurveData &piece : out) part.outside.append(piece);
            // both sides of a line too short for its width would count as slivers: it goes where it starts
            if (in.isEmpty() && out.isEmpty() && curve.pointCount() > 0) {
                (inside(curve.positions.first()) ? part.inside : part.outside).append(curve);
            }
        }
        if (!part.inside.isEmpty()) parts << part;
    }

    QVector<Part> parts;
    QVector<QPair<KoShape*, QTransform>> others;
    bool read_ = false;
    bool applied = false;
};

class CutCommand : public VGuiThreadCommand
{
public:
    CutCommand(KisShapeLayer *layer, KoShapeControllerBase *controller, std::shared_ptr<Split> split)
        : m_layer(layer),
          m_split(split)
    {
        QList<KoShape*> gone;
        for (const Part &part : m_split->parts) {
            if (part.outside.isEmpty()) gone << part.shape;
        }
        for (const auto &other : m_split->others) gone << other.first;
        if (!gone.isEmpty()) m_delete.reset(new KoShapeDeleteCommand(controller, gone));
    }

protected:
    void apply(bool redo) override
    {
        for (const Part &part : m_split->parts) {
            if (!part.outside.isEmpty()) part.shape->setCurvesAndUpdate(redo ? part.outside : part.before);
        }
        if (m_delete) {
            if (redo) {
                m_delete->redo();
            } else {
                m_delete->undo();
            }
        }
    }

private:
    KisShapeLayerSP m_layer;
    std::shared_ptr<Split> m_split;
    std::unique_ptr<KoShapeDeleteCommand> m_delete;
};

void setTransformationAndUpdate(KoShape *shape, const QTransform &transformation)
{
    const QRectF before = shape->boundingRect();
    shape->setTransformation(transformation);
    shape->updateAbsolute(before);
    shape->update();
}

class MoveCommand : public VGuiThreadCommand
{
public:
    MoveCommand(KisShapeLayer *layer, KisSelectionSP selection, std::shared_ptr<Split> split,
                const QTransform &documentTransform, qreal brushScale)
        : m_layer(layer),
          m_selection(selection),
          m_split(split),
          m_transform(documentTransform),
          m_brushScale(brushScale)
    {
    }

protected:
    void apply(bool redo) override
    {
        Split &split = *m_split;
        if (!split.read_) {
            KisImageSP image = m_layer->image();
            if (image) split.read(m_layer->shapes(), m_selection, QTransform::fromScale(image->xRes(), image->yRes()));
            for (Part &part : split.parts) {
                if (part.outside.isEmpty()) continue;
                part.piece = dynamic_cast<VCurveShape*>(part.shape->cloneShape());
                KIS_SAFE_ASSERT_RECOVER_RETURN(part.piece);
                part.piece->setCurves(part.inside);
            }
            split.read_ = true;
        }

        if (redo) {
            for (const Part &part : split.parts) {
                KoShapeContainer *parent = part.shape->parent();
                VCurveShape *moving = part.shape;
                if (part.piece) {
                    part.shape->setCurvesAndUpdate(part.outside);
                    parent->addShape(part.piece);
                    moving = part.piece;
                }
                VCurveData curves = part.piece ? part.inside : part.before;
                if (!qFuzzyCompare(m_brushScale, 1.0) && dynamic_cast<VStrokeShape*>(moving)) {
                    for (qreal &radius : curves.radii) radius *= m_brushScale;
                    moving->setCurvesAndUpdate(curves);
                }
                setTransformationAndUpdate(moving, moved(part.transform, parent));
            }
            for (const auto &other : split.others) {
                setTransformationAndUpdate(other.first, moved(other.second, other.first->parent()));
            }
        } else {
            for (const Part &part : split.parts) {
                if (part.piece) {
                    part.piece->update();
                    part.shape->parent()->removeShape(part.piece);
                    part.shape->setCurvesAndUpdate(part.before);
                } else {
                    part.shape->setCurvesAndUpdate(part.before);
                    setTransformationAndUpdate(part.shape, part.transform);
                }
            }
            for (const auto &other : split.others) {
                setTransformationAndUpdate(other.first, other.second);
            }
        }
        split.applied = redo;
    }

private:
    /// the transformation of a shape in \p parent, from \p local, that goes where m_transform says
    QTransform moved(const QTransform &local, const KoShapeContainer *parent) const
    {
        const QTransform parentTransform = parent ? parent->absoluteTransformation() : QTransform();
        return local * parentTransform * m_transform * parentTransform.inverted();
    }

    KisShapeLayerSP m_layer;
    KisSelectionSP m_selection;
    std::shared_ptr<Split> m_split;
    const QTransform m_transform;
    const qreal m_brushScale;
};

}

VCurveData VSelectionSplit::within(const VCurveShape *shape, KisSelectionSP selection, const QTransform &documentToPixels)
{
    VCurveData result;
    qreal step = 1.0;
    const InsideTest inside = selectionTest(shape, selection, documentToPixels, &step);
    if (!inside) return result;
    const VCurveData &before = shape->curves();
    for (int c = 0; c < before.curveCount(); c++) {
        Q_FOREACH (const VCurveData &piece, VCurveTrim::keepInside(before.curve(c), inside, step)) {
            result.append(piece);
        }
    }
    return result;
}

KUndo2Command *VSelectionSplit::cutCommand(KisShapeLayer *layer, KoShapeControllerBase *controller, KisSelectionSP selection)
{
    if (!VFreehandLayer::isFreehand(layer) || !selection || !controller) return nullptr;
    KisImageSP image = layer->image();
    if (!image) return nullptr;

    std::shared_ptr<Split> split = std::make_shared<Split>();
    split->read(layer->shapes(), selection, QTransform::fromScale(image->xRes(), image->yRes()));
    if (split->parts.isEmpty() && split->others.isEmpty()) return nullptr;
    return new CutCommand(layer, controller, split);
}

KUndo2Command *VSelectionSplit::moveCommand(KisShapeLayer *layer, KisSelectionSP selection, const QTransform &imageTransform,
                                            bool scaleBrushSizes, std::shared_ptr<void> &session)
{
    if (!VFreehandLayer::isFreehand(layer) || !selection) return nullptr;
    KisImageSP image = layer->image();
    if (!image) return nullptr;

    if (!session) session = std::make_shared<Split>();
    const QTransform toDocument = QTransform::fromScale(1.0 / image->xRes(), 1.0 / image->yRes());
    return new MoveCommand(layer, selection, std::static_pointer_cast<Split>(session),
                           toDocument.inverted() * imageTransform * toDocument,
                           scaleBrushSizes ? brushScale(imageTransform) : 1.0);
}

qreal VSelectionSplit::brushScale(const QTransform &t)
{
    return qSqrt(qAbs(t.m11() * t.m22() - t.m12() * t.m21()));
}
