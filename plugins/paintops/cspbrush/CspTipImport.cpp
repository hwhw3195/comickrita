/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspTipImport.h"

#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>

#include <KisResourceCacheDb.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTag.h>
#include <KisTagModel.h>
#include <KisTagResourceModel.h>

#include <kis_gbr_brush.h>
#include <kis_imagepipe_brush.h>

namespace {
// the raw image: KisColorfulBrush::brushTipImage() adjusts the contrast of image tips
QImage rawTipImage(const KisBrush *brush)
{
    return brush->KisBrush::brushTipImage();
}

QImage toTip(const KisBrush *brush)
{
    const QImage image = rawTipImage(brush);
    if (image.isNull()) return QImage();
    const bool mask = brush->brushType() == MASK || brush->brushType() == PIPE_MASK
        || image.format() == QImage::Format_Grayscale8 || image.format() == QImage::Format_Indexed8;
    return mask ? CspTipImport::maskToTip(image) : image.convertToFormat(QImage::Format_ARGB32);
}
}

bool CspTipImport::isCspTipPng(const QByteArray &png)
{
    QByteArray bytes = png;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    return reader.canRead() && reader.textKeys().contains("csp_tip");
}

QImage CspTipImport::maskToTip(const QImage &mask)
{
    const QImage source = mask.convertToFormat(QImage::Format_ARGB32);
    QImage tip(source.size(), QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); y++) {
        const QRgb *in = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(tip.scanLine(y));
        for (int x = 0; x < source.width(); x++) {
            out[x] = qRgba(0, 0, 0, (255 - qGray(in[x])) * qAlpha(in[x]) / 255);
        }
    }
    return tip;
}

QVector<CspTipImport::Tip> CspTipImport::fromBrush(KisBrushSP brush, const QByteArray &rawFile)
{
    QVector<Tip> tips;
    if (!brush) return tips;

    if (const KisImagePipeBrush *pipe = dynamic_cast<const KisImagePipeBrush *>(brush.data())) {
        const QVector<KisGbrBrushSP> frames = pipe->brushes();
        for (int i = 0; i < frames.size(); i++) {
            const QImage image = toTip(frames[i].data());
            if (!image.isNull()) tips << Tip{QString("%1 %2").arg(brush->name()).arg(i + 1), image};
        }
        return tips;
    }

    if (!rawFile.isEmpty() && isCspTipPng(rawFile)) {
        const QImage image = QImage::fromData(rawFile, "PNG").convertToFormat(QImage::Format_ARGB32);
        if (!image.isNull()) tips << Tip{brush->name(), image};
        return tips;
    }

    const QImage image = toTip(brush.data());
    if (!image.isNull()) tips << Tip{brush->name(), image};
    return tips;
}

QByteArray CspTipImport::libraryPng(const Tip &tip)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "PNG");
    writer.setText("csp_tip", "1");
    if (!tip.name.isEmpty()) writer.setText("brush_name", tip.name);
    if (!writer.write(tip.image.convertToFormat(QImage::Format_ARGB32))) return QByteArray();
    return png;
}

QStringList CspTipImport::parseTags(const QString &text)
{
    QStringList tags;
    for (const QString &part : text.split(',')) {
        const QString tag = part.simplified();
        if (!tag.isEmpty() && !tags.contains(tag, Qt::CaseInsensitive)) tags << tag;
    }
    return tags;
}

KoResourceSP CspTipImport::addToLibrary(const Tip &tip, const QStringList &tags)
{
    if (!KisResourceCacheDb::isValid() || tip.image.isNull()) return nullptr;
    QByteArray png = libraryPng(tip);
    if (png.isEmpty()) return nullptr;

    // a file name from the tip's name, one the library does not have yet
    QString base;
    for (const QChar c : tip.name) {
        base += (c.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(c)) ? QChar('_') : c;
    }
    base = base.simplified();
    if (base.isEmpty()) base = QStringLiteral("tip");
    KisResourceModel model(ResourceType::Brushes);
    QString file = base + ".png";
    for (int n = 2; model.importWillOverwriteResource(file); n++) file = QString("%1_%2.png").arg(base).arg(n);

    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    KoResourceSP resource = model.importResource(file, &buffer, false);
    if (!resource) return nullptr;

    if (!tags.isEmpty()) {
        KisTagModel tagModel(ResourceType::Brushes);
        KisTagResourceModel tagResourceModel(ResourceType::Brushes);
        for (const QString &name : tags) {
            // an existing tag of that name, whatever its case, else a new one
            KisTagSP tag;
            for (int row = 0; row < tagModel.rowCount() && !tag; row++) {
                KisTagSP candidate = tagModel.tagForIndex(tagModel.index(row, 0));
                if (candidate && candidate->id() >= 0 && candidate->name().compare(name, Qt::CaseInsensitive) == 0) tag = candidate;
            }
            if (!tag) tag = tagModel.addTag(name, false, QVector<KoResourceSP>());
            if (tag) tagResourceModel.tagResources(tag, QVector<int>() << resource->resourceId());
        }
    }
    return resource;
}
