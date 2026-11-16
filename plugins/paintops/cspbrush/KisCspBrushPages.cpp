/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCspBrushPages.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <memory>

#include <KisAngleSelector.h>
#include <KisResourceCacheDb.h>
#include <KisResourceItemChooser.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTextureOptionData.h>
#include <kis_pattern_chooser.h>
#include <kis_properties_configuration.h>
#include <resources/KoPattern.h>
#include <kis_image_config.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

#include "CspBrushParams.h"
#include "CspTextureStage.h"
#include "CspTipCreator.h"
#include "CspTipImport.h"
#include "KisCspBrushOptionPage.h"

namespace {

// ---- images stored in the settings as base64 PNG ("Tips/<i>/Png", "Texture/0/Png")

QImage imageAt(const CspBrushData &d, const QString &group, int index)
{
    const QByteArray png = QByteArray::fromBase64(d.string(QString("%1/%2/Png").arg(group).arg(index)).toLatin1());
    return QImage::fromData(png, "PNG");
}

QString pngBase64(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_ARGB32).save(&buffer, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}

void setImage(CspBrushData &d, const QString &group, int index, const QImage &image, const QString &name)
{
    d.set(QString("%1/%2/Png").arg(group).arg(index), pngBase64(image));
    d.set(QString("%1/%2/Name").arg(group).arg(index), name);
}

void removeImages(CspBrushData &d, const QString &group)
{
    const QString prefix = group + "/";
    for (const QString &key : d.values.keys()) {
        if (key.startsWith(prefix)) d.values.remove(key);
    }
    d.set(group + "/Count", 0);
}

/// Image on a checkerboard, as a thumbnail
QIcon thumbnail(const QImage &image, int size)
{
    QImage out(size, size, QImage::Format_ARGB32);
    QPainter p(&out);
    const int cell = qMax(4, size / 8);
    for (int y = 0; y < size; y += cell) {
        for (int x = 0; x < size; x += cell) {
            p.fillRect(x, y, cell, cell, ((x + y) / cell) % 2 ? QColor(204, 204, 204) : Qt::white);
        }
    }
    if (!image.isNull()) {
        const QImage scaled = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    }
    p.end();
    return QIcon(QPixmap::fromImage(out));
}

QStringList openImages(QWidget *parent, const QString &title, bool multiple)
{
    QStringList filters;
    for (const QByteArray &format : QImageReader::supportedImageFormats()) filters << "*." + QString::fromLatin1(format);
    const QString filter = i18n("Images (%1)", filters.join(' '));
    if (multiple) return QFileDialog::getOpenFileNames(parent, title, QString(), filter);
    const QString file = QFileDialog::getOpenFileName(parent, title, QString(), filter);
    return file.isEmpty() ? QStringList() : QStringList(file);
}

// ---- pages

/// CspBrushParams::ribbonActive() on the raw keys: CspBrushParams::fromData decodes every image, too slow for each edit
static bool ribbonActive(const CspBrushData &d)
{
    return d.boolean("BrushRibbon") && d.boolean("BrushUsePatternImage") && d.integer("Tips/Count") > 0
        && !d.boolean("BrushUseSpray");
}

KisCspBrushOptionPage *generalPage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("General"), KisPaintOpOption::GENERAL, data, QString(), true);

    page->addSection(i18n("Brush size"));
    page->addLength("BrushSize", i18n("Brush size:"), 0.01, KisImageConfig(true).maxBrushSize(), 20.0, "BrushSizeEffector");
    page->addCheck("BrushAtLeast1Pixel", i18n("At least 1 pixel"));
    page->addNote(i18n("The brush size is the length of the tip's longest side."));

    page->addSection(i18n("Ink"));
    page->addInt("Opacity", i18n("Opacity:"), 0, 100, 100, i18n("%"), "BrushOpacityEffector");
    page->addNote(i18n("The blend mode is on the Blending Mode page, the color mixing mode on the Color mixing page."));

    page->addSection(i18n("Stroke"));
    page->addCombo("BrushAutoIntervalType", i18n("Gap:"), {i18n("Fixed"), i18n("Wide"), i18n("Normal"), i18n("Narrow")}, 2);
    // the slider's row, with its effector button
    QWidget *gap = page->addReal("BrushInterval", i18n("Fixed gap:"), 0.5, 1000.0, 1, 10.0, i18n("%"),
                                 "BrushIntervalEffector", 3.0)->parentWidget();
    page->addCheck("BrushContinuousPlot", i18n("Continuous spraying"));
    QCheckBox *ribbon = page->addCheck("BrushRibbon", i18n("Ribbon"));
    page->onUpdate([gap, ribbon](const CspBrushData &d) {
        gap->setEnabled(d.integer("BrushAutoIntervalType", 2) == CspBrushParams::SpacingFixed);
        ribbon->setEnabled(d.boolean("BrushUsePatternImage") && !d.boolean("BrushUseSpray"));
    });
    page->addNote(i18n("A ribbon needs a material tip and no spraying effect."));
    return page;
}

