/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushWidgetTest.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDomDocument>
#include <QImageReader>
#include <QDir>
#include <QLabel>
#include <QListView>
#include <QBuffer>
#include <kis_png_brush.h>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QToolButton>
#include <QTest>

#include <kistest.h>

#include <KisGlobalResourcesInterface.h>
#include <kis_paintop_preset.h>
#include <kis_slider_spin_box.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QRadioButton>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>

#include "CspEffector.h"
#include "CspTipCreator.h"
#include "CspEffectorEditor.h"
#include "KisCspBrushSettings.h"
#include "KisCspBrushSettingsWidget.h"

namespace {
KisPropertiesConfigurationSP loadPreset(const QString &name)
{
    QImageReader reader(QString(FILES_DATA_DIR) + "/" + name, "PNG");
    QDomDocument doc;
    doc.setContent(reader.text("preset"));
    KisPropertiesConfigurationSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    for (QDomElement e = doc.documentElement().firstChildElement("param"); !e.isNull(); e = e.nextSiblingElement("param")) {
        settings->setProperty(e.attribute("name"), e.text());
    }
    settings->setProperty("paintop", "cspbrush");
    return settings;
}

/**
 * What the editor writes into a preset's settings. Like Krita's preset
 * editor, which writes into the preset (configuration() is not used there):
 * the locked properties proxy drops writes to settings without an update
 * listener, which only a preset gives them. (A listener of the test's own
 * would not link on Windows: UpdateListener's destructor is not exported.)
 */
KisPropertiesConfigurationSP written(KisCspBrushSettingsWidget &widget)
{
    KisPaintOpSettingsSP blank = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    blank->setProperty("paintop", "cspbrush");
    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setSettings(blank); // clones, and gives the clone the preset's listener
    KisPaintOpSettingsSP settings = preset->settings();
    widget.writeConfiguration(settings);
    return settings;
}

QMap<QString, QString> cspProperties(KisPropertiesConfigurationSP config)
{
    QMap<QString, QString> out;
    const QMap<QString, QVariant> all = config->getProperties();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        if (it.key().startsWith("CSP/")) out[it.key()] = it.value().toString();
    }
    return out;
}
}

void CspBrushWidgetTest::testLoadDoesNotModify()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    QSignalSpy changed(&widget, SIGNAL(sigConfigurationItemChanged()));
    const KisPropertiesConfigurationSP preset = loadPreset("ribbon_wave.kpp");
    widget.setConfiguration(preset);
    QCOMPARE(changed.count(), 0);

    // every CSP setting comes back, also those without a control
    const QMap<QString, QString> before = cspProperties(preset);
    const QMap<QString, QString> after = cspProperties(written(widget));
    QVERIFY(before.size() > 100);
    QCOMPARE(after, before);
}

void CspBrushWidgetTest::testEditsReachConfiguration()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("mypen.kpp"));
    QSignalSpy changed(&widget, SIGNAL(sigConfigurationItemChanged()));

    KisDoubleSliderSpinBox *size = widget.findChild<KisDoubleSliderSpinBox *>("BrushSize");
    QVERIFY(size);
    size->setValue(55.0);
    QVERIFY(changed.count() > 0);
    KisPropertiesConfigurationSP config = written(widget);
    QCOMPARE(config->getDouble("CSP/BrushSize"), 55.0);
    QCOMPARE(config->getInt("CSP/BrushSizeUnit"), 0);
    // the size Krita compares with the Instant Preview threshold (the base class says 1 px: never offered)
    QCOMPARE(widget.effectiveBrushSize().get(), 55.0);
    size->setValue(1000.0);
    QCOMPARE(widget.effectiveBrushSize().get(), 1000.0);
    size->setValue(55.0);

    // a bit of the angle inputs, the other bits stay
    const int angleBits = config->getInt("CSP/BrushRotationEffector");
    QCheckBox *random = widget.findChild<QCheckBox *>(QString("BrushRotationEffector#%1").arg(0x80));
    QVERIFY(random);
    random->setChecked(!(angleBits & 0x80));
    QCOMPARE(written(widget)->getInt("CSP/BrushRotationEffector"), angleBits ^ 0x80);

    QComboBox *mode = widget.findChild<QComboBox *>("BrushColorMixingMode");
    QVERIFY(mode);
    mode->setCurrentIndex(0);
    QCOMPARE(written(widget)->getInt("CSP/BrushColorMixingMode"), 0);

    // a change of the shared data shows on the other pages too
    widget.setConfiguration(loadPreset("ribbon_dash.kpp"));
    QCOMPARE(size->value(), 100.0);
}

