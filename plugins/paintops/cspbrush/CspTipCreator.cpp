/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspTipCreator.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <memory>

#include <KisImageBarrierLock.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceTypes.h>
#include <KisTag.h>
#include <KisTagModel.h>
#include <KisViewManager.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <kis_clipboard.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_layer_utils.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <klocalizedstring.h>

namespace {
/// @p image cut to the pixels that are not transparent (null when all are)
QImage cropToContent(const QImage &image)
{
    int left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            if (!qAlpha(line[x])) continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    if (right < left || bottom < top) return QImage();
    return image.copy(left, top, right - left + 1, bottom - top + 1);
}

bool hasAncestorIn(KisNodeSP node, const KisNodeList &nodes)
{
    for (KisNodeSP parent = node->parent(); parent; parent = parent->parent()) {
        if (nodes.contains(parent)) return true;
    }
    return false;
}
}

KisPaintDeviceSP CspTipCreator::flattenLayers(KisImageSP image, const KisNodeList &nodes)
{
    // Right now flattens everything but might implement vector stamp brushes, idk for now
    KisPaintDeviceSP result = new KisPaintDevice(image->colorSpace());

    KisNodeList layers;
    for (KisNodeSP node : nodes) {
        if (!node || !qobject_cast<KisLayer *>(node.data()) || !node->visible()) continue;
        // a group's picture already holds its layers
        if (hasAncestorIn(node, nodes)) continue;
        layers << node;
    }
    // the order they have in the image, bottom first
    layers = KisLayerUtils::sortMergeableNodes(image->root(), layers);

    KisPainter gc(result);
    for (KisNodeSP node : layers) {
        KisPaintDeviceSP projection = node->projection();
        if (!projection) continue;
        const QRect rc = projection->exactBounds();
        if (rc.isEmpty()) continue;
        if (!(*projection->colorSpace() == *result->colorSpace())) {
            projection = new KisPaintDevice(*projection);
            projection->convertTo(result->colorSpace());
        }
        gc.setOpacityF(node->opacity() / 255.0);
        gc.setCompositeOpId(node->compositeOpId());
        gc.bitBlt(rc.topLeft(), projection, rc);
    }
    gc.end();
    return result;
}

QImage CspTipCreator::deviceToImage(KisPaintDeviceSP device, const QRect &limit, KisSelectionSP selection)
{
    if (!device) return QImage();
    QRect rc = device->exactBounds();
    if (limit.isValid()) rc &= limit;
    if (selection) rc &= selection->selectedExactRect();
    if (rc.isEmpty()) return QImage();

    QImage image = device->convertToQImage(nullptr, rc).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return QImage();

    if (selection) {
        QByteArray mask(rc.width() * rc.height(), 0);
        selection->projection()->readBytes(reinterpret_cast<quint8 *>(mask.data()), rc);
        const quint8 *m = reinterpret_cast<const quint8 *>(mask.constData());
        for (int y = 0; y < image.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++, m++) {
                const QRgb c = line[x];
                line[x] = qRgba(qRed(c), qGreen(c), qBlue(c), qAlpha(c) * *m / 255);
            }
        }
    }
    return cropToContent(image);
}

QImage CspTipCreator::toTip(const QImage &image, bool gray)
{
    QImage tip = image.convertToFormat(QImage::Format_ARGB32);
    if (!gray) return tip;
    for (int y = 0; y < tip.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(tip.scanLine(y));
        for (int x = 0; x < tip.width(); x++) {
            const int g = qGray(line[x]);
            line[x] = qRgba(g, g, g, qAlpha(line[x]));
        }
    }
    return tip;
}

CspTipCreator::Grabbed CspTipCreator::grab(Source source)
{
    Grabbed grabbed;
    if (source == Clipboard) {
        KisPaintDeviceSP clip = KisClipboard::instance()->clip(QRect(), false);
        if (clip) {
            grabbed.image = deviceToImage(clip, QRect(), nullptr);
            grabbed.name = i18nc("default name of a brush tip made from the clipboard", "Clipboard");
        }
        return grabbed;
    }

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *view = window ? window->viewManager() : nullptr;
    KisImageSP image = view ? view->image() : nullptr;
    if (!image) return grabbed;

    KisPaintDeviceSP device;
    {
        KisImageReadOnlyBarrierLock lock(image);
        if (source == SelectedLayers) {
            const KisNodeList nodes = view->nodeManager() ? view->nodeManager()->selectedNodes() : KisNodeList();
            device = flattenLayers(image, nodes);
            for (KisNodeSP node : nodes) {
                if (node && qobject_cast<KisLayer *>(node.data())) {
                    grabbed.name = node->name();
                    break;
                }
            }
        } else {
            device = new KisPaintDevice(*image->projection());
            grabbed.name = image->objectName();
        }
    }
    grabbed.image = deviceToImage(device, image->bounds(), image->globalSelection());
    return grabbed;
}