KisCspBrushOptionPage *colorMixingPage(lager::cursor<CspBrushData> data)
{
    // not a checkable page: the mixing mode also applies without color mixing, and a page that is off
    // disables all its controls
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Color mixing"), KisPaintOpOption::GENERAL, data);
    page->addCombo("BrushColorMixingMode", i18n("Color mixing mode:"), {i18n("Standard (Ver. 1)"), i18n("Perceptual")}, 1);
    page->addNote(i18n("The mixing mode is used for gray tips (main/sub color), color jitter and color mixing."));
    if (!CspColorMath::hasSpectral()) {
        page->addNote(i18n("Built without libmypaint: Perceptual mixes in OkLab instead of spectrally."));
    }

    QCheckBox *mixing = page->addCheck("BrushUseWaterColor", i18n("Color mixing"));
    QList<QWidget *> controls;
    QWidget *mode = page->addCombo("BrushWaterColor", i18n("Mode:"), {i18n("Blend"), i18n("Running color"), i18n("Smear")});
    // the slider rows, with their effector buttons
    QWidget *amount = page->addInt("BrushMixColor", i18n("Amount of paint:"), 0, 100, 100, i18n("%"), "BrushMixColorEffector")
                          ->parentWidget();
    QWidget *density = page->addInt("BrushMixAlpha", i18n("Density of paint:"), 0, 100, 100, i18n("%"), "BrushMixAlphaEffector")
                           ->parentWidget();
    QWidget *stretch = page->addInt("BrushMixColorExtension", i18n("Color stretch:"), 0, 100, 50, i18n("%"));
    QCheckBox *link = page->addCheck("BrushBlurLinkSize", i18n("Blur depends on brush size"));
    KisDoubleSliderSpinBox *blur = page->addReal("BrushBlur", i18n("Blur:"), 0, 100, 1, 0);
    page->onUpdate([=](const CspBrushData &d) {
        const bool ribbon = ribbonActive(d);
        const bool on = d.boolean("BrushUseWaterColor") && !ribbon;
        const int type = d.integer("BrushWaterColor");
        mixing->setEnabled(!ribbon);
        mode->setEnabled(on);
        // Smear paints opaque: CSP has no Density of paint for it
        density->setEnabled(on && type != CspBrushParams::MixSmear);
        stretch->setEnabled(on);
        amount->setEnabled(on);
        const bool running = on && type == CspBrushParams::MixRunningColor;
        blur->setEnabled(running);
        link->setEnabled(running);
        blur->setSuffix(d.boolean("BrushBlurLinkSize") ? i18n("% of size") : i18n(" px"));
    });
    page->addNote(i18n("Color mixing paints directly onto the layer (raster layers only)."));
    page->addNote(i18n("Ribbon brushes ignore color mixing."));
    return page;
}

