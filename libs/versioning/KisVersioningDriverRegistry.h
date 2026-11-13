/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGDRIVERREGISTRY_H
#define KISVERSIONINGDRIVERREGISTRY_H

#include <KoGenericRegistry.h>

#include "KisVersioningDriver.h"
#include "kritaversioning_export.h"

/// Holds the driver factories that the plugins register. Owns the factories.
class KRITAVERSIONING_EXPORT KisVersioningDriverRegistry : public KoGenericRegistry<KisVersioningDriverFactory *>
{
public:
    KisVersioningDriverRegistry();
    ~KisVersioningDriverRegistry() override;

    /// The first call loads the plugins of service type Krita/VersioningDriver.
    static KisVersioningDriverRegistry *instance();

private:
    void init();
    Q_DISABLE_COPY(KisVersioningDriverRegistry)
};

#endif // KISVERSIONINGDRIVERREGISTRY_H