QDialog *CspTipCreator::dialog(QWidget *parent, const std::function<Grabbed(Source)> &grab, CspTipImport::Tip *result,
                               QStringList *tags, const QStringList &knownTags)
{
    QDialog *dialog = new QDialog(parent);
    dialog->setWindowTitle(i18nc("@title:window", "Create Brush Tip"));
    QVBoxLayout *layout = new QVBoxLayout(dialog);

    QHBoxLayout *top = new QHBoxLayout();
    QLabel *preview = new QLabel(dialog);
    preview->setObjectName("TipPreview");
    preview->setFixedSize(110, 110);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    preview->setAlignment(Qt::AlignCenter);
    preview->setWordWrap(true);
    top->addWidget(preview);

    QFormLayout *form = new QFormLayout();
    QHBoxLayout *sourceRow = new QHBoxLayout();
    QComboBox *sourceBox = new QComboBox(dialog);
    sourceBox->setObjectName("TipSource");
    sourceBox->addItem(i18nc("@item:inlistbox brush tip source", "Selected layers"), int(SelectedLayers));
    sourceBox->addItem(i18nc("@item:inlistbox brush tip source", "Image"), int(WholeImage));
    sourceBox->addItem(i18nc("@item:inlistbox brush tip source", "Clipboard"), int(Clipboard));
    QPushButton *reload = new QPushButton(i18nc("@action:button reload image", "Reload"), dialog);
    sourceRow->addWidget(sourceBox, 1);
    sourceRow->addWidget(reload);
    form->addRow(i18nc("@label:listbox image source", "Source:"), sourceRow);

    QLineEdit *name = new QLineEdit(dialog);
    name->setObjectName("TipName");
    form->addRow(i18nc("@label:textbox brush tip name", "Name:"), name);

    QLineEdit *tagsEdit = new QLineEdit(dialog);
    tagsEdit->setObjectName("TipTags");
    tagsEdit->setPlaceholderText(i18nc("@info:placeholder", "Separated by commas"));
    tagsEdit->setToolTip(i18n("Tags the tip gets in Krita's brush tip library. A tag that does not exist is made."));
    form->addRow(i18nc("@label:textbox brush tip tags", "Tags:"), tagsEdit);
    if (!knownTags.isEmpty()) {
        // completes the tag being typed: the text after the last comma
        QCompleter *completer = new QCompleter(knownTags, tagsEdit);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setWidget(tagsEdit);
        QObject::connect(tagsEdit, &QLineEdit::textEdited, completer, [completer](const QString &text) {
            const QString typed = text.mid(text.lastIndexOf(',') + 1).trimmed();
            completer->setCompletionPrefix(typed);
            if (typed.isEmpty() || completer->completionCount() == 0) {
                completer->popup()->hide();
            } else {
                completer->complete();
            }
        });
        QObject::connect(completer, QOverload<const QString &>::of(&QCompleter::activated), tagsEdit,
                         [tagsEdit](const QString &tag) {
            const QString text = tagsEdit->text();
            const int comma = text.lastIndexOf(',');
            tagsEdit->setText((comma < 0 ? QString() : text.left(comma + 1) + ' ') + tag);
        });
    }

    QRadioButton *gray = new QRadioButton(i18nc("@option:radio", "Black and white"), dialog);
    gray->setObjectName("TipGray");
    gray->setToolTip(i18n("The tip is made gray. The brush paints its dark parts with the main color and its "
                          "light parts with the sub color."));
    QRadioButton *color = new QRadioButton(i18nc("@option:radio", "Color"), dialog);
    color->setObjectName("TipColor");
    color->setToolTip(i18n("The tip keeps the colors of the image, and the brush paints with them."));
    gray->setChecked(true);
    QHBoxLayout *kindRow = new QHBoxLayout();
    kindRow->addWidget(gray);
    kindRow->addWidget(color);
    kindRow->addStretch();
    form->addRow(i18nc("@label brush tip kind", "Tip:"), kindRow);
    top->addLayout(form, 1);
    layout->addLayout(top);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Create"));
    layout->addWidget(buttons);

    // the source's picture, and the name it offered (kept apart from a name the user typed)
    auto grabbed = std::make_shared<Grabbed>();
    auto showPreview = [=]() {
        const QImage tip = toTip(grabbed->image, gray->isChecked());
        if (tip.isNull()) {
            preview->setPixmap(QPixmap());
            preview->setText(i18nc("@info empty image", "Empty"));
        } else {
            const int side = preview->width() - 2 * preview->frameWidth();
            preview->setPixmap(QPixmap::fromImage(tip.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!tip.isNull());
    };
    auto load = [=]() {
        const bool nameIsOffered = name->text().isEmpty() || name->text() == grabbed->name;
        *grabbed = grab(Source(sourceBox->currentData().toInt()));
        if (grabbed->name.isEmpty()) grabbed->name = i18nc("default name of a new brush tip", "Tip");
        if (nameIsOffered) name->setText(grabbed->name);
        showPreview();
    };

    QObject::connect(sourceBox, QOverload<int>::of(&QComboBox::activated), dialog, [=](int) { load(); });
    QObject::connect(reload, &QPushButton::clicked, dialog, load);
    QObject::connect(gray, &QRadioButton::toggled, dialog, [=](bool) { showPreview(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=]() {
        const QImage tip = toTip(grabbed->image, gray->isChecked());
        if (tip.isNull()) return;
        result->image = tip;
        result->name = name->text().trimmed().isEmpty() ? grabbed->name : name->text().trimmed();
        if (tags) *tags = CspTipImport::parseTags(tagsEdit->text());
        dialog->accept();
    });

    load();
    return dialog;
}

bool CspTipCreator::create(QWidget *parent, CspTipImport::Tip *result, QStringList *tags)
{
    QStringList knownTags;
    if (KisResourceCacheDb::isValid()) {
        KisTagModel tagModel(ResourceType::Brushes);
        for (int row = 0; row < tagModel.rowCount(); row++) {
            KisTagSP tag = tagModel.tagForIndex(tagModel.index(row, 0));
            if (tag && tag->id() >= 0) knownTags << tag->name();
        }
    }
    std::unique_ptr<QDialog> d(dialog(parent, &CspTipCreator::grab, result, tags, knownTags));
    return d->exec() == QDialog::Accepted;
}