KisCspBrushOptionPage *colorJitterPage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Color jitter"), KisPaintOpOption::GENERAL, data);
    page->addCheck("BrushChangePatternColor", i18n("Change brush tip color (per dab)"));
    page->addCheck("BrushChangeStrokeColor", i18n("Change color for each stroke"));

    struct Channel {
        QString title;
        QString key;       ///< per dab; its effector is key + "Effector"
        QString strokeKey;
        int max;
        QString suffix;
    };
    const Channel channels[] = {
        {i18n("Mix (sub color)"), "BrushSubColor", "BrushStrokeSubColor", 100, i18n("%")},
        {i18n("Hue"), "BrushHueChange", "BrushStrokeHueChange", 180, i18n("°")},
        {i18n("Saturation"), "BrushSaturationChange", "BrushStrokeSaturationChange", 100, i18n("%")},
        {i18n("Value"), "BrushValueChange", "BrushStrokeValueChange", 100, i18n("%")},
    };
    QList<QWidget *> dabControls;
    QList<QWidget *> strokeControls;
    for (const Channel &c : channels) {
        page->beginFoldSection(c.title);
        // the slider's row, with its effector button
        dabControls << page->addInt(c.key, i18n("Per dab:"), 0, c.max, 0, c.suffix, c.key + "Effector")->parentWidget();
        strokeControls << page->addInt(c.strokeKey, i18n("Per stroke:"), 0, c.max, 0, c.suffix);
    }
    page->endFoldSection();
    page->addNote(i18n("Per dab, each amount is scaled by its effect source: CSP's default, Random down to -100%, "
                       "shifts the color both ways. Mix blends the main color toward the sub color."));

    page->onUpdate([=](const CspBrushData &d) {
        for (QWidget *w : dabControls) w->setEnabled(d.boolean("BrushChangePatternColor"));
        for (QWidget *w : strokeControls) w->setEnabled(d.boolean("BrushChangeStrokeColor"));
    });
    return page;
}

