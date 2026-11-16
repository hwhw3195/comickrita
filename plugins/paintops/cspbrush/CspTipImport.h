/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_TIP_IMPORT_H
#define CSP_TIP_IMPORT_H

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <kis_brush.h>

#include "kritacspbrushpaintop_export.h"

/**
 * Krita brush tips (brush resources) -> CSP Brush tips, for "Add images":
 * every frame of an animated brush (.gih) becomes a tip of its own.
 *
 * The CSP Brush paints where a tip is dark and opaque (gray tips: black
 * main color, white sub color). Krita's mask brushes are opaque gray images
 * where white means no paint, so they become black with alpha = 255 - gray.
 * Tips written by sut2kpp (PNG text "csp_tip") are taken as they are, so
 * a two-color CSP tip keeps its white (sub color) part.
 */
namespace CspTipImport
{
struct Tip {
    QString name;
    QImage image; ///< ARGB32
};

/// @p rawFile: the brush's file, when known (only read for PNG brushes)
KRITACSPBRUSHPAINTOP_EXPORT QVector<Tip> fromBrush(KisBrushSP brush, const QByteArray &rawFile = QByteArray());

/// A Krita mask (gray, white = no paint) as a CSP tip: black, alpha = 255 - gray (times the image's alpha)
KRITACSPBRUSHPAINTOP_EXPORT QImage maskToTip(const QImage &mask);

/// True for a PNG written by sut2kpp or by libraryPng() as a CSP tip
KRITACSPBRUSHPAINTOP_EXPORT bool isCspTipPng(const QByteArray &png);

/// @p tip as the PNG file of a brush tip of Krita's library: marked as a CSP tip, so "Add images" takes it back as it is
KRITACSPBRUSHPAINTOP_EXPORT QByteArray libraryPng(const Tip &tip);

/// @p text of a tags field (separated by commas) as tag names: trimmed, no empty ones, none twice
KRITACSPBRUSHPAINTOP_EXPORT QStringList parseTags(const QString &text);

/**
 * Adds @p tip to Krita's brush tip library (a PNG brush tip in the user's resource folder; a name
 * already taken gets a number) and gives it @p tags, which are made when they do not exist.
 * Null when there is no resource database or the tip could not be added.
 */
KRITACSPBRUSHPAINTOP_EXPORT KoResourceSP addToLibrary(const Tip &tip, const QStringList &tags = QStringList());
}

#endif // CSP_TIP_IMPORT_H
