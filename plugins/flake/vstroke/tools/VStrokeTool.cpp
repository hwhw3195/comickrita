/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeTool.h"
#include "VTrace.h"
#include "VStrokeToolSwitcher.h"
#include "VFreehandLayer.h"
#include "VSelectionCover.h"

#include <QPainter>
#include <QPainterPathStroker>
#include <QRandomGenerator>
#include <QSet>

#include <KisViewManager.h>
#include <KoCanvasBase.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoInsets.h>
#include <KoPointerEvent.h>
#include <KoShapeController.h>
#include <KoShapeStrokeModel.h>
#include <KoViewConverter.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_canvas2.h>
#include <kis_cursor.h>
#include <kis_icon.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_processing_applicator.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <kis_smoothing_options.h>
#include <kis_tool_freehand_helper.h>
#include <kis_painting_information_builder.h>
#include <kundo2command.h>
#include <strokes/KisFreehandStrokeInfo.h>

#include "VStrokeShape.h"

namespace {
const QString ColorMixingKey = QStringLiteral("CSP/BrushUseWaterColor");

int randomSeed()
{
    return int(QRandomGenerator::global()->generate() & 0x7fffffff);
}
}

/**
 * Hands the start distance the helper made to the facade, and owns the
 * stroke infos the stroke strategy would normally delete.
 */
class VStrokeFreehandHelper : public KisToolFreehandHelper
{
public:
    using KisToolFreehandHelper::KisToolFreehandHelper;

    ~VStrokeFreehandHelper() override {
        qDeleteAll(m_strokeInfos);
    }

    std::function<void(KisDistanceInformation *dragDistance, qreal startAngle)> paintersCreated;

protected:
    void createPainters(QVector<KisFreehandStrokeInfo*> &strokeInfos,
                        const KisDistanceInformation &startDist) override {
        // the helper forgets the previous ones when its stroke ends
        qDeleteAll(m_strokeInfos);
        m_strokeInfos.clear();

        KisToolFreehandHelper::createPainters(strokeInfos, startDist);
        m_strokeInfos = strokeInfos;

        if (paintersCreated && !strokeInfos.isEmpty()) {
            paintersCreated(strokeInfos.first()->dragDistance, startDist.lastDrawingAngle());
        }
    }

private:
    QVector<KisFreehandStrokeInfo*> m_strokeInfos;
};

VStrokeTool::VStrokeTool(KoCanvasBase *canvas)
    : KisToolFreehand(canvas, KisCursor::load("tool_freehand_cursor.xpm", 2, 2),
                      kundo2_i18n("Vector Brush Stroke"))
{
    setObjectName("tool_vstroke");

    m_helper = new VStrokeFreehandHelper(paintingInformationBuilder(), canvas->resourceManager(),
                                         kundo2_i18n("Vector Brush Stroke"),
                                         new KisSmoothingOptions());
    resetHelper(m_helper);
    // KisToolFreehand connected only the helper it made itself
    connect(m_helper, SIGNAL(requestExplicitUpdateOutline()), SLOT(explicitUpdateOutline()));

    m_helper->paintersCreated = [this] (KisDistanceInformation *dragDistance, qreal startAngle) {
        m_facade.prepare(m_settings, dragDistance);
        m_startAngle = startAngle;
    };
    m_facade.dirtyCallback = [this] (const QVector<QRect> &rects) { slotDirty(rects); };
    m_facade.finishedCallback = [this] () { commitStroke(); };
}

VStrokeTool::~VStrokeTool()
{
    // the pending shapes arrive without us
    QSet<KisShapeLayer*> layers;
    for (LiveStroke &stroke : m_liveStrokes) {
        if (!layers.contains(stroke.layer.data())) {
            layers.insert(stroke.layer.data());
            stroke.layer->setLiveOverlay(KisShapeLayer::LiveOverlay());
        }
    }
}

namespace {

/**
 * Runs a callback after its first redo(): the command adding a shape runs
 * in a stroke job, and the live stroke must be removed after the shape is
 * in its layer.
 */
class NotifyingCommand : public KUndo2Command
{
public:
    NotifyingCommand(const KUndo2MagicString &text, std::function<void()> done)
        : KUndo2Command(text), m_done(done) {}

    void redo() override {
        KUndo2Command::redo();
        if (m_done) {
            m_done();
            m_done = nullptr;
        }
    }

private:
    std::function<void()> m_done;
};

}

QString VStrokeTool::unsupportedPresetReason(KisPaintOpPresetSP preset)
{
    if (!preset) return i18n("No brush preset is selected.");

    // Only these two follow a point's edited thickness (KisPaintInformation::sizeMultiplier())
    // and have a size response for Trim (VInkSize). Others either read the canvas, which a
    // vector stroke does not have, or would draw but ignore the thickness.
    const QString id = preset->paintOp().id();
    if (id != "cspbrush" && id != "paintbrush") {
        return i18n("Vector strokes are drawn with the Pixel and CSP brush engines. The %1 engine cannot draw them.",
                    preset->paintOp().name().isEmpty() ? id : preset->paintOp().name());
    }
    return QString();
}