void CspBrushWidgetTest::testDynamicsEdit()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("mypen.kpp"));

    // the size slider has the button of CSP's effect source popup
    CspEffectorButton *button = widget.findChild<CspEffectorButton *>("BrushSizeEffector");
    QVERIFY(button);
    CspEffectorEditor *editor = button->editor();
    QCheckBox *pressure = editor->findChild<QCheckBox *>(QString("input%1").arg(CspEffector::Pressure));
    QCheckBox *fade = editor->findChild<QCheckBox *>(QString("input%1").arg(CspEffector::Fade));
    QVERIFY(pressure && fade);

    pressure->setChecked(false);
    fade->setChecked(false);
    QVERIFY(!CspEffector::fromString(written(widget)->getString("CSP/BrushSizeEffector")).isActive());

    QSignalSpy changed(&widget, SIGNAL(sigConfigurationItemChanged()));
    fade->setChecked(true);
    QVERIFY(changed.count() > 0);
    const CspEffector e = CspEffector::fromString(written(widget)->getString("CSP/BrushSizeEffector"));
    QVERIFY(e.isEnabled(CspEffector::Fade));
    QVERIFY(!e.isEnabled(CspEffector::Pressure));
    // the label stays "…"; the tooltip names the inputs in use
    QCOMPARE(button->text(), QString::fromUtf8("…"));
    QVERIFY(button->toolTip().contains(CspEffectorEditor::inputName(CspEffector::Fade)));
    QVERIFY(!button->toolTip().contains(CspEffectorEditor::inputName(CspEffector::Pressure)));
}

void CspBrushWidgetTest::testTexturePage()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("texture_pen.kpp"));

    // CSP's texture settings, with the density's effect source button
    QVERIFY(widget.findChild<CspEffectorButton *>("TextureDensityEffector"));
    QVERIFY(widget.findChild<QComboBox *>("TextureCompositeMode"));
    QLabel *name = widget.findChild<QLabel *>("TexturePatternName");
    QVERIFY(name);
    QVERIFY(name->text().contains("texture"));

    // the pattern (Krita's Texture/Pattern/ keys) survives editing
    KisPropertiesConfigurationSP config = written(widget);
    QVERIFY(config->getBool("Texture/Pattern/Enabled"));
    QVERIFY(!config->getString("Texture/Pattern/PatternMD5Sum").isEmpty());
    QVERIFY(!config->getString("Texture/Pattern/Pattern").isEmpty());

    // Krita's Auto invert for eraser
    QCheckBox *invertOnErase = widget.findChild<QCheckBox *>("TextureAutoInvertOnErase");
    QVERIFY(invertOnErase && invertOnErase->isEnabled());
    QVERIFY(!invertOnErase->isChecked());
    invertOnErase->setChecked(true);
    QVERIFY(written(widget)->getBool("Texture/Pattern/AutoInvertOnErase"));

    QPushButton *remove = widget.findChild<QPushButton *>("TextureRemove");
    QVERIFY(remove && remove->isEnabled());
    QSignalSpy changed(&widget, SIGNAL(sigConfigurationItemChanged()));
    remove->click();
    QVERIFY(changed.count() > 0);
    QVERIFY(!written(widget)->getBool("Texture/Pattern/Enabled"));
    QVERIFY(!remove->isEnabled());
}