KisCspBrushOptionPage *brushTipPage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Brush tip"), KisPaintOpOption::GENERAL, data);
    QFormLayout *form = page->form();

    page->addCombo("BrushUsePatternImage", i18n("Tip shape:"), {i18n("Circle"), i18n("Material")});

    // material tips
    QListWidget *tips = new QListWidget(page->pageWidget());
    tips->setViewMode(QListView::IconMode);
    tips->setIconSize(QSize(56, 56));
    tips->setMovement(QListView::Static);
    tips->setResizeMode(QListView::Adjust);
    tips->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tips->setMaximumHeight(150);
    tips->setObjectName("TipList");
    form->addRow(tips);

    QWidget *buttons = new QWidget(page->pageWidget());
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    QPushButton *add = new QPushButton(i18n("Add images..."), buttons);
    QPushButton *createNew = new QPushButton(i18n("+Create New"), buttons);
    createNew->setObjectName("CreateTip");
    createNew->setToolTip(i18n("Make a brush tip from the selected layers, the image or the clipboard. "
                               "It is added to this brush and to Krita's brush tip library."));
    QPushButton *remove = new QPushButton(i18n("Remove"), buttons);
    buttonLayout->addWidget(add);
    buttonLayout->addWidget(createNew);
    buttonLayout->addWidget(remove);
    buttonLayout->addStretch();
    form->addRow(buttons);

    auto addTips = [page](const QVector<CspTipImport::Tip> &added) {
        if (added.isEmpty()) return;
        page->edit([&](CspBrushData &d) {
            int count = d.integer("Tips/Count");
            for (const CspTipImport::Tip &tip : added) setImage(d, "Tips", count++, tip.image, tip.name);
            d.set("Tips/Count", count);
            d.set("BrushUsePatternImage", 1);
        });
    };
    auto addFiles = [addTips](QWidget *parent) {
        QVector<CspTipImport::Tip> added;
        for (const QString &file : openImages(parent, i18n("Add brush tip images"), true)) {
            const QImage image(file);
            if (!image.isNull()) added << CspTipImport::Tip{QFileInfo(file).completeBaseName(), image};
        }
        addTips(added);
    };

    QObject::connect(createNew, &QPushButton::clicked, page, [createNew, addTips]() {
        CspTipImport::Tip tip;
        QStringList tags;
        if (!CspTipCreator::create(createNew, &tip, &tags)) return;
        addTips({tip});
        // also a brush tip of Krita's library, where "Add images" and the other engines find it
        CspTipImport::addToLibrary(tip, tags);
    });

    if (!KisResourceCacheDb::isValid()) {
        // no resource database (headless tests): image files only
        QObject::connect(add, &QPushButton::clicked, page, [add, addFiles]() { addFiles(add); });
    } else {
        // Krita's brush tip library; every frame of an animated brush becomes a tip
        QWidget *popup = new QWidget(add, Qt::Popup);
        QVBoxLayout *popupLayout = new QVBoxLayout(popup);
        KisResourceItemChooser *chooser = new KisResourceItemChooser(ResourceType::Brushes, false, popup);
        chooser->setObjectName("TipChooser");
        chooser->showTaggingBar(true);
        popupLayout->addWidget(chooser, 1);
        QHBoxLayout *popupButtons = new QHBoxLayout();
        QPushButton *addSelected = new QPushButton(i18n("Add selected tip"), popup);
        QPushButton *fromFile = new QPushButton(i18n("From file..."), popup);
        popupButtons->addWidget(addSelected);
        popupButtons->addStretch();
        popupButtons->addWidget(fromFile);
        popupLayout->addLayout(popupButtons);
        popup->resize(380, 460);

        QObject::connect(add, &QPushButton::clicked, popup, [add, popup]() {
            popup->move(add->mapToGlobal(QPoint(0, add->height())));
            popup->show();
        });
        QObject::connect(addSelected, &QPushButton::clicked, page, [chooser, popup, addTips]() {
            KoResourceSP resource = chooser->currentResource();
            KisBrushSP brush = resource.dynamicCast<KisBrush>();
            if (!brush) return;
            // the file itself only matters for PNG tips written by sut2kpp (see CspTipImport)
            QByteArray raw;
            if (resource->filename().endsWith(".png", Qt::CaseInsensitive)) {
                QBuffer buffer(&raw);
                buffer.open(QIODevice::WriteOnly);
                KisResourceModel(ResourceType::Brushes).exportResource(resource, &buffer);
            }
            addTips(CspTipImport::fromBrush(brush, raw));
            popup->hide();
        });
        QObject::connect(fromFile, &QPushButton::clicked, page, [popup, add, addFiles]() {
            popup->hide();
            addFiles(add);
        });
    }
    QObject::connect(remove, &QPushButton::clicked, page, [page, tips]() {
        QList<int> selected;
        for (const QModelIndex &index : tips->selectionModel()->selectedIndexes()) selected << index.row();
        if (selected.isEmpty()) return;
        page->edit([&](CspBrushData &d) {
            QList<QPair<QImage, QString>> kept;
            const int count = d.integer("Tips/Count");
            for (int i = 0; i < count; i++) {
                if (!selected.contains(i)) kept << qMakePair(imageAt(d, "Tips", i), d.string(QString("Tips/%1/Name").arg(i)));
            }
            removeImages(d, "Tips");
            for (int i = 0; i < kept.size(); i++) setImage(d, "Tips", i, kept[i].first, kept[i].second);
            d.set("Tips/Count", kept.size());
        });
    });

    // rebuilding thumbnails is slow for big tips: only when the tips changed
    auto tipsKey = std::make_shared<QString>();
    page->onUpdate([tips, tipsKey](const CspBrushData &d) {
        QString key;
        const int count = d.integer("Tips/Count");
        for (int i = 0; i < count; i++) key += d.string(QString("Tips/%1/Png").arg(i)).right(64);
        if (key == *tipsKey && tips->count() == count) return;
        *tipsKey = key;
        tips->clear();
        for (int i = 0; i < count; i++) {
            QListWidgetItem *item = new QListWidgetItem(thumbnail(imageAt(d, "Tips", i), 56), QString(), tips);
            item->setToolTip(d.string(QString("Tips/%1/Name").arg(i)));
        }
    });

    QWidget *hardness = page->addInt("BrushHardness", i18n("Hardness:"), 1, 100, 100);
    QWidget *antiAlias = page->addCombo("AntiAlias", i18n("Anti-aliasing:"), {i18n("None"), i18n("Weak"), i18n("Middle"), i18n("Strong")}, 2);
    page->addInt("BrushThickness", i18n("Thickness:"), 1, 100, 100, i18n("%"), "BrushThicknessEffector");
    page->addCheck("BrushVerticalThicknes", i18n("Thickness applies vertically"));

    page->addSection(i18n("Direction"));
    page->addAngle("BrushRotation", i18n("Angle:"));
    page->addBit("BrushRotationEffector", i18n("Pen direction (tilt)"), CspBrushParams::AnglePenDirection);
    page->addBit("BrushRotationEffector", i18n("Pen rotation"), CspBrushParams::AnglePenRotation);
    page->addBit("BrushRotationEffector", i18n("Line direction"), CspBrushParams::AngleLineDirection);
    page->addBit("BrushRotationEffector", i18n("Random"), CspBrushParams::AngleRandom);
    QWidget *randomScale = page->addInt("BrushRotationRandomScale", i18n("Random range:"), 0, 100, 100, i18n("%"));

    page->addSection(i18n("Density"));
    page->addInt("BrushFlow", i18n("Brush density:"), 0, 100, 100, i18n("%"), "BrushFlowEffector");
    // "Adjust density by gap" (BrushAdjustFlowByInterval) has no control: the engine reads the value but
    // does nothing with it yet. A preset keeps its value. Add the control back with the engine code

    page->addSection(i18n("Material"));
    const QStringList flips = {i18n("None"), i18n("Flip"), i18n("Random")};
    QWidget *flipH = page->addCombo("BrushPatternReverseHorizontal", i18n("Flip horizontal:"), flips);
    QWidget *flipV = page->addCombo("BrushPatternReverseVertical", i18n("Flip vertical:"), flips);
    QWidget *order = page->addCombo("BrushPatternOrderType2", i18n("Repeat method:"),
                                    {i18n("Repeat"), i18n("Reverse"), i18n("No repeat"), i18n("Random"),
                                     i18n("One time only"), i18n("One random cycle")});
    // "Blend tips by darken" (BrushBlendPatternByDarken): no control, as for "Adjust density by gap" above
    QCheckBox *swap = page->addCheck("x-SwapMainSub", i18n("Swap main/sub color of gray tips"));
    swap->setToolTip(i18n("Gray tips paint black with the main (foreground) color and white with the sub "
                          "(background) color. Check to swap them."));

    page->onUpdate([=](const CspBrushData &d) {
        const bool material = d.boolean("BrushUsePatternImage");
        hardness->setEnabled(!material);
        antiAlias->setEnabled(!material);
        for (QWidget *w : {static_cast<QWidget *>(tips), buttons, flipH, flipV, order, static_cast<QWidget *>(swap)}) {
            w->setEnabled(material);
        }
        randomScale->setEnabled(d.integer("BrushRotationEffector") & CspBrushParams::AngleRandom);
    });
    return page;
}