KisPaintOpPresetSP VStrokeTool::vectorPreset(KisPaintOpPresetSP preset)
{
    if (!preset || preset->paintOp().id() != "cspbrush" ||
        !preset->settings()->getBool(ColorMixingKey, false)) {
        return preset;
    }
    KisPaintOpPresetSP result = preset->clone().dynamicCast<KisPaintOpPreset>();
    result->settings()->setProperty(ColorMixingKey, false);
    return result;
}

QList<KoShape*> VStrokeTool::shapesToClear(KisShapeLayer *layer, KisSelectionSP selection)
{
    return VSelectionCover::shapesToClear(layer, selection);
}

bool VStrokeTool::coveredBySelection(KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels)
{
    return VSelectionCover::coveredBySelection(shape, selection, documentToPixels);
}

KisShapeLayer *VStrokeTool::shapeLayer() const
{
    return dynamic_cast<KisShapeLayer*>(const_cast<VStrokeTool*>(this)->currentNode().data());
}

void VStrokeTool::showMessage(const QString &message)
{
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->showFloatingMessage(message, KisIconUtils::loadIcon("object-locked"));
    }
}

void VStrokeTool::beginPrimaryAction(KoPointerEvent *event)
{
    // KisToolFreehand::beginPrimaryAction() refuses vector layers
    if (!VFreehandLayer::isFreehand(shapeLayer())) {
        showMessage(i18n("The vector brush draws on freehand vector layers. Please select a freehand vector layer."));
        event->ignore();
        return;
    }
    if (!nodeEditable()) {
        event->ignore();
        return;
    }
    const QString reason = unsupportedPresetReason(currentPaintOpPreset());
    if (!reason.isEmpty()) {
        showMessage(reason);
        event->ignore();
        return;
    }
    KIS_SAFE_ASSERT_RECOVER_RETURN(!m_helper->isRunning());

    requestUpdateOutline(event->point, event);
    setMode(KisTool::PAINT_MODE);

    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
        kisCanvas->viewManager()->disableControls();
    }

    initStroke(event);
}

void VStrokeTool::initStroke(KoPointerEvent *event)
{
    VTrace::Scope trace("brush.begin");
    // a cancelled stroke is never committed
    if (m_liveStrokeId) removeLiveStroke(m_liveStrokeId);
    m_liveStrokeId = 0;

    m_resources = new KisResourcesSnapshot(image(), currentNode(), canvas()->resourceManager());
    m_compositeOp = m_resources->compositeOpId();
    if (!VStrokeData::canBlendWith(m_compositeOp)) {
        if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas())) {
            const QString mode = KoCompositeOpRegistry::instance().getKoID(m_compositeOp).name();
            kisCanvas->viewManager()->showFloatingMessage(
                i18n("Vector strokes draw \"%1\" as Normal. The stroke keeps the mode.", mode), QIcon());
        }
    }

    const KoColorSpace *rgb8 = KoColorSpaceRegistry::instance()->rgb8();
    KoColor fg = m_resources->currentFgColor();
    fg.convertTo(rgb8);
    KoColor bg = m_resources->currentBgColor();
    bg.convertTo(rgb8);

    m_settings = VStrokeRenderSettings();
    m_settings.preset = vectorPreset(m_resources->currentPaintOpPreset());
    m_settings.paintColor = m_compositeOp == COMPOSITE_ERASE ? KoColor(Qt::black, rgb8) : fg;
    m_settings.backgroundColor = bg;
    m_settings.opacity = m_resources->opacity();
    m_settings.seed = randomSeed();
    m_settings.perStrokeSeed = randomSeed();

    m_helper->initPaint(event, convertToPixelCoord(event), image(), currentNode(), &m_facade);

    if (KisShapeLayerSP layer = shapeLayer()) {
        LiveStroke stroke;
        stroke.id = m_liveStrokeId = ++m_lastLiveStrokeId;
        stroke.layer = layer;
        stroke.device = m_facade.device();
        stroke.opacity = m_facade.mergeOpacity();
        stroke.compositeOp = m_compositeOp;
        addLiveStroke(stroke);
    }
}

void VStrokeTool::slotDirty(const QVector<QRect> &rects)
{
    for (LiveStroke &stroke : m_liveStrokes) {
        if (stroke.id != m_liveStrokeId) continue;
        Q_FOREACH (const QRect &rc, rects) {
            stroke.layer->updateLiveOverlay(rc);
        }
    }
}