void CspBrushWidgetTest::testColorJitterPage()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("mypen.kpp"));

    // Mix, Hue, Saturation and Value fold under their headers, folded at first
    QToolButton *hue = widget.findChild<QToolButton *>("Fold Hue");
    QWidget *hueContent = widget.findChild<QWidget *>("FoldContent Hue");
    QVERIFY(hue && hueContent);
    QVERIFY(widget.findChild<QToolButton *>("Fold Mix (sub color)"));
    QVERIFY(hueContent->isHidden());
    hue->click();
    QVERIFY(!hueContent->isHidden());

    // the per-dab amounts have CSP's effect source popup; per stroke has none
    QVERIFY(hueContent->findChild<CspEffectorButton *>("BrushHueChangeEffector"));
    QVERIFY(!widget.findChild<CspEffectorButton *>("BrushStrokeHueChangeEffector"));

    QCheckBox *perDab = widget.findChild<QCheckBox *>("BrushChangePatternColor");
    KisSliderSpinBox *amount = widget.findChild<KisSliderSpinBox *>("BrushHueChange");
    QVERIFY(perDab && amount);
    perDab->setChecked(true);
    amount->setValue(45);
    KisPropertiesConfigurationSP config = written(widget);
    QCOMPARE(config->getInt("CSP/BrushChangePatternColor"), 1);
    QCOMPARE(config->getInt("CSP/BrushHueChange"), 45);
    QVERIFY(config->getString("hSensor").isEmpty());
}

void CspBrushWidgetTest::testSprayDirection()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPropertiesConfigurationSP preset = loadPreset("spray_shapes.kpp");
    preset->setProperty("CSP/BrushRotationEffectorInSpray", 0x3 | 0x80); // low flags, random
    widget.setConfiguration(preset);

    // one direction at a time; Random is a separate switch, the other bits stay
    QComboBox *direction = widget.findChild<QComboBox *>("BrushRotationEffectorInSpray#combo");
    QCheckBox *random = widget.findChild<QCheckBox *>(QString("BrushRotationEffectorInSpray#%1").arg(0x80));
    QVERIFY(direction && random);
    QCOMPARE(direction->currentIndex(), 0);
    QVERIFY(random->isChecked());

    direction->setCurrentIndex(3); // toward center
    QCOMPARE(written(widget)->getInt("CSP/BrushRotationEffectorInSpray"), 0x3 | 0x80 | 0x200);
    direction->setCurrentIndex(1); // line direction replaces it
    QCOMPARE(written(widget)->getInt("CSP/BrushRotationEffectorInSpray"), 0x3 | 0x80 | 0x40);
    random->setChecked(false);
    QCOMPARE(written(widget)->getInt("CSP/BrushRotationEffectorInSpray"), 0x3 | 0x40);
    QCOMPARE(direction->currentIndex(), 1);
}

void CspBrushWidgetTest::testPageOrder()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());

    // as shown in the editor's option list: one category, the pages in this order
    QListView *list = widget.findChild<QListView *>();
    QVERIFY(list && list->model());
    QStringList shown;
    for (int row = 0; row < list->model()->rowCount(); row++) {
        shown << list->model()->index(row, 0).data(Qt::DisplayRole).toString();
    }
    const QStringList expected = {"General", "Brush tip", "Blending Mode", "Color mixing", "Color jitter",
                                  "Spraying effect", "Texture", "Watercolor edge", "CSP Velocity Calibration"};
    shown.removeAll(QString());
    QCOMPARE(shown.mid(shown.size() - expected.size()), expected);
    QCOMPARE(shown.size(), expected.size() + 1); // the one category header

    // the color mixing mode stays editable with color mixing off
    widget.setConfiguration(loadPreset("mypen.kpp"));
    QCheckBox *mixing = widget.findChild<QCheckBox *>("BrushUseWaterColor");
    QComboBox *mode = widget.findChild<QComboBox *>("BrushColorMixingMode");
    QVERIFY(mixing && mode);
    mixing->setChecked(false);
    QVERIFY(mode->isEnabled());
    QVERIFY(!widget.findChild<QComboBox *>("BrushWaterColor")->isEnabled());
}