KisCspBrushOptionPage *sprayPage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Spraying effect"), KisPaintOpOption::GENERAL, data, "BrushUseSpray");
    page->addLength("BrushSpraySize", i18n("Particle size:"), 0.01, 1000.0, 2.0, "BrushSpraySizeEffector");
    page->addCheck("BrushSpraySizeSyncBrushSize", i18n("Particle size follows brush size"));
    page->addInt("BrushSprayDensity", i18n("Particle density:"), 1, 100, 1, QString(), "BrushSprayDensityEffector");
    page->addInt("BrushSprayBias", i18n("Spray deviation:"), -100, 100, 0);

    page->addSection(i18n("Particle direction"));
    page->addAngle("BrushRotationInSpray", i18n("Angle:"));
    page->addComboBits("BrushRotationEffectorInSpray", i18n("Direction:"),
                       {i18n("None"), i18n("Line direction"), i18n("Whole spray"), i18n("Toward center")},
                       CspBrushParams::SprayDirectionMask,
                       {0, CspBrushParams::SprayLineDirection, CspBrushParams::SprayWhole, CspBrushParams::SprayTowardCenter});
    page->addBit("BrushRotationEffectorInSpray", i18n("Random"), CspBrushParams::SprayRandom);
    QWidget *random = page->addInt("BrushRotationRandomInSpray", i18n("Random range:"), 0, 100, 100, i18n("%"));
    page->onUpdate([random](const CspBrushData &d) {
        random->setEnabled(d.integer("BrushRotationEffectorInSpray") & CspBrushParams::SprayRandom);
    });
    return page;
}

