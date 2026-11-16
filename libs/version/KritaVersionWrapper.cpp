/*
 *  SPDX-FileCopyrightText: 2015 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KritaVersionWrapper.h"

#include <kritaversion.h>
#include <kritagitversion.h>

QString KritaVersionWrapper::versionString(bool checkGit)
{
    QString kritaVersion = QStringLiteral(KRITA_VERSION_STRING);
    QString version = kritaVersion;

    if (checkGit) {
#ifdef KRITA_GIT_SHA1_STRING
        QString gitVersion = QStringLiteral(KRITA_GIT_SHA1_STRING);
        version = QStringLiteral("%1 (git %2)").arg(kritaVersion, gitVersion);
#endif
        // comicKrita: a build of the fork says so wherever the full version
        // is shown (About, the logs). The plain version, which is written
        // into files, stays Krita's.
        const QString release = QStringLiteral(COMICKRITA_RELEASE);
        version += release.isEmpty() ? QStringLiteral(" comicKrita") : QStringLiteral(" comicKrita %1").arg(release);
    }
    return version;
}

bool KritaVersionWrapper::isDevelopersBuild()
{
    // Qt6 is not considered stable yet, don't present it as such.
#if defined(KRITA_STABLE) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    return false;
#else
    return true;
#endif
}
