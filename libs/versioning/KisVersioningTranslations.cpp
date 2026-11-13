/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningTranslations.h"

#include <QDir>
#include <QtGlobal>

#include <KoResourcePaths.h>
#include <klocalizedstring.h>

namespace KisVersioning {

void registerTranslations()
{
    // The same folder, and the same condition, as krita/main.cc uses for the "krita" domain.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) || defined Q_OS_WIN || defined Q_OS_MACOS
    static const bool registered = []() {
        const QString root = KoResourcePaths::getApplicationRoot();
        KLocalizedString::addDomainLocaleDir("geppetto", QDir(root + QStringLiteral("share/locale")).absolutePath());
        return true;
    }();
    Q_UNUSED(registered);
#endif
}

}