KisCspBrushOptionPage *texturePage(lager::cursor<CspBrushData> data, KisResourcesInterfaceSP resourcesInterface)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Texture"), KisPaintOpOption::GENERAL, data);
    QFormLayout *form = page->form();

    // the image: a pattern of Krita's library (Texture/Pattern/..., outside the CSP data)
    QWidget *row = new QWidget(page->pageWidget());
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    QLabel *preview = new QLabel(row);
    preview->setFixedSize(72, 72);
    QLabel *name = new QLabel(row);
    name->setObjectName("TexturePatternName");
    name->setWordWrap(true);
    QPushButton *choose = new QPushButton(i18n("Choose..."), row);
    choose->setObjectName("TextureChoose");
    QPushButton *remove = new QPushButton(i18n("Remove"), row);
    remove->setObjectName("TextureRemove");
    layout->addWidget(preview);
    layout->addWidget(name, 1);
    layout->addWidget(choose);
    layout->addWidget(remove);
    form->addRow(i18n("Texture:"), row);

    QList<QWidget *> controls;
    // the slider's row, with its effector button
    controls << page->addInt("TextureDensity", i18n("Texture density:"), 0, 100, 50, i18n("%"), "TextureDensityEffector")
                    ->parentWidget();
    controls << page->addReal("TextureScale2", i18n("Scale:"), 1.0, 1000.0, 2, 100.0, i18n("%"), QString(), 3.0);
    controls << page->addReal("TextureRotate", i18n("Rotation:"), -180.0, 180.0, 1, 0.0, i18n("°"));
    // TODO CSP session: the stored codes of these modes are not confirmed yet
    controls << page->addCombo("TextureCompositeMode", i18n("Texture composite mode:"),
                               {i18n("Normal"), i18n("Multiply"), i18n("Subtract"), i18n("Compare"), i18n("Outline"),
                                i18n("Overlay"), i18n("Color dodge"), i18n("Hard mix"), i18n("Height")});
    controls << page->addCheck("TextureReverseDensity", i18n("Reverse density"));
    // Krita's option (Texture/Pattern/AutoInvertOnErase), not CSP's
    QCheckBox *invertOnErase = new QCheckBox(i18n("Auto invert for eraser"), page->pageWidget());
    invertOnErase->setObjectName("TextureAutoInvertOnErase");
    invertOnErase->setToolTip(i18n("Reverse the texture density when the brush erases (eraser mode or the Erase blending mode)"));
    form->addRow(QString(), invertOnErase);
    controls << invertOnErase;
    controls << page->addCheck("TextureStressDensity", i18n("Emphasize density"));
    controls << page->addInt("TextureBrightness", i18n("Brightness:"), -100, 100, 0);
    controls << page->addInt("TextureContrast", i18n("Contrast:"), -100, 100, 0);
    page->addNote(i18n("Texture composite modes and their numbering are first guesses, to be fitted to CSP reference strokes."));

    auto texture = std::make_shared<KisTextureOptionData>();
    auto show = [=](const QImage &image) {
        const bool on = texture->isEnabled && !texture->textureData.isNull();
        preview->setPixmap(thumbnail(on ? image : QImage(), 72).pixmap(72, 72));
        name->setText(on ? i18n("%1\n%2 x %3 px", texture->textureData.name, image.width(), image.height())
                         : i18n("None"));
        remove->setEnabled(on);
        for (QWidget *w : controls) w->setEnabled(on);
        const QSignalBlocker blocker(invertOnErase);
        invertOnErase->setChecked(texture->autoInvertOnErase);
    };
    page->addExternalSettings(
        [=](const KisPropertiesConfiguration *settings) {
            texture->read(settings);
            show(CspTextureStage::loadPattern(settings, resourcesInterface));
        },
        [texture](KisPropertiesConfiguration *settings) {
            texture->write(settings);
            // Krita writes only the link; a converted preset keeps its inline copy as the fallback
            if (!texture->textureData.patternBase64.isEmpty()) {
                settings->setProperty("Texture/Pattern/Pattern", texture->textureData.patternBase64);
            }
        });
    show(QImage());

    QObject::connect(invertOnErase, &QCheckBox::toggled, page, [=](bool on) {
        texture->autoInvertOnErase = on;
        page->notifyExternalChange();
    });
    QObject::connect(remove, &QPushButton::clicked, page, [=]() {
        texture->isEnabled = false;
        show(QImage());
        page->notifyExternalChange();
    });

    // the chooser needs the resource database, which headless tests don't have
    if (!KisResourceCacheDb::isValid()) {
        choose->setEnabled(false);
        choose->setToolTip(i18n("Krita's pattern library is not available"));
        return page;
    }
    QWidget *popup = new QWidget(choose, Qt::Popup);
    QVBoxLayout *popupLayout = new QVBoxLayout(popup);
    KisPatternChooser *chooser = new KisPatternChooser(popup);
    popupLayout->addWidget(chooser);
    popup->resize(360, 420);
    QObject::connect(choose, &QPushButton::clicked, popup, [choose, popup]() {
        popup->move(choose->mapToGlobal(QPoint(0, choose->height())));
        popup->show();
    });
    QObject::connect(chooser, &KisPatternChooser::resourceSelected, page, [=](KoResourceSP resource) {
        KoPatternSP pattern = resource.dynamicCast<KoPattern>();
        if (!pattern) return;
        texture->textureData = KisEmbeddedTextureData::fromPattern(pattern);
        texture->isEnabled = true;
        show(pattern->pattern());
        page->notifyExternalChange();
    });
    return page;
}