void CspBrushWidgetTest::testRandomMinimumRange()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("mypen.kpp"));

    // hue, saturation and value jitter shift both ways; the rest stops at 0% as in CSP
    auto randomMinimum = [&](const QString &effectorKey) {
        CspEffectorButton *button = widget.findChild<CspEffectorButton *>(effectorKey);
        return button ? button->editor()->findChild<KisSliderSpinBox *>(QString("minimum%1").arg(CspEffector::Random))
                      : nullptr;
    };
    for (const QString &key : {"BrushHueChangeEffector", "BrushSaturationChangeEffector", "BrushValueChangeEffector"}) {
        QVERIFY2(randomMinimum(key), qPrintable(key));
        QCOMPARE(randomMinimum(key)->minimum(), -100);
        QCOMPARE(randomMinimum(key)->value(), -100); // CSP's stored default for jitter
    }
    for (const QString &key : {"BrushSubColorEffector", "BrushSizeEffector", "BrushOpacityEffector"}) {
        QVERIFY2(randomMinimum(key), qPrintable(key));
        QCOMPARE(randomMinimum(key)->minimum(), 0);
    }
}

void CspBrushWidgetTest::testPageScreenshots()
{
    // for review only: the pages as laid out, next to the stroke renders
    const QString persistent = QDir::homePath() + "/persistent";
    if (!QDir(persistent).exists()) QSKIP("no ~/persistent");
    const QString dir = persistent + "/cspbrush_renders";
    QDir().mkpath(dir);

    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("mypen.kpp"));
    for (QScrollArea *scroll : widget.findChildren<QScrollArea *>()) {
        if (!scroll->objectName().startsWith("CspPage ")) continue;
        QWidget *content = scroll->widget();
        content->resize(content->sizeHint().expandedTo(QSize(520, 100)));
        content->grab().save(dir + "/ui_" + scroll->objectName().section(' ', 1).replace(' ', '_') + ".png");
    }
}

void CspBrushWidgetTest::testSummary()
{
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPropertiesConfigurationSP preset = loadPreset("ribbon_wave.kpp");
    widget.setConfiguration(preset);

    QLabel *summary = widget.findChild<QLabel *>("BrushSummary");
    QVERIFY(summary);
    QVERIFY(summary->text().contains("ribbon"));
}

void CspBrushWidgetTest::testRibbonGreysOutMixing()
{
    KisPropertiesConfigurationSP preset = loadPreset("ribbon_wave.kpp");
    preset->setProperty("CSP/BrushUseWaterColor", 1);

    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(preset);

    QCheckBox *mixing = widget.findChild<QCheckBox *>("BrushUseWaterColor");
    QComboBox *type = widget.findChild<QComboBox *>("BrushWaterColor");
    QCheckBox *ribbon = widget.findChild<QCheckBox *>("BrushRibbon");
    QVERIFY(mixing && type && ribbon);
    QVERIFY(ribbon->isChecked());
    QVERIFY(!mixing->isEnabled());
    QVERIFY(!type->isEnabled());
    // the mode is also used for gray tips and color jitter
    QVERIFY(widget.findChild<QComboBox *>("BrushColorMixingMode")->isEnabled());

    // ignored, not forgotten: the setting stays, and comes back without Ribbon
    QCOMPARE(written(widget)->getInt("CSP/BrushUseWaterColor"), 1);
    ribbon->setChecked(false);
    QVERIFY(mixing->isEnabled());
    QVERIFY(mixing->isChecked());
    QVERIFY(type->isEnabled());
}

