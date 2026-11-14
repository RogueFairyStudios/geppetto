/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGTRANSLATIONS_H
#define KISVERSIONINGTRANSLATIONS_H

#include <QString>
#include <QStringList>

#include "kritaversioning_export.h"

namespace KisVersioning {

/**
 * Makes the translations of the "geppetto" domain available. The application
 * registers its locale folder only for the "krita" domain; this registers the
 * same folder for ours. Plugins that show "geppetto" strings call it when they
 * load. Calling it more than once does nothing more.
 */
KRITAVERSIONING_EXPORT void registerTranslations();

/**
 * The locale folders under @p applicationRoot (which ends with '/') that hold a
 * "geppetto" catalogue: bin/locale, where the installed package keeps it, and
 * share/locale, where a build puts it. When neither holds one, share/locale.
 */
KRITAVERSIONING_EXPORT QStringList translationsFolders(const QString &applicationRoot);

/// Registers translationsFolders(@p applicationRoot) for the "geppetto" domain.
KRITAVERSIONING_EXPORT void registerTranslationsIn(const QString &applicationRoot);

}

#endif // KISVERSIONINGTRANSLATIONS_H
