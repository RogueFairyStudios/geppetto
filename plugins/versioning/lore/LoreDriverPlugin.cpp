/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreDriverPlugin.h"

#include <kpluginfactory.h>
#include <klocalizedstring.h>

#include <KisVersioningDriver.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningTranslations.h>

#include "LoreDriver.h"

K_PLUGIN_FACTORY_WITH_JSON(LoreDriverPluginFactory, "kritaversioninglore.json",
                           registerPlugin<LoreDriverPlugin>();)

namespace {

class LoreDriverFactory : public KisVersioningDriverFactory
{
public:
    QString id() const override
    {
        return QStringLiteral("lore");
    }

    QString name() const override
    {
        return i18n("Lore server");
    }

    KisVersioningDriver *create(QObject *parent) const override
    {
        return new LoreDriver(parent);
    }
};

} // namespace

LoreDriverPlugin::LoreDriverPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KisVersioning::registerTranslations();
    // The registry owns the factory.
    KisVersioningDriverRegistry::instance()->add(new LoreDriverFactory());
}

LoreDriverPlugin::~LoreDriverPlugin() = default;

#include "LoreDriverPlugin.moc"
