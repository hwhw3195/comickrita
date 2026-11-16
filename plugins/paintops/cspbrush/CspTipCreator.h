/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_TIP_CREATOR_H
#define CSP_TIP_CREATOR_H

#include <QImage>
#include <QRect>
#include <QString>
#include <functional>

#include <kis_types.h>

#include "CspTipImport.h"
#include "kritacspbrushpaintop_export.h"

class QDialog;
class QWidget;

/**
 * "+Create New" on the Brush tip page: a brush tip made from the open image,
 * like the Brush tip creator of Krita's pixel engine (KisCustomBrushWidget),
 * with these settings: Source, Name, Tags, and Black and white or Color.
 * The Brush tip page puts the tip into the brush and into Krita's brush tip
 * library (CspTipImport::addToLibrary()), under the tags.
 *
 * Unlike Krita's creator the tip always keeps the alpha of its source.
 * Black and white makes the tip gray, which the engine paints with the main
 * and sub colors; Color keeps the image's own colors.
 */
namespace CspTipCreator
{
enum Source {
    SelectedLayers = 0, ///< the layers selected in the Layers docker, flattened
    WholeImage,         ///< everything visible
    Clipboard
};

/// What a source gave: the picture (ARGB32, cut to its content; null when there is none) and a name to offer
struct Grabbed {
    QImage image;
    QString name;
};

/// @p nodes of @p image painted over each other, bottom first, on one raster device
KRITACSPBRUSHPAINTOP_EXPORT KisPaintDeviceSP flattenLayers(KisImageSP image, const KisNodeList &nodes);

/**
 * The pixels of @p device inside @p limit (a null rect: no limit) as ARGB32, masked by
 * @p selection when there is one, and cut to the pixels that are not transparent.
 */
KRITACSPBRUSHPAINTOP_EXPORT QImage deviceToImage(KisPaintDeviceSP device, const QRect &limit, KisSelectionSP selection);

/// The tip of @p image: its alpha is kept; @p gray replaces each color by its gray value
KRITACSPBRUSHPAINTOP_EXPORT QImage toTip(const QImage &image, bool gray);

/// The source's picture, from the active window (or the clipboard)
KRITACSPBRUSHPAINTOP_EXPORT Grabbed grab(Source source);

/**
 * The dialog. @p grab gives the picture of a Source (a parameter so that tests need no window);
 * accepting it writes the tip to @p result and the tags typed for it to @p tags (when given).
 * The caller runs and deletes it. @p knownTags are offered while a tag is typed.
 */
KRITACSPBRUSHPAINTOP_EXPORT QDialog *dialog(QWidget *parent, const std::function<Grabbed(Source)> &grab,
                                            CspTipImport::Tip *result, QStringList *tags = nullptr,
                                            const QStringList &knownTags = QStringList());

/// Runs the dialog on the active window's image, offering the library's brush tip tags; false when cancelled
KRITACSPBRUSHPAINTOP_EXPORT bool create(QWidget *parent, CspTipImport::Tip *result, QStringList *tags = nullptr);
}

#endif // CSP_TIP_CREATOR_H