void VStrokeTool::commitStroke()
{
    VTrace::Scope trace("brush.commit");
    const int liveId = m_liveStrokeId;
    m_liveStrokeId = 0;

    KisShapeLayer *layer = shapeLayer();
    const QVector<VStrokePrimitive> primitives = m_facade.primitives();
    if (!layer || primitives.isEmpty() || !m_settings.preset) {
        removeLiveStroke(liveId);
        return;
    }

    KisPaintOpPresetSP original = currentPaintOpPreset();

    VStrokeData data;
    data.preset = m_settings.preset;
    data.presetName = original ? original->name() : m_settings.preset->name();
    data.presetFileName = original ? original->filename() : QString();
    data.presetMd5 = original ? original->md5Sum(false) : QString();
    data.brushSize = m_settings.preset->settings()->paintOpSize();
    data.opacity = m_settings.opacity;
    data.seed = m_settings.seed;
    data.perStrokeSeed = m_settings.perStrokeSeed;
    data.startAngle = m_startAngle;
    data.backgroundColor = m_settings.backgroundColor.toQColor();
    data.compositeOp = m_compositeOp;

    // in document coordinates: adding the shape moves it into the layer's transformation
    VStrokeShape *shape = VStrokeShape::fromImagePixels(primitives, image()->xRes(), data);
    if (!data.erases()) {
        shape->setBackground(QSharedPointer<KoColorBackground>(
            new KoColorBackground(m_settings.paintColor.toQColor())));
    }
    // the live stroke is the rendering: no second render
    shape->adoptRendering(m_facade.mergedDevice());

    // the live stroke stays in the layer until the shape is there
    KUndo2Command *cmd = new NotifyingCommand(kundo2_i18n("Vector Brush Stroke"), [this, liveId] () {
        QMetaObject::invokeMethod(this, [this, liveId] () { removeLiveStroke(liveId); }, Qt::QueuedConnection);
    });
    canvas()->shapeController()->addShape(shape, layer, cmd);
    KisProcessingApplicator::runSingleCommandStroke(image(), cmd, KisStrokeJobData::SEQUENTIAL,
                                                    KisStrokeJobData::EXCLUSIVE);
}

void VStrokeTool::addLiveStroke(const LiveStroke &stroke)
{
    m_liveStrokes << stroke;
    updateLiveOverlay(stroke.layer);
}

void VStrokeTool::removeLiveStroke(int id)
{
    for (int i = 0; i < m_liveStrokes.size(); i++) {
        if (m_liveStrokes[i].id != id) continue;

        LiveStroke stroke = m_liveStrokes.takeAt(i);
        updateLiveOverlay(stroke.layer);
        const QRect rc = stroke.device->extent();
        if (!rc.isEmpty()) stroke.layer->updateLiveOverlay(rc);
        return;
    }
}

void VStrokeTool::updateLiveOverlay(KisShapeLayerSP layer)
{
    QVector<LiveStroke> strokes;
    Q_FOREACH (const LiveStroke &stroke, m_liveStrokes) {
        if (stroke.layer == layer) strokes << stroke;
    }
    if (strokes.isEmpty()) {
        layer->setLiveOverlay(KisShapeLayer::LiveOverlay());
        return;
    }

    layer->setLiveOverlay(liveOverlay(strokes));
}

std::function<void(QPainter&, const QRect&)> VStrokeTool::liveOverlay(const QVector<LiveStroke> &strokes)
{
    // runs in the layer's rendering thread: only reads the devices
    return [strokes] (QPainter &gc, const QRect &imageRect) {
        VTrace::Scope trace("brush.overlay");
        trace.setDetail(QStringLiteral("%1x%2").arg(imageRect.width()).arg(imageRect.height()));
        const KoColorProfile *profile = KoColorSpaceRegistry::instance()->rgb8()->profile();
        Q_FOREACH (const LiveStroke &stroke, strokes) {
            const QRect rc = imageRect & stroke.device->extent();
            if (rc.isEmpty()) continue;

            gc.save();
            gc.setOpacity(stroke.opacity);
            VStrokeData::drawImage(gc, stroke.compositeOp, rc.topLeft(), stroke.device->convertToQImage(profile, rc));
            gc.restore();
        }
    };
}

VStrokeToolFactory::VStrokeToolFactory()
    : KisToolPaintFactoryBase("VStrokeTool")
{
    setToolTip(i18n("Vector Brush Tool: draws brush strokes that stay editable on vector layers"));
    setSection(ToolBoxSection::Shape);
    setIconName(koIconNameCStr("krita_tool_freehandvector"));
    setPriority(2);
    // the brush tool's button stands for this tool (VStrokeToolSwitcher):
    // KoToolBox hides a button whose code is never selected, and
    // KoToolManager keeps a tool active whose ids include flake/always
    setActivationShapeId(QStringLiteral("vstroke/no-button,") + KRITA_TOOL_ACTIVATION_ID + QStringLiteral(",vstroke/no-button"));
}

KoToolBase *VStrokeToolFactory::createTool(KoCanvasBase *canvas)
{
    VStrokeToolSwitcher::start();
    return new VStrokeTool(canvas);
}