void CspBrushWidgetTest::testTipCreatorFlattensLayers()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(nullptr, 100, 100, cs, "tip source");
    KisPaintLayerSP bottom = new KisPaintLayer(image, "bottom", OPACITY_OPAQUE_U8);
    KisPaintLayerSP top = new KisPaintLayer(image, "top", OPACITY_OPAQUE_U8);
    KisPaintLayerSP other = new KisPaintLayer(image, "other", OPACITY_OPAQUE_U8);
    image->addNode(bottom);
    image->addNode(top);
    image->addNode(other);
    bottom->paintDevice()->fill(QRect(10, 10, 40, 40), KoColor(Qt::red, cs));
    top->paintDevice()->fill(QRect(30, 30, 40, 40), KoColor(Qt::blue, cs));
    other->paintDevice()->fill(QRect(0, 0, 100, 100), KoColor(Qt::green, cs));

    // the selected layers only, in the image's order whatever the order they were selected in
    KisPaintDeviceSP flat = CspTipCreator::flattenLayers(image, {top, bottom});
    QCOMPARE(flat->exactBounds(), QRect(10, 10, 60, 60));
    QImage tip = CspTipCreator::deviceToImage(flat, image->bounds(), nullptr);
    QCOMPARE(tip.size(), QSize(60, 60)); // cut to the content
    QCOMPARE(tip.pixelColor(5, 5), QColor(Qt::red));
    QCOMPARE(tip.pixelColor(25, 25), QColor(Qt::blue)); // the top layer covers the bottom one
    QCOMPARE(tip.pixelColor(55, 55), QColor(Qt::blue));
    QCOMPARE(qAlpha(tip.pixel(55, 5)), 0); // no layer there: the alpha is kept

    // a hidden layer is left out
    top->setVisible(false);
    QCOMPARE(CspTipCreator::flattenLayers(image, {top, bottom})->exactBounds(), QRect(10, 10, 40, 40));
    top->setVisible(true);

    // a selection cuts the tip
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(20, 20, 20, 20));
    selection->updateProjection();
    tip = CspTipCreator::deviceToImage(flat, image->bounds(), selection);
    QCOMPARE(tip.size(), QSize(20, 20));
    QCOMPARE(tip.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(tip.pixelColor(19, 19), QColor(Qt::blue));

    // nothing there: no tip
    QVERIFY(CspTipCreator::deviceToImage(CspTipCreator::flattenLayers(image, {}), image->bounds(), nullptr).isNull());
}

void CspBrushWidgetTest::testTipCreatorTip()
{
    QImage source(2, 1, QImage::Format_ARGB32);
    source.setPixel(0, 0, qRgba(255, 0, 0, 255));
    source.setPixel(1, 0, qRgba(0, 0, 255, 100));

    // Color: as it is
    QCOMPARE(CspTipCreator::toTip(source, false), source);

    // Black and white: gray, and the alpha is always kept
    const QImage gray = CspTipCreator::toTip(source, true);
    for (int x = 0; x < 2; x++) {
        const QRgb c = gray.pixel(x, 0);
        QCOMPARE(qRed(c), qGreen(c));
        QCOMPARE(qGreen(c), qBlue(c));
        QCOMPARE(qRed(c), qGray(source.pixel(x, 0)));
        QCOMPARE(qAlpha(c), qAlpha(source.pixel(x, 0)));
    }
}

