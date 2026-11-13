/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGTRANSLATIONS_H
#define KISVERSIONINGTRANSLATIONS_H

#include "kritaversioning_export.h"

namespace KisVersioning {

/**
 * Makes the translations of the "geppetto" domain available. The application
 * registers its locale folder only for the "krita" domain; this registers the
 * same folder for ours. Plugins that show "geppetto" strings call it when they
 * load. Calling it more than once does nothing more.
 */
KRITAVERSIONING_EXPORT void registerTranslations();

}

#endif // KISVERSIONINGTRANSLATIONS_H