KisCspBrushOptionPage *waterEdgePage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("Watercolor edge"), KisPaintOpOption::GENERAL, data, "BrushUseWaterEdge");
    page->addLength("BrushWaterEdgeRadius", i18n("Range:"), 0.1, 100.0, 3.0);
    page->addInt("BrushWaterEdgeAlphaPower", i18n("Opacity effect:"), 0, 100, 20, i18n("%"));
    page->addInt("BrushWaterEdgeValuePower", i18n("Darkness effect:"), 0, 100, 0, i18n("%"));
    page->addNote(i18n("The edge is applied while painting; \"process after drag\" needs a Krita core hook."));
    return page;
}

KisCspBrushOptionPage *enginePage(lager::cursor<CspBrushData> data)
{
    KisCspBrushOptionPage *page = new KisCspBrushOptionPage(i18n("CSP Velocity Calibration"), KisPaintOpOption::GENERAL, data);
    page->addReal("x-VelocityFast", i18n("Fast speed:"), 0.1, 20.0, 2, 2.0, i18n(" px/ms"));
    page->addReal("x-VelocityKnee", i18n("Knee:"), 0.01, 5.0, 2, 0.3, i18n(" px/ms"));
    page->addNote(i18n("Speed at which CSP's velocity input reaches \"fast\", and where its logarithmic response bends."));

    page->addSection(i18n("Brush summary"));
    QLabel *info = new QLabel(page->pageWidget());
    info->setObjectName("BrushSummary");
    info->setWordWrap(true);
    page->form()->addRow(info);
    page->onUpdate([info](const CspBrushData &d) {
        const bool useMaterial = d.boolean("BrushUsePatternImage");
        const int tipCount = d.integer("Tips/Count");
        const bool useSpray = d.boolean("BrushUseSpray");
        QStringList lines;
        lines << (useMaterial ? i18np("%1 material tip", "%1 material tips", tipCount) : i18n("Circle tip"));
        const bool ribbon = ribbonActive(d);
        if (d.boolean("BrushUseWaterColor") && !ribbon) lines << i18n("color mixing");
        if (ribbon) lines << i18n("ribbon");
        if (useSpray) lines << i18n("spray");
        if (d.boolean("BrushUseWaterEdge")) lines << i18n("watercolor edge");
        info->setText(lines.join(QStringLiteral(" · ")));
    });
    return page;
}
}

QList<KisCspBrushOptionPage *> createCspBrushPages(lager::cursor<CspBrushData> data,
                                                   KisResourcesInterfaceSP resourcesInterface)
{
    return {generalPage(data),    brushTipPage(data), colorMixingPage(data),
            colorJitterPage(data), sprayPage(data), texturePage(data, resourcesInterface), waterEdgePage(data),
            enginePage(data)};
}