void CspBrushWidgetTest::testTipCreatorDialog()
{
    QImage red(4, 4, QImage::Format_ARGB32);
    red.fill(qRgba(255, 0, 0, 255));
    QVector<int> asked;
    auto grab = [&](CspTipCreator::Source source) {
        asked << int(source);
        return source == CspTipCreator::Clipboard ? CspTipCreator::Grabbed()
                                                  : CspTipCreator::Grabbed{red, "Layer 1"};
    };

    {
        // defaults: the selected layers, their name, black and white
        CspTipImport::Tip tip;
        QScopedPointer<QDialog> dialog(CspTipCreator::dialog(nullptr, grab, &tip));
        QCOMPARE(asked, QVector<int>({int(CspTipCreator::SelectedLayers)}));
        QCOMPARE(dialog->findChild<QLineEdit *>("TipName")->text(), QString("Layer 1"));
        QVERIFY(dialog->findChild<QRadioButton *>("TipGray")->isChecked());
        QPushButton *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QVERIFY(ok->isEnabled());
        ok->click();
        QCOMPARE(dialog->result(), int(QDialog::Accepted));
        QCOMPARE(tip.name, QString("Layer 1"));
        QCOMPARE(tip.image.size(), QSize(4, 4));
        QCOMPARE(qRed(tip.image.pixel(0, 0)), qBlue(tip.image.pixel(0, 0)));
        QCOMPARE(qAlpha(tip.image.pixel(0, 0)), 255);
    }
    {
        // Color, and a name of the user's
        CspTipImport::Tip tip;
        QScopedPointer<QDialog> dialog(CspTipCreator::dialog(nullptr, grab, &tip));
        dialog->findChild<QRadioButton *>("TipColor")->setChecked(true);
        dialog->findChild<QLineEdit *>("TipName")->setText("Petal");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(tip.name, QString("Petal"));
        QCOMPARE(tip.image.pixelColor(0, 0), QColor(Qt::red));
    }
    {
        // tags: separated by commas, trimmed, none empty, none twice; known tags are offered
        CspTipImport::Tip tip;
        QStringList tags{"stale"};
        QScopedPointer<QDialog> dialog(CspTipCreator::dialog(nullptr, grab, &tip, &tags, {"Ink", "Pens"}));
        QLineEdit *edit = dialog->findChild<QLineEdit *>("TipTags");
        QVERIFY(edit);
        QVERIFY(edit->text().isEmpty());
        edit->setText("  ink, Pens ,, ink ,  my   tips ");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(tags, QStringList({"ink", "Pens", "my tips"}));
    }
    {
        // no tags typed, and a caller that takes none
        CspTipImport::Tip tip;
        QStringList tags{"stale"};
        QScopedPointer<QDialog> dialog(CspTipCreator::dialog(nullptr, grab, &tip, &tags));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(tags.isEmpty());
    }
    {
        // an empty source can't be created
        CspTipImport::Tip tip;
        QScopedPointer<QDialog> dialog(CspTipCreator::dialog(nullptr, grab, &tip));
        QComboBox *source = dialog->findChild<QComboBox *>("TipSource");
        source->setCurrentIndex(source->findData(int(CspTipCreator::Clipboard)));
        Q_EMIT source->activated(source->currentIndex());
        QVERIFY(!dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(tip.image.isNull());
    }

    // the button is on the Brush tip page
    KisCspBrushSettingsWidget widget;
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.setConfiguration(loadPreset("ribbon_wave.kpp"));
    QVERIFY(widget.findChild<QPushButton *>("CreateTip"));
}

void CspBrushWidgetTest::testTipLibraryPng()
{
    // the file a created tip gets in Krita's brush tip library: a PNG brush tip, marked so that
    // "Add images" takes the picture back unchanged (a gray tip's white part and its alpha included)
    QImage image(6, 4, QImage::Format_ARGB32);
    image.fill(qRgba(255, 255, 255, 180));
    image.setPixel(1, 1, qRgba(0, 0, 0, 255));
    image.setPixel(2, 2, qRgba(0, 0, 0, 0));
    const CspTipImport::Tip tip{"My tip", image};

    QByteArray png = CspTipImport::libraryPng(tip);
    QVERIFY(!png.isEmpty());
    QVERIFY(CspTipImport::isCspTipPng(png));
    QCOMPARE(QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32), image);

    QSharedPointer<KisPngBrush> brush(new KisPngBrush("my_tip.png"));
    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    QVERIFY(brush->loadFromDevice(&buffer, KisGlobalResourcesInterface::instance()));
    QVERIFY(brush->valid());
    QCOMPARE(brush->name(), QString("My tip"));
    const QVector<CspTipImport::Tip> back = CspTipImport::fromBrush(brush, png);
    QCOMPARE(back.size(), 1);
    QCOMPARE(back[0].name, QString("My tip"));
    QCOMPARE(back[0].image, image);

    QVERIFY(CspTipImport::parseTags(" , ,").isEmpty());
    // there is no resource database in this test: nothing is added, nothing crashes
    QVERIFY(!CspTipImport::addToLibrary(tip, {"a"}));
}

KISTEST_MAIN(CspBrushWidgetTest)
